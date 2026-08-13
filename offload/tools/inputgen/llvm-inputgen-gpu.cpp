//===- llvm-inputgen-gpu.cpp - Launch InputGen GPU entry kernels ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Prototype InputGen GPU launcher. This consumes a prebuilt device image and a
// record JSON, derives the generated InputGen entry kernel name from the JSON
// kernel name, and launches the entry kernel through libomptarget.
//
//===----------------------------------------------------------------------===//

#include "omptarget.h"

#include "InputGenInterface.hpp"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Frontend/Offloading/Utility.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/WithColor.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

using namespace llvm;

#define TOOL_NAME "llvm-inputgen-gpu"
#define TOOL_PREFIX "[" TOOL_NAME "]"
#ifndef OMP_KERNEL_ARG_VERSION
#define OMP_KERNEL_ARG_VERSION 5
#endif

namespace {

enum class InputGenMode : int32_t {
  Generate = INPUTGEN_MODE_GENERATE,
  Replay = INPUTGEN_MODE_REPLAY,
};

struct InputGenInvocation {
  InputGenMode Mode;
  std::string EntryName;
  std::string DataFilename;
  int32_t DeviceId;
  uint32_t NumTeams;
  uint32_t NumThreads;
};

class DeviceAllocations {
public:
  DeviceAllocations(void *Buffer, void *Result, int32_t DeviceId)
      : Buffer(Buffer), Result(Result), DeviceId(DeviceId) {}

  DeviceAllocations(const DeviceAllocations &) = delete;
  DeviceAllocations &operator=(const DeviceAllocations &) = delete;

  DeviceAllocations(DeviceAllocations &&Other)
      : Buffer(Other.Buffer), Result(Other.Result), DeviceId(Other.DeviceId) {
    Other.Buffer = nullptr;
    Other.Result = nullptr;
  }

  DeviceAllocations &operator=(DeviceAllocations &&Other) = delete;

  ~DeviceAllocations() {
    if (Buffer)
      omp_target_free(Buffer, DeviceId);
    if (Result)
      omp_target_free(Result, DeviceId);
  }

  void *Buffer = nullptr;
  void *Result = nullptr;
  int32_t DeviceId = 0;
};

struct KernelLaunchArguments {
  int32_t Mode = 0;
  int64_t BufferSize = sizeof(int32_t);
  void *ArgBasePtrs[4] = {};
  void *ArgPtrs[4] = {};
  int64_t ArgSizes[4] = {};
  int64_t ArgTypes[4] = {};
  KernelArgsTy KernelArgs{};
};

cl::OptionCategory InputGenGPUCategory(TOOL_NAME " Options");

cl::opt<std::string> ModeArg(cl::Positional, cl::desc("<generate|replay>"),
                             cl::Required, cl::cat(InputGenGPUCategory));

cl::opt<std::string> ImageFilename(cl::Positional, cl::desc("<image>"),
                                   cl::Required, cl::cat(InputGenGPUCategory));

cl::opt<std::string> JsonFilename(cl::Positional, cl::desc("<record.json>"),
                                  cl::Required, cl::cat(InputGenGPUCategory));

cl::opt<std::string> InputGenDataFilename(
    "inputgen-data",
    cl::desc("Text file used to write generated values and read replay values"),
    cl::init(""), cl::cat(InputGenGPUCategory));

cl::opt<int32_t> DeviceIdOpt("device-id", cl::desc("Override JSON DeviceId"),
                             cl::init(-1), cl::cat(InputGenGPUCategory));

cl::opt<uint32_t>
    NumTeamsOpt("num-teams",
                cl::desc("Override the default one-team launch geometry"),
                cl::init(0), cl::cat(InputGenGPUCategory));

cl::opt<uint32_t>
    NumThreadsOpt("num-threads",
                  cl::desc("Override the default one-thread launch geometry"),
                  cl::init(0), cl::cat(InputGenGPUCategory));

template <typename... ArgsTy>
Error createErr(const char *ErrFmt, ArgsTy &&...Args) {
  return createStringError(inconvertibleErrorCode(), ErrFmt,
                           std::forward<ArgsTy>(Args)...);
}

template <typename T>
Error getInteger(const json::Object *Obj, StringRef Key, T &Result) {
  std::optional<int64_t> OptInt = Obj->getInteger(Key);
  if (!OptInt)
    return createErr("failed to read JSON integer %s", Key.data());
  Result = static_cast<T>(*OptInt);
  return Error::success();
}

Error getString(const json::Object *Obj, StringRef Key, StringRef &Result) {
  std::optional<StringRef> OptStr = Obj->getString(Key);
  if (!OptStr)
    return createErr("failed to read JSON string %s", Key.data());
  Result = *OptStr;
  return Error::success();
}

std::string getDefaultInputGenDataFilename(StringRef JsonPath) {
  SmallString<256> Path(JsonPath);
  sys::path::replace_extension(Path, "inputgen");
  return std::string(Path);
}

Error readReplayValue(StringRef Filename, int32_t &Value) {
  FILE *File = std::fopen(Filename.str().c_str(), "r");
  if (!File)
    return createErr("failed to open replay data file '%s'", Filename.data());

  int Scanned = std::fscanf(File, "%" SCNd32, &Value);
  std::fclose(File);
  if (Scanned != 1)
    return createErr("failed to read replay value from '%s'", Filename.data());

  return Error::success();
}

Error writeGeneratedValue(StringRef Filename, int32_t Value) {
  FILE *File = std::fopen(Filename.str().c_str(), "w");
  if (!File)
    return createErr("failed to open generated data file '%s'",
                     Filename.data());

  if (std::fprintf(File, "%" PRId32 "\n", Value) < 0) {
    std::fclose(File);
    return createErr("failed to write generated value to '%s'",
                     Filename.data());
  }

  std::fclose(File);
  return Error::success();
}

Expected<InputGenMode> parseMode() {
  InputGenMode Mode;
  if (ModeArg == "generate")
    Mode = InputGenMode::Generate;
  else if (ModeArg == "replay")
    Mode = InputGenMode::Replay;
  else
    return createErr("invalid mode '%s'; expected 'generate' or 'replay'",
                     ModeArg.c_str());
  return Mode;
}

Expected<InputGenInvocation> parseInvocation() {
  // Parse the requested InputGen mode.
  Expected<InputGenMode> ModeOrErr = parseMode();
  if (!ModeOrErr)
    return ModeOrErr.takeError();

  // Load and parse the record JSON.
  auto JsonBufferOrErr = MemoryBuffer::getFile(JsonFilename, /*IsText=*/true,
                                               /*RequiresNullTerminator=*/true);
  if (!JsonBufferOrErr)
    return createErr("failed to read JSON file '%s'", JsonFilename.c_str());

  Expected<json::Value> JsonValueOrErr =
      json::parse(JsonBufferOrErr.get()->getBuffer());
  if (!JsonValueOrErr)
    return JsonValueOrErr.takeError();

  const json::Object *JsonObj = JsonValueOrErr->getAsObject();
  if (!JsonObj)
    return createErr("invalid JSON file '%s'", JsonFilename.c_str());

  // Read the fields the prototype entry launcher uses.
  StringRef RecordedKernelName;
  if (Error Err = getString(JsonObj, "Name", RecordedKernelName))
    return Err;

  int32_t JsonDeviceId = 0;
  if (Error Err = getInteger(JsonObj, "DeviceId", JsonDeviceId))
    return Err;

  // Derive the generated entry name and default data file.
  InputGenInvocation Invocation{
      *ModeOrErr,
      (Twine("__ig_entry_") + RecordedKernelName).str(),
      InputGenDataFilename.empty()
          ? getDefaultInputGenDataFilename(JsonFilename)
          : InputGenDataFilename.getValue(),
      DeviceIdOpt >= 0 ? DeviceIdOpt : JsonDeviceId,
      NumTeamsOpt > 0 ? static_cast<uint32_t>(NumTeamsOpt) : 1,
      NumThreadsOpt > 0 ? static_cast<uint32_t>(NumThreadsOpt) : 1,
  };
  return Invocation;
}

Expected<std::unique_ptr<MemoryBuffer>> loadDeviceImage() {
  // Load the prebuilt device image.
  auto ImageBufferOrErr = MemoryBuffer::getFile(
      ImageFilename, /*IsText=*/false, /*RequiresNullTerminator=*/false);
  if (!ImageBufferOrErr)
    return createErr("failed to read image file '%s'", ImageFilename.c_str());
  return std::move(*ImageBufferOrErr);
}

void buildOffloadEntries(
    const std::string &EntryName,
    SmallVectorImpl<llvm::offloading::EntryTy> &OffloadEntries) {
  // Build the offload entry for the generated entry kernel.
  OffloadEntries.assign(
      1, llvm::offloading::EntryTy{0x0, 0x1, object::OffloadKind::OFK_OpenMP, 0,
                                   nullptr, nullptr, 0, 0, nullptr});
  OffloadEntries[0].SymbolName = const_cast<char *>(EntryName.c_str());
  OffloadEntries[0].Address = reinterpret_cast<void *>(0x1);
}

void registerDeviceImage(
    MemoryBuffer &ImageBuffer,
    SmallVectorImpl<llvm::offloading::EntryTy> &OffloadEntries,
    __tgt_device_image &DeviceImage, __tgt_bin_desc &Desc) {
  // Build the device image structure from the image buffer and offload entry.
  DeviceImage.ImageStart = const_cast<char *>(ImageBuffer.getBufferStart());
  DeviceImage.ImageEnd = const_cast<char *>(ImageBuffer.getBufferEnd());
  DeviceImage.EntriesBegin = &OffloadEntries[0];
  DeviceImage.EntriesEnd = OffloadEntries.data() + OffloadEntries.size();

  // Build the binary descriptor structure from the device image and entry.
  Desc.NumDeviceImages = 1;
  Desc.DeviceImages = &DeviceImage;
  Desc.HostEntriesBegin = &OffloadEntries[0];
  Desc.HostEntriesEnd = OffloadEntries.data() + OffloadEntries.size();

  // Register the device image and offload entry with the OpenMP runtime.
  __tgt_register_lib(&Desc);
}

Expected<DeviceAllocations> allocateInputGenBuffers(int32_t DeviceId) {
  // Allocate InputGen buffers on the GPU.
  void *DeviceBuffer = omp_target_alloc(sizeof(int32_t), DeviceId);
  void *DeviceResult = omp_target_alloc(sizeof(int32_t), DeviceId);
  if (!DeviceBuffer || !DeviceResult) {
    if (DeviceBuffer)
      omp_target_free(DeviceBuffer, DeviceId);
    if (DeviceResult)
      omp_target_free(DeviceResult, DeviceId);
    return createErr("omp_target_alloc failed on device %" PRId32, DeviceId);
  }
  return DeviceAllocations(DeviceBuffer, DeviceResult, DeviceId);
}

Error initializeReplayBuffer(const InputGenInvocation &Invocation,
                             DeviceAllocations &Allocs, int HostDevice) {
  if (Invocation.Mode != InputGenMode::Replay)
    return Error::success();

  // Copy the replay value into the device buffer.
  int32_t ReplayValue = 0;
  if (Error Err = readReplayValue(Invocation.DataFilename, ReplayValue))
    return Err;

  if (omp_target_memcpy(Allocs.Buffer, &ReplayValue, sizeof(ReplayValue), 0, 0,
                        Invocation.DeviceId, HostDevice) != 0)
    return createErr("failed to copy replay value to device");

  return Error::success();
}

void buildKernelLaunchArguments(const InputGenInvocation &Invocation,
                                DeviceAllocations &Allocs,
                                KernelLaunchArguments &Args) {
  // Build arguments for the kernel launch.
  Args.Mode = static_cast<int32_t>(Invocation.Mode);
  Args.BufferSize = sizeof(int32_t);
  Args.ArgBasePtrs[0] = &Args.Mode;
  Args.ArgBasePtrs[1] = &Allocs.Buffer;
  Args.ArgBasePtrs[2] = &Args.BufferSize;
  Args.ArgBasePtrs[3] = &Allocs.Result;
  Args.ArgPtrs[0] = &Args.Mode;
  Args.ArgPtrs[1] = &Allocs.Buffer;
  Args.ArgPtrs[2] = &Args.BufferSize;
  Args.ArgPtrs[3] = &Allocs.Result;
  Args.ArgSizes[0] = sizeof(Args.Mode);
  Args.ArgSizes[1] = sizeof(Allocs.Buffer);
  Args.ArgSizes[2] = sizeof(Args.BufferSize);
  Args.ArgSizes[3] = sizeof(Allocs.Result);
  Args.ArgTypes[0] = OMP_TGT_MAPTYPE_TARGET_PARAM | OMP_TGT_MAPTYPE_LITERAL;
  Args.ArgTypes[1] = OMP_TGT_MAPTYPE_TARGET_PARAM | OMP_TGT_MAPTYPE_LITERAL;
  Args.ArgTypes[2] = OMP_TGT_MAPTYPE_TARGET_PARAM | OMP_TGT_MAPTYPE_LITERAL;
  Args.ArgTypes[3] = OMP_TGT_MAPTYPE_TARGET_PARAM | OMP_TGT_MAPTYPE_LITERAL;

  // Build the kernel argument structure.
  Args.KernelArgs.Version = OMP_KERNEL_ARG_VERSION;
  Args.KernelArgs.NumArgs = 4;
  Args.KernelArgs.ArgBasePtrs = Args.ArgBasePtrs;
  Args.KernelArgs.ArgPtrs = Args.ArgPtrs;
  Args.KernelArgs.ArgSizes = Args.ArgSizes;
  Args.KernelArgs.ArgTypes = Args.ArgTypes;
  Args.KernelArgs.Flags.IsPtrArgs = 1;
  Args.KernelArgs.Flags.StrictBlocksAndThreads = 1;
  Args.KernelArgs.UserNumBlocks[0] = Invocation.NumTeams;
  Args.KernelArgs.UserNumBlocks[1] = 1;
  Args.KernelArgs.UserNumBlocks[2] = 1;
  Args.KernelArgs.UserThreadLimit[0] = Invocation.NumThreads;
  Args.KernelArgs.UserThreadLimit[1] = 1;
  Args.KernelArgs.UserThreadLimit[2] = 1;
}

Error launchEntryKernel(const InputGenInvocation &Invocation,
                        llvm::offloading::EntryTy &Entry,
                        KernelLaunchArguments &Args) {
  // Launch the generated entry kernel.
  if (__tgt_target_kernel(nullptr, Invocation.DeviceId, Invocation.NumTeams,
                          Invocation.NumThreads, Entry.Address,
                          &Args.KernelArgs) != OMP_TGT_SUCCESS)
    return createErr("failed to launch entry kernel '%s'",
                     Invocation.EntryName.c_str());

  return Error::success();
}

Error copyAndHandleResults(const InputGenInvocation &Invocation,
                           DeviceAllocations &Allocs, int HostDevice) {
  // Copy results back from the device buffers.
  int32_t Result = 0;
  if (omp_target_memcpy(&Result, Allocs.Result, sizeof(Result), 0, 0,
                        HostDevice, Invocation.DeviceId) != 0)
    return createErr("failed to copy result from device");

  int32_t BufferValue = 0;
  if (omp_target_memcpy(&BufferValue, Allocs.Buffer, sizeof(BufferValue), 0, 0,
                        HostDevice, Invocation.DeviceId) != 0)
    return createErr("failed to copy InputGen buffer from device");

  outs() << "b = " << Result << "\n";
  if (Invocation.Mode == InputGenMode::Generate) {
    outs() << "generated value = " << BufferValue << "\n";
    if (Error Err = writeGeneratedValue(Invocation.DataFilename, BufferValue))
      return Err;
  } else {
    outs() << "replay value = " << BufferValue << "\n";
  }

  return Error::success();
}

Error runInputGenGPU() {
  Expected<InputGenInvocation> InvocationOrErr = parseInvocation();
  if (!InvocationOrErr)
    return InvocationOrErr.takeError();
  InputGenInvocation Invocation = std::move(*InvocationOrErr);

  Expected<std::unique_ptr<MemoryBuffer>> ImageBufferOrErr = loadDeviceImage();
  if (!ImageBufferOrErr)
    return ImageBufferOrErr.takeError();
  std::unique_ptr<MemoryBuffer> ImageBuffer = std::move(*ImageBufferOrErr);

  SmallVector<llvm::offloading::EntryTy> OffloadEntries;
  buildOffloadEntries(Invocation.EntryName, OffloadEntries);

  __tgt_device_image DeviceImage;
  __tgt_bin_desc Desc;
  registerDeviceImage(*ImageBuffer, OffloadEntries, DeviceImage, Desc);

  int HostDevice = omp_get_initial_device();
  Expected<DeviceAllocations> AllocsOrErr =
      allocateInputGenBuffers(Invocation.DeviceId);
  if (!AllocsOrErr)
    return AllocsOrErr.takeError();
  DeviceAllocations Allocs = std::move(*AllocsOrErr);

  if (Error Err = initializeReplayBuffer(Invocation, Allocs, HostDevice))
    return Err;

  KernelLaunchArguments LaunchArgs;
  buildKernelLaunchArguments(Invocation, Allocs, LaunchArgs);

  if (Error Err = launchEntryKernel(Invocation, OffloadEntries[0], LaunchArgs))
    return Err;

  return copyAndHandleResults(Invocation, Allocs, HostDevice);
}

} // namespace

int main(int Argc, char **Argv) {
  cl::HideUnrelatedOptions(InputGenGPUCategory);
  cl::ParseCommandLineOptions(
      Argc, Argv,
      "Launch an InputGen GPU entry kernel from an image and record JSON\n");

  if (Error Err = runInputGenGPU()) {
    WithColor::error(errs(), TOOL_NAME) << toString(std::move(Err)) << "\n";
    return 1;
  }
  return 0;
}
