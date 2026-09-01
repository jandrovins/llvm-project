//===- llvm-inputgen-gpu.cpp - Launch InputGen GPU entry kernels ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Launch a prebuilt direct-entry InputGen GPU image through libomptarget.
// compiler-rt's host codec owns the factory and input-record representations;
// this tool owns command-line policy, device-image registration, and launch.
//
//===----------------------------------------------------------------------===//

#include "omptarget.h"

#include "inputgen_gpu/InputGenGPUCodec.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Frontend/Offloading/Utility.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <cinttypes>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

using namespace llvm;

#define TOOL_NAME "llvm-inputgen-gpu"
#ifndef OMP_KERNEL_ARG_VERSION
#define OMP_KERNEL_ARG_VERSION 5
#endif

namespace {

constexpr uint64_t DefaultSliceBytes = 16384;
constexpr uint64_t DefaultObjectBytes = 1024;
constexpr StringLiteral InputGenGPUEntryPointName = "__ig_entry";

// Holds one parsed launcher invocation.  The codec validates and materializes
// the factory-related fields; DeviceId remains launcher-only state.
struct InputGenInvocation {
  inputgen_gpu::Mode Mode;
  std::string DataFilename;
  int32_t DeviceId;
  inputgen_gpu::FactoryConfig FactoryConfig;
  inputgen_gpu::ReplayRequest ReplayRequest;
};

// Owns the launcher-allocated device factory and releases it on destruction.
class DeviceAllocation {
public:
  DeviceAllocation(void *Factory, int32_t DeviceId)
      : Factory(Factory), DeviceId(DeviceId) {}
  DeviceAllocation(const DeviceAllocation &) = delete;
  DeviceAllocation &operator=(const DeviceAllocation &) = delete;
  DeviceAllocation(DeviceAllocation &&Other)
      : Factory(Other.Factory), DeviceId(Other.DeviceId) {
    Other.Factory = nullptr;
  }
  DeviceAllocation &operator=(DeviceAllocation &&) = delete;
  ~DeviceAllocation() {
    if (Factory)
      omp_target_free(Factory, DeviceId);
  }

  void *Factory = nullptr;
  int32_t DeviceId = 0;
};

// Owns the arrays backing the entry kernel's single opaque context argument.
struct KernelLaunchArguments {
  void *ArgBasePtrs[1] = {};
  void *ArgPtrs[1] = {};
  int64_t ArgSizes[1] = {};
  int64_t ArgTypes[1] = {};
  KernelArgsTy KernelArgs{};
};

cl::OptionCategory InputGenGPUCategory(TOOL_NAME " Options");

cl::opt<std::string> ModeArg(cl::Positional, cl::desc("<generate|replay>"),
                             cl::Required, cl::cat(InputGenGPUCategory));
cl::opt<std::string> ImageFilename(cl::Positional, cl::desc("<image>"),
                                   cl::Required, cl::cat(InputGenGPUCategory));
cl::opt<std::string> InputGenDataFilename(
    "inputgen-data",
    cl::desc("Binary file used to write generated input and read replay input"),
    cl::init(""), cl::cat(InputGenGPUCategory));
cl::opt<int32_t> DeviceIdOpt("device-id", cl::desc("Select the target device"),
                             cl::init(0), cl::cat(InputGenGPUCategory));
cl::opt<uint32_t>
    NumTeamsOpt("num-teams",
                cl::desc("Override the default one-team launch geometry"),
                cl::init(0), cl::cat(InputGenGPUCategory));
cl::opt<uint32_t>
    NumThreadsOpt("num-threads",
                  cl::desc("Override the default one-thread launch geometry"),
                  cl::init(0), cl::cat(InputGenGPUCategory));
cl::opt<uint64_t> FactoryBytesOpt(
    "factory-bytes-per-thread",
    cl::desc(
        "Bytes reserved by the driver for each GPU thread's factory slice"),
    cl::init(DefaultSliceBytes), cl::cat(InputGenGPUCategory));
cl::opt<uint64_t> ObjectBytesOpt(
    "object-bytes",
    cl::desc("Fixed data capacity for each lazily allocated object"),
    cl::init(DefaultObjectBytes), cl::cat(InputGenGPUCategory));
cl::opt<uint32_t> ObjectsPerThreadOpt(
    "objects-per-thread",
    cl::desc("Total object capacity per GPU thread, including the argument "
             "object"),
    cl::init(4), cl::cat(InputGenGPUCategory));

template <typename... ArgsTy>
Error createErr(const char *ErrFmt, ArgsTy &&...Args) {
  return createStringError(inconvertibleErrorCode(), ErrFmt,
                           std::forward<ArgsTy>(Args)...);
}

Error convertError(const inputgen_gpu::Error &Failure) {
  return createErr("%s", Failure.message().c_str());
}

template <typename T>
Error convertResultError(const inputgen_gpu::Result<T> &Failure) {
  return convertError(Failure.error());
}

std::string getDefaultInputGenDataFilename(StringRef ImagePath) {
  SmallString<256> Path(ImagePath);
  sys::path::replace_extension(Path, "inputgen");
  return std::string(Path);
}

Expected<inputgen_gpu::Mode> parseMode() {
  if (ModeArg == "generate")
    return inputgen_gpu::Mode::Generate;
  if (ModeArg == "replay")
    return inputgen_gpu::Mode::Replay;
  return createErr("invalid mode '%s'; expected 'generate' or 'replay'",
                   ModeArg.c_str());
}

Expected<InputGenInvocation> parseInvocation() {
  Expected<inputgen_gpu::Mode> ModeOrErr = parseMode();
  if (!ModeOrErr)
    return ModeOrErr.takeError();

  InputGenInvocation Invocation{
      *ModeOrErr,
      InputGenDataFilename.empty()
          ? getDefaultInputGenDataFilename(ImageFilename)
          : InputGenDataFilename.getValue(),
      DeviceIdOpt,
      {NumTeamsOpt > 0 ? NumTeamsOpt.getValue() : 1,
       NumThreadsOpt > 0 ? NumThreadsOpt.getValue() : 1, FactoryBytesOpt,
       ObjectBytesOpt, ObjectsPerThreadOpt},
      {},
  };
  if (NumTeamsOpt.getNumOccurrences())
    Invocation.ReplayRequest.NumTeams = NumTeamsOpt;
  if (NumThreadsOpt.getNumOccurrences())
    Invocation.ReplayRequest.NumThreads = NumThreadsOpt;
  if (FactoryBytesOpt.getNumOccurrences())
    Invocation.ReplayRequest.SliceBytes = FactoryBytesOpt;
  if (ObjectBytesOpt.getNumOccurrences())
    Invocation.ReplayRequest.ObjectBytes = ObjectBytesOpt;
  if (ObjectsPerThreadOpt.getNumOccurrences())
    Invocation.ReplayRequest.ObjectsPerThread = ObjectsPerThreadOpt;
  return Invocation;
}

Expected<std::unique_ptr<MemoryBuffer>> loadDeviceImage() {
  auto ImageBufferOrErr = MemoryBuffer::getFile(
      ImageFilename, /*IsText=*/false, /*RequiresNullTerminator=*/false);
  if (!ImageBufferOrErr)
    return createErr("failed to read image file '%s'", ImageFilename.c_str());
  return std::move(*ImageBufferOrErr);
}

void buildOffloadEntries(
    SmallVectorImpl<llvm::offloading::EntryTy> &OffloadEntries) {
  OffloadEntries.assign(
      1, llvm::offloading::EntryTy{0x0, 0x1, object::OffloadKind::OFK_OpenMP, 0,
                                   nullptr, nullptr, 0, 0, nullptr});
  OffloadEntries[0].SymbolName =
      const_cast<char *>(InputGenGPUEntryPointName.data());
  OffloadEntries[0].Address = reinterpret_cast<void *>(0x1);
}

void registerDeviceImage(
    MemoryBuffer &ImageBuffer,
    SmallVectorImpl<llvm::offloading::EntryTy> &OffloadEntries,
    __tgt_device_image &DeviceImage, __tgt_bin_desc &Desc) {
  DeviceImage.ImageStart = const_cast<char *>(ImageBuffer.getBufferStart());
  DeviceImage.ImageEnd = const_cast<char *>(ImageBuffer.getBufferEnd());
  DeviceImage.EntriesBegin = &OffloadEntries[0];
  DeviceImage.EntriesEnd = OffloadEntries.data() + OffloadEntries.size();
  Desc.NumDeviceImages = 1;
  Desc.DeviceImages = &DeviceImage;
  Desc.HostEntriesBegin = &OffloadEntries[0];
  Desc.HostEntriesEnd = OffloadEntries.data() + OffloadEntries.size();
  __tgt_register_lib(&Desc);
}

Expected<DeviceAllocation> allocateFactory(int32_t DeviceId, uint64_t Size) {
  void *Factory = omp_target_alloc(Size, DeviceId);
  if (!Factory)
    return createErr("omp_target_alloc failed on device %" PRId32, DeviceId);
  return DeviceAllocation(Factory, DeviceId);
}

Error copyToDevice(DeviceAllocation &Allocation,
                   const inputgen_gpu::Factory &Factory, int HostDevice) {
  if (omp_target_memcpy(Allocation.Factory, Factory.data(), Factory.size(), 0,
                        0, Allocation.DeviceId, HostDevice) != 0)
    return createErr("failed to copy factory to device");
  return Error::success();
}

void buildKernelLaunchArguments(const InputGenInvocation &Invocation,
                                DeviceAllocation &Allocation,
                                KernelLaunchArguments &Args) {
  Args.ArgBasePtrs[0] = &Allocation.Factory;
  Args.ArgPtrs[0] = &Allocation.Factory;
  Args.ArgSizes[0] = sizeof(Allocation.Factory);
  Args.ArgTypes[0] = OMP_TGT_MAPTYPE_TARGET_PARAM | OMP_TGT_MAPTYPE_LITERAL;
  Args.KernelArgs.Version = OMP_KERNEL_ARG_VERSION;
  Args.KernelArgs.NumArgs = 1;
  Args.KernelArgs.ArgBasePtrs = Args.ArgBasePtrs;
  Args.KernelArgs.ArgPtrs = Args.ArgPtrs;
  Args.KernelArgs.ArgSizes = Args.ArgSizes;
  Args.KernelArgs.ArgTypes = Args.ArgTypes;
  Args.KernelArgs.Flags.IsPtrArgs = 1;
  Args.KernelArgs.Flags.StrictBlocksAndThreads = 1;
  Args.KernelArgs.UserNumBlocks[0] = Invocation.FactoryConfig.NumTeams;
  Args.KernelArgs.UserNumBlocks[1] = 1;
  Args.KernelArgs.UserNumBlocks[2] = 1;
  Args.KernelArgs.UserThreadLimit[0] = Invocation.FactoryConfig.NumThreads;
  Args.KernelArgs.UserThreadLimit[1] = 1;
  Args.KernelArgs.UserThreadLimit[2] = 1;
}

Error launchEntryKernel(const InputGenInvocation &Invocation,
                        llvm::offloading::EntryTy &Entry,
                        KernelLaunchArguments &Args) {
  if (__tgt_target_kernel(nullptr, Invocation.DeviceId,
                          Invocation.FactoryConfig.NumTeams,
                          Invocation.FactoryConfig.NumThreads, Entry.Address,
                          &Args.KernelArgs) != OMP_TGT_SUCCESS)
    return createErr("failed to launch entry kernel '%s'",
                     InputGenGPUEntryPointName.data());
  return Error::success();
}

Error reportResults(const InputGenInvocation &Invocation,
                    const inputgen_gpu::Factory &Factory) {
  auto Results = inputgen_gpu::inspectThreadResults(Factory);
  if (!Results)
    return convertResultError(Results);
  for (const inputgen_gpu::ThreadResult &Result : Results.value()) {
    if (Result.ErrorCode)
      return createErr(
          "device GPU thread %u reported InputGen factory error %u",
          Result.ThreadIndex, Result.ErrorCode);
    if (!Result.ResultSize)
      continue;
    outs() << (Invocation.Mode == inputgen_gpu::Mode::Generate
                   ? "result"
                   : "replay result")
           << "[" << Result.ThreadIndex << "] = " << Result.ResultBits << "\n";
  }
  return Error::success();
}

Error runInputGenGPU() {
  Expected<InputGenInvocation> InvocationOrErr = parseInvocation();
  if (!InvocationOrErr)
    return InvocationOrErr.takeError();
  InputGenInvocation Invocation = std::move(*InvocationOrErr);

  auto CreateFactory = [&]() -> inputgen_gpu::Result<inputgen_gpu::Factory> {
    if (Invocation.Mode == inputgen_gpu::Mode::Generate)
      return inputgen_gpu::createGenerationFactory(Invocation.FactoryConfig);
    return inputgen_gpu::createReplayFactory(Invocation.DataFilename,
                                             Invocation.ReplayRequest);
  };
  inputgen_gpu::Result<inputgen_gpu::Factory> FactoryOrErr = CreateFactory();
  if (!FactoryOrErr)
    return convertResultError(FactoryOrErr);
  inputgen_gpu::Factory HostFactory = std::move(FactoryOrErr).value();
  Invocation.FactoryConfig = HostFactory.config();

  Expected<std::unique_ptr<MemoryBuffer>> ImageBufferOrErr = loadDeviceImage();
  if (!ImageBufferOrErr)
    return ImageBufferOrErr.takeError();
  std::unique_ptr<MemoryBuffer> ImageBuffer = std::move(*ImageBufferOrErr);

  SmallVector<llvm::offloading::EntryTy> OffloadEntries;
  buildOffloadEntries(OffloadEntries);
  __tgt_device_image DeviceImage;
  __tgt_bin_desc Desc;
  registerDeviceImage(*ImageBuffer, OffloadEntries, DeviceImage, Desc);

  int NumDevices = omp_get_num_devices();
  if (NumDevices <= 0)
    return createErr("no OpenMP target devices are available");
  if (Invocation.DeviceId < 0 || Invocation.DeviceId >= NumDevices)
    return createErr("requested device %" PRId32
                     " is outside the available device range [0, %d)",
                     Invocation.DeviceId, NumDevices);

  int HostDevice = omp_get_initial_device();
  Expected<DeviceAllocation> AllocationOrErr =
      allocateFactory(Invocation.DeviceId, HostFactory.size());
  if (!AllocationOrErr)
    return AllocationOrErr.takeError();
  DeviceAllocation Allocation = std::move(*AllocationOrErr);
  if (Error Err = copyToDevice(Allocation, HostFactory, HostDevice))
    return Err;

  KernelLaunchArguments LaunchArgs;
  buildKernelLaunchArguments(Invocation, Allocation, LaunchArgs);
  if (Error Err = launchEntryKernel(Invocation, OffloadEntries[0], LaunchArgs))
    return Err;

  if (omp_target_memcpy(HostFactory.data(), Allocation.Factory,
                        HostFactory.size(), 0, 0, HostDevice,
                        Invocation.DeviceId) != 0)
    return createErr("failed to copy factory from device");

  if (Invocation.Mode == inputgen_gpu::Mode::Generate) {
    if (inputgen_gpu::Error Failure = inputgen_gpu::writeGenerationRecord(
            Invocation.DataFilename, HostFactory))
      return convertError(Failure);
    outs() << "serialized input = " << Invocation.DataFilename << "\n";
  }
  return reportResults(Invocation, HostFactory);
}

} // namespace

int main(int Argc, char **Argv) {
  cl::HideUnrelatedOptions(InputGenGPUCategory);
  cl::ParseCommandLineOptions(
      Argc, Argv,
      "Launch an InputGen GPU entry kernel from a device image\n");

  if (Error Err = runInputGenGPU()) {
    WithColor::error(errs(), TOOL_NAME) << toString(std::move(Err)) << "\n";
    return 1;
  }
  return 0;
}
