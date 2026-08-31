//===- llvm-inputgen-gpu.cpp - Launch InputGen GPU entry kernels ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Launch a prebuilt direct-entry InputGen GPU image through libomptarget.
// Factory layout is private to this launcher and the device runtime.
//
//===----------------------------------------------------------------------===//

#include "omptarget.h"

#include "inputgen_gpu_factory.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Frontend/Offloading/Utility.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

#define TOOL_NAME "llvm-inputgen-gpu"
#ifndef OMP_KERNEL_ARG_VERSION
#define OMP_KERNEL_ARG_VERSION 5
#endif

namespace {

constexpr uint64_t DefaultSliceBytes = 16384;
constexpr uint64_t DefaultObjectBytes = 1024;
constexpr uint64_t DefaultResultStride = 8;
constexpr uint64_t FactoryHeaderBytes =
    (sizeof(InputGenGPUFactoryHeader) + 7) & ~uint64_t(7);
constexpr StringLiteral InputGenGPUEntryPointName = "__ig_entry";

enum class InputGenMode : int32_t {
  Generate = INPUTGEN_MODE_GENERATE,
  Replay = INPUTGEN_MODE_REPLAY,
};

// Holds one parsed launcher invocation and its factory layout settings.
struct InputGenInvocation {
  InputGenMode Mode;
  std::string DataFilename;
  int32_t DeviceId;
  uint32_t NumTeams;
  uint32_t NumThreads;
  uint64_t SliceBytes;
  uint64_t ObjectBytes;
};

// Stores one contiguous range of observed input bytes within an object.
struct InputRun {
  uint32_t Offset = 0;
  std::vector<char> Bytes;
};

// Collects an object's fixed capacity and its sparse serialized input ranges.
struct InputObject {
  uint32_t Capacity = 0;
  std::vector<InputRun> Runs;
};

// Represents one GPU thread's serialized objects and pointer relationships.
struct InputLane {
  std::vector<InputObject> Objects;
  std::vector<InputGenGPUInputFileRelation> Relations;
};

// Owns the complete host-side representation of one InputGen data file.
struct InputRecord {
  InputGenGPUInputFileHeader Header{};
  std::vector<InputLane> Lanes;
};

// Owns the launcher-allocated device factory and releases it on destruction.
class DeviceAllocations {
public:
  DeviceAllocations(void *Factory, int32_t DeviceId)
      : Factory(Factory), DeviceId(DeviceId) {}

  DeviceAllocations(const DeviceAllocations &) = delete;
  DeviceAllocations &operator=(const DeviceAllocations &) = delete;

  DeviceAllocations(DeviceAllocations &&Other)
      : Factory(Other.Factory), DeviceId(Other.DeviceId) {
    Other.Factory = nullptr;
  }

  DeviceAllocations &operator=(DeviceAllocations &&Other) = delete;

  ~DeviceAllocations() {
    if (Factory)
      omp_target_free(Factory, DeviceId);
  }

  void *Factory = nullptr;
  int32_t DeviceId = 0;
};

// Owns the arrays backing the single opaque-context kernel argument.
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
cl::opt<std::string> JsonFilename(cl::Positional, cl::desc("<record.json>"),
                                  cl::Required, cl::cat(InputGenGPUCategory));
cl::opt<std::string> InputGenDataFilename(
    "inputgen-data",
    cl::desc("Binary file used to write generated input and read replay input"),
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
cl::opt<uint64_t> FactoryBytesOpt(
    "factory-bytes-per-thread",
    cl::desc(
        "Bytes reserved by the driver for each GPU thread's factory slice"),
    cl::init(DefaultSliceBytes), cl::cat(InputGenGPUCategory));
cl::opt<uint64_t> ObjectBytesOpt(
    "object-bytes",
    cl::desc("Fixed data capacity for each pointer-argument object"),
    cl::init(DefaultObjectBytes), cl::cat(InputGenGPUCategory));

template <typename... ArgsTy>
Error createErr(const char *ErrFmt, ArgsTy &&...Args) {
  return createStringError(inconvertibleErrorCode(), ErrFmt,
                           std::forward<ArgsTy>(Args)...);
}

// Write one fixed-size record value and translate a short write into an Error.
template <typename T> Error writeValue(FILE *File, const T &Value) {
  if (std::fwrite(&Value, sizeof(Value), 1, File) != 1)
    return createErr("failed to write InputGen data");
  return Error::success();
}

// Read one fixed-size record value and reject truncated binary input.
template <typename T> Error readValue(FILE *File, T &Value) {
  if (std::fread(&Value, sizeof(Value), 1, File) != 1)
    return createErr("failed to read InputGen data");
  return Error::success();
}

// Round a factory offset up to its power-of-two object alignment.
uint64_t alignTo(uint64_t Value, uint64_t Alignment) {
  return (Value + Alignment - 1) & ~(Alignment - 1);
}

// Derive the lane count from launch geometry while rejecting overflow or zero.
Expected<uint32_t> getNumLanes(uint32_t NumTeams, uint32_t NumThreads) {
  if (!NumTeams || !NumThreads ||
      NumTeams > std::numeric_limits<uint32_t>::max() / NumThreads)
    return createErr("invalid launch geometry");
  return NumTeams * NumThreads;
}

// Compute the driver allocation size for all lane slices without overflow.
Expected<uint64_t> getFactorySize(uint32_t NumLanes, uint64_t SliceBytes) {
  if (!SliceBytes ||
      NumLanes > (std::numeric_limits<uint64_t>::max() - FactoryHeaderBytes) /
                     SliceBytes)
    return createErr("factory allocation size overflows");
  return FactoryHeaderBytes + uint64_t(NumLanes) * SliceBytes;
}

// Validate that CLI capacities fit the fixed-width fields used by the device
// factory layout before allocating device memory.
Error validateFactoryOptions(const InputGenInvocation &Invocation) {
  if (!Invocation.SliceBytes || Invocation.SliceBytes % 8 ||
      Invocation.SliceBytes < sizeof(InputGenGPUFactorySliceHeader) ||
      Invocation.SliceBytes > std::numeric_limits<uint32_t>::max())
    return createErr("--factory-bytes-per-thread must be an 8-byte-aligned "
                     "value between %zu and %u",
                     sizeof(InputGenGPUFactorySliceHeader),
                     std::numeric_limits<uint32_t>::max());
  if (!Invocation.ObjectBytes ||
      Invocation.ObjectBytes > std::numeric_limits<uint32_t>::max())
    return createErr(
        "--object-bytes must be a nonzero value no greater than %u",
        std::numeric_limits<uint32_t>::max());
  return Error::success();
}

Error validateLaneLayout(const InputLane &Lane, uint64_t SliceBytes) {
  if (Lane.Objects.empty() ||
      Lane.Objects.size() > INPUTGEN_GPU_VPTR_OBJECT_MASK + 1 ||
      Lane.Relations.size() + 1 != Lane.Objects.size())
    return createErr("invalid InputGen object relationship layout");

  for (uint32_t I = 0; I < Lane.Relations.size(); ++I) {
    const auto &Relation = Lane.Relations[I];
    if (Relation.OwnerObject >= Lane.Objects.size() ||
        Relation.TargetObject >= Lane.Objects.size() ||
        Relation.SlotOffset > Lane.Objects[Relation.OwnerObject].Capacity ||
        sizeof(uint64_t) >
            Lane.Objects[Relation.OwnerObject].Capacity - Relation.SlotOffset ||
        Relation.TargetOffset > Lane.Objects[Relation.TargetObject].Capacity)
      return createErr("invalid InputGen pointer relationship");
  }

  uint64_t LayoutBytes = alignTo(sizeof(InputGenGPUFactorySliceHeader), 8) +
                         uint64_t(Lane.Objects.size()) * sizeof(uint64_t) +
                         uint64_t(Lane.Relations.size()) *
                             sizeof(InputGenGPUFactoryPointerRelation);
  return LayoutBytes <= SliceBytes
             ? Error::success()
             : createErr("InputGen tables do not fit in slice");
}

template <typename T>
Error getInteger(const json::Object *Obj, StringRef Key, T &Result) {
  std::optional<int64_t> OptInt = Obj->getInteger(Key);
  if (!OptInt)
    return createErr("failed to read JSON integer %s", Key.data());
  Result = static_cast<T>(*OptInt);
  return Error::success();
}

std::string getDefaultInputGenDataFilename(StringRef JsonPath) {
  SmallString<256> Path(JsonPath);
  sys::path::replace_extension(Path, "inputgen");
  return std::string(Path);
}

Expected<InputGenMode> parseMode() {
  if (ModeArg == "generate")
    return InputGenMode::Generate;
  if (ModeArg == "replay")
    return InputGenMode::Replay;
  return createErr("invalid mode '%s'; expected 'generate' or 'replay'",
                   ModeArg.c_str());
}

Expected<InputGenInvocation> parseInvocation() {
  Expected<InputGenMode> ModeOrErr = parseMode();
  if (!ModeOrErr)
    return ModeOrErr.takeError();

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

  int32_t JsonDeviceId = 0;
  if (Error Err = getInteger(JsonObj, "DeviceId", JsonDeviceId))
    return std::move(Err);

  return InputGenInvocation{
      *ModeOrErr,
      InputGenDataFilename.empty()
          ? getDefaultInputGenDataFilename(JsonFilename)
          : InputGenDataFilename.getValue(),
      DeviceIdOpt >= 0 ? DeviceIdOpt : JsonDeviceId,
      NumTeamsOpt > 0 ? NumTeamsOpt.getValue() : 1,
      NumThreadsOpt > 0 ? NumThreadsOpt.getValue() : 1,
      FactoryBytesOpt,
      ObjectBytesOpt,
  };
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

// Allocate the single driver-owned opaque factory context.
Expected<DeviceAllocations> allocateInputGenBuffers(int32_t DeviceId,
                                                    uint64_t FactorySize) {
  void *Factory = omp_target_alloc(FactorySize, DeviceId);
  if (!Factory) {
    return createErr("omp_target_alloc failed on device %" PRId32, DeviceId);
  }
  return DeviceAllocations(Factory, DeviceId);
}

// Persist sparse per-object input ranges so replay stores only bytes observed
// by a load, rather than the complete device factory.
Error writeInputRecord(StringRef Filename, const InputRecord &Record) {
  FILE *File = std::fopen(Filename.str().c_str(), "wb");
  if (!File)
    return createErr("failed to open generated data file '%s'",
                     Filename.data());

  Error Err = writeValue(File, Record.Header);
  for (const InputLane &Lane : Record.Lanes) {
    InputGenGPUInputFileLaneHeader LaneHeader{
        static_cast<uint32_t>(Lane.Objects.size()),
        static_cast<uint32_t>(Lane.Relations.size())};
    if (!Err)
      Err = writeValue(File, LaneHeader);
    for (const InputObject &Object : Lane.Objects) {
      InputGenGPUInputFileObjectHeader ObjectHeader{
          Object.Capacity, static_cast<uint32_t>(Object.Runs.size())};
      if (!Err)
        Err = writeValue(File, ObjectHeader);
      for (const InputRun &Run : Object.Runs) {
        InputGenGPUInputFileRunHeader RunHeader{
            Run.Offset, static_cast<uint32_t>(Run.Bytes.size())};
        if (!Err)
          Err = writeValue(File, RunHeader);
        if (!Err && !Run.Bytes.empty() &&
            std::fwrite(Run.Bytes.data(), 1, Run.Bytes.size(), File) !=
                Run.Bytes.size())
          Err = createErr("failed to write InputGen data");
      }
    }
    for (const auto &Relation : Lane.Relations)
      if (!Err)
        Err = writeValue(File, Relation);
  }
  std::fclose(File);
  return std::move(Err);
}

// Parse and bounds-check the sparse binary record needed to rebuild replay
// factory objects.
Expected<InputRecord> readInputRecord(StringRef Filename) {
  FILE *File = std::fopen(Filename.str().c_str(), "rb");
  if (!File)
    return createErr("failed to open replay data file '%s'", Filename.data());

  InputRecord Record;
  Error Err = readValue(File, Record.Header);
  if (!Err && (Record.Header.Magic != INPUTGEN_GPU_INPUT_MAGIC ||
               Record.Header.Version != INPUTGEN_GPU_FACTORY_VERSION))
    Err = createErr("unsupported InputGen data file '%s'", Filename.data());
  if (!Err) {
    Expected<uint32_t> NumLanes =
        getNumLanes(Record.Header.NumTeams, Record.Header.NumThreads);
    if (!NumLanes || *NumLanes != Record.Header.NumLanes ||
        !Record.Header.SliceBytes || !Record.Header.ObjectBytes ||
        Record.Header.ResultStride != DefaultResultStride)
      Err = createErr("invalid InputGen data file '%s'", Filename.data());
  }

  if (!Err)
    Record.Lanes.resize(Record.Header.NumLanes);
  for (InputLane &Lane : Record.Lanes) {
    InputGenGPUInputFileLaneHeader LaneHeader{};
    if (!Err)
      Err = readValue(File, LaneHeader);
    if (!Err && !LaneHeader.ObjectCount)
      Err = createErr("invalid InputGen data file '%s'", Filename.data());
    if (!Err)
      Lane.Objects.resize(LaneHeader.ObjectCount);
    if (!Err)
      Lane.Relations.resize(LaneHeader.RelationCount);
    for (InputObject &Object : Lane.Objects) {
      InputGenGPUInputFileObjectHeader ObjectHeader{};
      if (!Err)
        Err = readValue(File, ObjectHeader);
      if (!Err && ObjectHeader.Capacity == 0 &&
          &Object != &Lane.Objects.front())
        Err = createErr("invalid InputGen data file '%s'", Filename.data());
      if (!Err) {
        Object.Capacity = ObjectHeader.Capacity;
        Object.Runs.resize(ObjectHeader.NumRuns);
      }
      for (InputRun &Run : Object.Runs) {
        InputGenGPUInputFileRunHeader RunHeader{};
        if (!Err)
          Err = readValue(File, RunHeader);
        if (!Err && (RunHeader.Offset > Object.Capacity ||
                     RunHeader.Size > Object.Capacity - RunHeader.Offset))
          Err = createErr("invalid InputGen data file '%s'", Filename.data());
        if (!Err) {
          Run.Offset = RunHeader.Offset;
          Run.Bytes.resize(RunHeader.Size);
          if (RunHeader.Size && std::fread(Run.Bytes.data(), 1, RunHeader.Size,
                                           File) != RunHeader.Size)
            Err = createErr("failed to read InputGen data");
        }
      }
    }
    for (auto &Relation : Lane.Relations)
      if (!Err)
        Err = readValue(File, Relation);
    if (!Err)
      Err = validateLaneLayout(Lane, Record.Header.SliceBytes);
  }
  std::fclose(File);
  if (Err)
    return std::move(Err);
  return Record;
}

// Convert device slices into sparse input ranges, using saved bytes when user
// code overwrote a value after reading it.
Expected<InputRecord> serializeFactory(const InputGenInvocation &Invocation,
                                       ArrayRef<uint8_t> Factory) {
  Expected<uint32_t> NumLanes =
      getNumLanes(Invocation.NumTeams, Invocation.NumThreads);
  if (!NumLanes)
    return NumLanes.takeError();
  InputRecord Record;
  Record.Header = {INPUTGEN_GPU_INPUT_MAGIC,
                   INPUTGEN_GPU_FACTORY_VERSION,
                   Invocation.NumTeams,
                   Invocation.NumThreads,
                   *NumLanes,
                   Invocation.SliceBytes,
                   Invocation.ObjectBytes,
                   DefaultResultStride};
  Record.Lanes.resize(*NumLanes);

  for (uint32_t LaneIndex = 0; LaneIndex < *NumLanes; ++LaneIndex) {
    uint64_t SliceOffset =
        FactoryHeaderBytes + uint64_t(LaneIndex) * Invocation.SliceBytes;
    if (SliceOffset + sizeof(InputGenGPUFactorySliceHeader) > Factory.size())
      return createErr("factory copy is truncated");
    auto *Slice = reinterpret_cast<const InputGenGPUFactorySliceHeader *>(
        Factory.data() + SliceOffset);
    if (Slice->Magic != INPUTGEN_GPU_FACTORY_SLICE_MAGIC)
      return createErr("device GPU thread %u has invalid factory magic",
                       LaneIndex);
    if (Slice->Version != INPUTGEN_GPU_FACTORY_VERSION)
      return createErr("device GPU thread %u has factory version %u", LaneIndex,
                       Slice->Version);
    if (Slice->Error)
      return createErr("device GPU thread %u reported InputGen factory error %u",
                       LaneIndex, Slice->Error);
    if (Slice->SliceIndex != LaneIndex)
      return createErr("device GPU thread %u reported slice index %u",
                       LaneIndex, Slice->SliceIndex);
    if (!Slice->ObjectCount)
      return createErr("device GPU thread %u created no argument object",
                       LaneIndex);
    if (Slice->ObjectCount != Slice->ObjectLimit)
      return createErr("device GPU thread %u created %u of %u objects",
                       LaneIndex, Slice->ObjectCount, Slice->ObjectLimit);
    if (Slice->RelationCount + 1 != Slice->ObjectCount ||
        Slice->RelationCount != Slice->RelationLimit)
      return createErr("device GPU thread %u has inconsistent pointer relations",
                       LaneIndex);

    InputLane &Lane = Record.Lanes[LaneIndex];
    Lane.Objects.reserve(Slice->ObjectCount);
    for (uint32_t ObjectIndex = 0; ObjectIndex < Slice->ObjectCount;
         ++ObjectIndex) {
      uint64_t TableOffset =
          Slice->ObjectTableOffset + uint64_t(ObjectIndex) * sizeof(uint64_t);
      if (TableOffset > Invocation.SliceBytes ||
          sizeof(uint64_t) > Invocation.SliceBytes - TableOffset)
        return createErr("invalid object table in device factory");
      uint64_t RecordOffset = *reinterpret_cast<const uint64_t *>(
          Factory.data() + SliceOffset + TableOffset);
      if (RecordOffset > Invocation.SliceBytes ||
          sizeof(InputGenGPUFactoryObjectHeader) >
              Invocation.SliceBytes - RecordOffset)
        return createErr("invalid object layout in device factory");
      auto *Object = reinterpret_cast<const InputGenGPUFactoryObjectHeader *>(
          Factory.data() + SliceOffset + RecordOffset);
      if (Object->Magic != INPUTGEN_GPU_FACTORY_OBJECT_MAGIC ||
          Object->ObjectIndex != ObjectIndex)
        return createErr("invalid object layout in device factory");
      uint64_t TotalSize = sizeof(*Object) + 3ull * Object->Capacity;
      if (TotalSize > Invocation.SliceBytes - RecordOffset)
        return createErr("invalid object capacity in device factory");

      const char *Data = reinterpret_cast<const char *>(Object + 1);
      const auto *Mask =
          reinterpret_cast<const uint8_t *>(Data + Object->Capacity);
      const char *Saved =
          reinterpret_cast<const char *>(Mask + Object->Capacity);
      InputObject SerializedObject;
      SerializedObject.Capacity = Object->Capacity;
      uint32_t Offset = 0;
      while (Offset < Object->Capacity) {
        if (!(Mask[Offset] & INPUTGEN_GPU_MASK_READ) ||
            (Mask[Offset] & INPUTGEN_GPU_MASK_POINTER)) {
          ++Offset;
          continue;
        }
        InputRun Run;
        Run.Offset = Offset;
        while (Offset < Object->Capacity &&
               (Mask[Offset] & INPUTGEN_GPU_MASK_READ) &&
               !(Mask[Offset] & INPUTGEN_GPU_MASK_POINTER)) {
          Run.Bytes.push_back((Mask[Offset] & INPUTGEN_GPU_MASK_WRITTEN)
                                  ? Saved[Offset]
                                  : Data[Offset]);
          ++Offset;
        }
        SerializedObject.Runs.push_back(std::move(Run));
      }
      Lane.Objects.push_back(std::move(SerializedObject));
    }
    uint64_t RelationBytes = uint64_t(Slice->RelationCount) *
                             sizeof(InputGenGPUFactoryPointerRelation);
    if (Slice->RelationTableOffset > Invocation.SliceBytes ||
        RelationBytes > Invocation.SliceBytes - Slice->RelationTableOffset)
      return createErr("invalid pointer relationship table in device factory");
    auto *Relations =
        reinterpret_cast<const InputGenGPUFactoryPointerRelation *>(
            Factory.data() + SliceOffset + Slice->RelationTableOffset);
    Lane.Relations.clear();
    Lane.Relations.reserve(Slice->RelationCount);
    for (uint32_t I = 0; I < Slice->RelationCount; ++I)
      Lane.Relations.push_back(
          {Relations[I].OwnerObject, Relations[I].SlotOffset,
           Relations[I].TargetObject, Relations[I].TargetOffset});
    if (Error Err = validateLaneLayout(Lane, Invocation.SliceBytes))
      return std::move(Err);
  }
  return Record;
}

// Rebuild factory headers, data, and read masks from sparse input ranges so
// replay callbacks can reject loads missing from the recording.
Error reconstructFactory(const InputRecord &Record,
                         std::vector<uint8_t> &Factory) {
  Expected<uint64_t> FactorySize =
      getFactorySize(Record.Header.NumLanes, Record.Header.SliceBytes);
  if (!FactorySize)
    return FactorySize.takeError();
  Factory.assign(*FactorySize, 0);

  for (uint32_t LaneIndex = 0; LaneIndex < Record.Header.NumLanes;
       ++LaneIndex) {
    const InputLane &Lane = Record.Lanes[LaneIndex];
    if (Error Err = validateLaneLayout(Lane, Record.Header.SliceBytes))
      return std::move(Err);
    uint8_t *SliceStart = Factory.data() + FactoryHeaderBytes +
                          uint64_t(LaneIndex) * Record.Header.SliceBytes;
    auto *Slice = reinterpret_cast<InputGenGPUFactorySliceHeader *>(SliceStart);
    Slice->Magic = INPUTGEN_GPU_FACTORY_SLICE_MAGIC;
    Slice->Version = INPUTGEN_GPU_FACTORY_VERSION;
    Slice->Mode = INPUTGEN_MODE_REPLAY;
    Slice->SliceIndex = LaneIndex;
    Slice->ObjectCount = static_cast<uint32_t>(Lane.Objects.size());
    Slice->ObjectLimit = static_cast<uint32_t>(Lane.Objects.size());
    Slice->RelationCount = static_cast<uint32_t>(Lane.Relations.size());
    Slice->RelationLimit = static_cast<uint32_t>(Lane.Relations.size());
    Slice->ArgumentBytes = Lane.Objects.front().Capacity;
    Slice->ObjectBytes = Record.Header.ObjectBytes;

    Slice->ObjectTableOffset = alignTo(sizeof(*Slice), 8);
    Slice->RelationTableOffset =
        Slice->ObjectTableOffset +
        uint64_t(Slice->ObjectLimit) * sizeof(uint64_t);
    uint64_t ObjectOffset =
        alignTo(Slice->RelationTableOffset +
                    uint64_t(Slice->RelationLimit) *
                        sizeof(InputGenGPUFactoryPointerRelation),
                8);
    for (uint32_t ObjectIndex = 0; ObjectIndex < Lane.Objects.size();
         ++ObjectIndex) {
      const InputObject &SerializedObject = Lane.Objects[ObjectIndex];
      uint64_t TotalSize = sizeof(InputGenGPUFactoryObjectHeader) +
                           3ull * SerializedObject.Capacity;
      if ((!SerializedObject.Capacity && ObjectIndex != 0) ||
          TotalSize > Record.Header.SliceBytes - ObjectOffset)
        return createErr("InputGen replay object does not fit in its slice");
      auto *Object = reinterpret_cast<InputGenGPUFactoryObjectHeader *>(
          SliceStart + ObjectOffset);
      Object->Magic = INPUTGEN_GPU_FACTORY_OBJECT_MAGIC;
      Object->ObjectIndex = ObjectIndex;
      Object->Capacity = SerializedObject.Capacity;
      Object->SliceOffset = ObjectOffset;
      *reinterpret_cast<uint64_t *>(SliceStart + Slice->ObjectTableOffset +
                                    ObjectIndex * sizeof(uint64_t)) =
          ObjectOffset;
      char *Data = reinterpret_cast<char *>(Object + 1);
      auto *Mask = reinterpret_cast<uint8_t *>(Data + Object->Capacity);
      for (const InputRun &Run : SerializedObject.Runs) {
        if (Run.Offset > Object->Capacity ||
            Run.Bytes.size() > Object->Capacity - Run.Offset)
          return createErr("invalid replay run");
        std::memcpy(Data + Run.Offset, Run.Bytes.data(), Run.Bytes.size());
        std::memset(Mask + Run.Offset, INPUTGEN_GPU_MASK_READ,
                    Run.Bytes.size());
      }
      ObjectOffset = alignTo(ObjectOffset + TotalSize, 8);
    }
    auto *Relations = reinterpret_cast<InputGenGPUFactoryPointerRelation *>(
        SliceStart + Slice->RelationTableOffset);
    for (uint32_t I = 0; I < Lane.Relations.size(); ++I)
      Relations[I] = {
          Lane.Relations[I].OwnerObject, Lane.Relations[I].SlotOffset,
          Lane.Relations[I].TargetObject, Lane.Relations[I].TargetOffset};
    Slice->NextOffset = ObjectOffset;
  }
  return Error::success();
}

void initializeFactoryHeader(std::vector<uint8_t> &Factory,
                             const InputGenInvocation &Invocation,
                             uint64_t FactorySize) {
  auto *Header = reinterpret_cast<InputGenGPUFactoryHeader *>(Factory.data());
  Header->Magic = INPUTGEN_GPU_FACTORY_SLICE_MAGIC;
  Header->Version = INPUTGEN_GPU_FACTORY_VERSION;
  Header->Mode = static_cast<uint32_t>(Invocation.Mode);
  Header->NumTeams = Invocation.NumTeams;
  Header->ThreadsPerTeam = Invocation.NumThreads;
  Header->NumLanes = Invocation.NumTeams * Invocation.NumThreads;
  Header->SliceBytes = Invocation.SliceBytes;
  Header->ObjectBytes = Invocation.ObjectBytes;
  Header->FactoryBytes = FactorySize;
}

// Take replay geometry and capacities from the record and reject conflicting
// CLI options, keeping generate and replay factory layouts identical.
Error applyReplayInvocation(InputGenInvocation &Invocation,
                            const InputRecord &Record) {
  if ((NumTeamsOpt.getNumOccurrences() &&
       NumTeamsOpt != Record.Header.NumTeams) ||
      (NumThreadsOpt.getNumOccurrences() &&
       NumThreadsOpt != Record.Header.NumThreads) ||
      (FactoryBytesOpt.getNumOccurrences() &&
       FactoryBytesOpt != Record.Header.SliceBytes) ||
      (ObjectBytesOpt.getNumOccurrences() &&
       ObjectBytesOpt != Record.Header.ObjectBytes))
    return createErr("replay options conflict with the InputGen data file");

  Invocation.NumTeams = Record.Header.NumTeams;
  Invocation.NumThreads = Record.Header.NumThreads;
  Invocation.SliceBytes = Record.Header.SliceBytes;
  Invocation.ObjectBytes = Record.Header.ObjectBytes;
  return Error::success();
}

// Copy initialized factory bytes to the device and clear every result slot.
Error copyToDevice(DeviceAllocations &Allocs, ArrayRef<uint8_t> Factory,
                   int HostDevice) {
  if (omp_target_memcpy(Allocs.Factory, Factory.data(), Factory.size(), 0, 0,
                        Allocs.DeviceId, HostDevice) != 0)
    return createErr("failed to copy factory to device");
  return Error::success();
}

// Marshal the private factory ABI into literal OpenMP kernel arguments for the
// generated entry wrapper.
void buildKernelLaunchArguments(const InputGenInvocation &Invocation,
                                DeviceAllocations &Allocs, uint64_t FactorySize,
                                KernelLaunchArguments &Args) {
  (void)FactorySize;
  Args.ArgBasePtrs[0] = &Allocs.Factory;
  Args.ArgPtrs[0] = &Allocs.Factory;
  Args.ArgSizes[0] = sizeof(Allocs.Factory);
  Args.ArgTypes[0] = OMP_TGT_MAPTYPE_TARGET_PARAM | OMP_TGT_MAPTYPE_LITERAL;

  Args.KernelArgs.Version = OMP_KERNEL_ARG_VERSION;
  Args.KernelArgs.NumArgs = 1;
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
  if (__tgt_target_kernel(nullptr, Invocation.DeviceId, Invocation.NumTeams,
                          Invocation.NumThreads, Entry.Address,
                          &Args.KernelArgs) != OMP_TGT_SUCCESS)
    return createErr("failed to launch entry kernel '%s'",
                     InputGenGPUEntryPointName.data());
  return Error::success();
}

// Copy per-lane return values back from the device and label generate/replay
// output for the reduction pipeline.
Error copyResults(const InputGenInvocation &Invocation,
                  ArrayRef<uint8_t> Factory, uint32_t NumLanes) {
  for (uint32_t Lane = 0; Lane < NumLanes; ++Lane) {
    uint64_t Offset =
        FactoryHeaderBytes + uint64_t(Lane) * Invocation.SliceBytes;
    auto *Slice = reinterpret_cast<const InputGenGPUFactorySliceHeader *>(
        Factory.data() + Offset);
    if (Slice->Error)
      return createErr(
          "device GPU thread %u reported InputGen factory error %u", Lane,
          Slice->Error);
    if (!Slice->ResultSize)
      continue;
    outs() << (Invocation.Mode == InputGenMode::Generate ? "result"
                                                         : "replay result")
           << "[" << Lane << "] = " << Slice->ResultBits << "\n";
  }
  return Error::success();
}

Error runInputGenGPU() {
  Expected<InputGenInvocation> InvocationOrErr = parseInvocation();
  if (!InvocationOrErr)
    return InvocationOrErr.takeError();
  InputGenInvocation Invocation = std::move(*InvocationOrErr);

  if (Error Err = validateFactoryOptions(Invocation))
    return std::move(Err);

  std::vector<uint8_t> HostFactory;
  if (Invocation.Mode == InputGenMode::Replay) {
    Expected<InputRecord> RecordOrErr =
        readInputRecord(Invocation.DataFilename);
    if (!RecordOrErr)
      return RecordOrErr.takeError();
    if (Error Err = applyReplayInvocation(Invocation, *RecordOrErr))
      return std::move(Err);
    if (Error Err = validateFactoryOptions(Invocation))
      return std::move(Err);
    if (Error Err = reconstructFactory(*RecordOrErr, HostFactory))
      return std::move(Err);
  }

  Expected<uint32_t> NumLanes =
      getNumLanes(Invocation.NumTeams, Invocation.NumThreads);
  if (!NumLanes)
    return NumLanes.takeError();
  Expected<uint64_t> FactorySize =
      getFactorySize(*NumLanes, Invocation.SliceBytes);
  if (!FactorySize)
    return FactorySize.takeError();
  if (Invocation.Mode == InputGenMode::Generate)
    HostFactory.assign(*FactorySize, 0);
  initializeFactoryHeader(HostFactory, Invocation, *FactorySize);

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
  Expected<DeviceAllocations> AllocsOrErr =
      allocateInputGenBuffers(Invocation.DeviceId, *FactorySize);
  if (!AllocsOrErr)
    return AllocsOrErr.takeError();
  DeviceAllocations Allocs = std::move(*AllocsOrErr);

  if (Error Err = copyToDevice(Allocs, HostFactory, HostDevice))
    return std::move(Err);
  KernelLaunchArguments LaunchArgs;
  buildKernelLaunchArguments(Invocation, Allocs, *FactorySize, LaunchArgs);
  if (Error Err = launchEntryKernel(Invocation, OffloadEntries[0], LaunchArgs))
    return std::move(Err);

  if (omp_target_memcpy(HostFactory.data(), Allocs.Factory, HostFactory.size(),
                        0, 0, HostDevice, Invocation.DeviceId) != 0)
    return createErr("failed to copy factory from device");
  if (Invocation.Mode == InputGenMode::Generate) {
    Expected<InputRecord> RecordOrErr =
        serializeFactory(Invocation, HostFactory);
    if (!RecordOrErr)
      return RecordOrErr.takeError();
    if (Error Err = writeInputRecord(Invocation.DataFilename, *RecordOrErr))
      return std::move(Err);
    outs() << "serialized input = " << Invocation.DataFilename << "\n";
  }
  return copyResults(Invocation, HostFactory, *NumLanes);
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
