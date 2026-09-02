//===-- InputGenGPUCodec.cpp - InputGen GPU host codec --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "inputgen_gpu/InputGenGPUCodec.h"

#include "../inputgen_gpu_factory.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace inputgen_gpu {
namespace {

struct InputRun {
  uint32_t Offset = 0;
  std::vector<uint8_t> Bytes;
};

struct InputObject {
  std::vector<InputRun> Runs;
};

struct InputThread {
  std::vector<InputObject> Objects;
  std::vector<InputGenGPUFactoryPointerRelation> Relations;
};

struct Record {
  InputGenGPUInputFileHeader Header{};
  std::vector<InputThread> Threads;
};

template <typename... Args>
Error makeError(const char *Format, Args... Values) {
  int Size = std::snprintf(nullptr, 0, Format, Values...);
  if (Size < 0)
    return Error("failed to format InputGen GPU codec error");
  std::string Message(static_cast<size_t>(Size), '\0');
  std::snprintf(Message.data(), Message.size() + 1, Format, Values...);
  return Error(std::move(Message));
}

template <typename T> Error writeValue(FILE *File, const T &Value) {
  if (std::fwrite(&Value, sizeof(Value), 1, File) != 1)
    return Error("failed to write InputGen data");
  return Error();
}

template <typename T> Error readValue(FILE *File, T &Value) {
  if (std::fread(&Value, sizeof(Value), 1, File) != 1)
    return Error("failed to read InputGen data");
  return Error();
}

// Derive the factory layout, mapping the shared header's reason code to the
// diagnostic the launcher prints.  Every offset this file uses comes from here;
// see inputgen_gpu_factory.h for why it must not be recomputed locally.
Result<InputGenGPUFactoryLayout> computeLayout(const FactoryConfig &Config) {
  InputGenGPUFactoryLayout Layout;
  switch (inputgenGPUComputeLayout(Config.NumTeams, Config.NumThreads,
                                   Config.ObjectBytes, Config.ObjectsPerThread,
                                   &Layout)) {
  case INPUTGEN_GPU_LAYOUT_OK:
    return Layout;
  case INPUTGEN_GPU_LAYOUT_BAD_OBJECT_BYTES:
    return makeError("object bytes must be an 8-byte-aligned nonzero value no "
                     "greater than %u",
                     std::numeric_limits<uint32_t>::max());
  case INPUTGEN_GPU_LAYOUT_BAD_OBJECT_COUNT:
    return Error("objects per GPU thread must be nonzero");
  case INPUTGEN_GPU_LAYOUT_OVERFLOW:
    return Error("factory allocation size overflows");
  default:
    return Error("invalid launch geometry");
  }
}

Error validateThreadLayout(const InputThread &Thread, uint64_t ArgumentBytes,
                           uint64_t ObjectBytes, uint32_t ObjectsPerThread) {
  if (ArgumentBytes > ObjectBytes || Thread.Objects.empty() ||
      Thread.Objects.size() > ObjectsPerThread ||
      Thread.Relations.size() >= ObjectsPerThread ||
      Thread.Relations.size() + 1 != Thread.Objects.size() || !ObjectsPerThread)
    return Error("invalid InputGen object relationship layout");

  for (const InputGenGPUFactoryPointerRelation &Relation : Thread.Relations) {
    uint64_t OwnerCapacity =
        Relation.OwnerObject == 0 ? ArgumentBytes : ObjectBytes;
    uint64_t TargetCapacity =
        Relation.TargetObject == 0 ? ArgumentBytes : ObjectBytes;
    if (Relation.OwnerObject >= Thread.Objects.size() ||
        Relation.TargetObject >= Thread.Objects.size() ||
        Relation.SlotOffset > OwnerCapacity ||
        sizeof(uint64_t) > OwnerCapacity - Relation.SlotOffset ||
        Relation.TargetOffset > TargetCapacity)
      return Error("invalid InputGen pointer relationship");
  }

  return Error();
}

FactoryConfig configFromHeader(const InputGenGPUInputFileHeader &Header) {
  return FactoryConfig{Header.NumTeams, Header.NumThreads, Header.ObjectBytes,
                       Header.ObjectsPerThread};
}

void initializeFactoryHeader(std::vector<uint8_t> &Bytes,
                             const FactoryConfig &Config,
                             const InputGenGPUFactoryLayout &Layout,
                             Mode ExecutionMode) {
  auto *Header = reinterpret_cast<InputGenGPUFactoryHeader *>(Bytes.data());
  Header->Magic = INPUTGEN_GPU_FACTORY_SLICE_MAGIC;
  Header->Version = INPUTGEN_GPU_FACTORY_VERSION;
  Header->Mode = static_cast<uint32_t>(ExecutionMode);
  Header->ThreadsPerTeam = Config.NumThreads;
  Header->NumGPUThreads = Layout.NumThreads;
  Header->ObjectsPerThread = Config.ObjectsPerThread;
  Header->ObjectBytes = Config.ObjectBytes;
}

} // namespace

Result<Factory> createGenerationFactory(const FactoryConfig &Config) {
  Result<InputGenGPUFactoryLayout> Layout = computeLayout(Config);
  if (!Layout)
    return Layout.error();
  std::vector<uint8_t> Bytes(Layout.value().TotalBytes, 0);
  initializeFactoryHeader(Bytes, Config, Layout.value(), Mode::Generate);
  return Factory(Mode::Generate, Config, std::move(Bytes));
}

namespace {

Error writeRecord(const std::string &Filename, const Record &Storage) {
  FILE *RawFile = std::fopen(Filename.c_str(), "wb");
  if (!RawFile)
    return makeError("failed to open generated data file '%s'",
                     Filename.c_str());
  std::unique_ptr<FILE, int (*)(FILE *)> File(RawFile, &std::fclose);

  if (Error Failure = writeValue(File.get(), Storage.Header))
    return Failure;
  for (const InputThread &Thread : Storage.Threads) {
    InputGenGPUInputFileThreadHeader ThreadHeader{
        static_cast<uint32_t>(Thread.Objects.size()),
        static_cast<uint32_t>(Thread.Relations.size())};
    if (Error Failure = writeValue(File.get(), ThreadHeader))
      return Failure;
    for (const InputObject &Object : Thread.Objects) {
      InputGenGPUInputFileObjectHeader ObjectHeader{
          static_cast<uint32_t>(Object.Runs.size())};
      if (Error Failure = writeValue(File.get(), ObjectHeader))
        return Failure;
      for (const InputRun &Run : Object.Runs) {
        InputGenGPUInputFileRunHeader RunHeader{
            Run.Offset, static_cast<uint32_t>(Run.Bytes.size())};
        if (Error Failure = writeValue(File.get(), RunHeader))
          return Failure;
        if (!Run.Bytes.empty() &&
            std::fwrite(Run.Bytes.data(), 1, Run.Bytes.size(), File.get()) !=
                Run.Bytes.size())
          return Error("failed to write InputGen data");
      }
    }
    for (const InputGenGPUFactoryPointerRelation &Relation : Thread.Relations)
      if (Error Failure = writeValue(File.get(), Relation))
        return Failure;
  }
  if (std::fclose(File.release()) != 0)
    return Error("failed to close generated InputGen data");
  return Error();
}

Result<Record> readRecord(const std::string &Filename) {
  FILE *RawFile = std::fopen(Filename.c_str(), "rb");
  if (!RawFile)
    return makeError("failed to open replay data file '%s'", Filename.c_str());
  std::unique_ptr<FILE, int (*)(FILE *)> File(RawFile, &std::fclose);

  Record Storage;
  if (Error Failure = readValue(File.get(), Storage.Header))
    return Failure;
  if (Storage.Header.Magic != INPUTGEN_GPU_INPUT_MAGIC ||
      Storage.Header.Version != INPUTGEN_GPU_FACTORY_VERSION)
    return makeError("unsupported InputGen data file '%s'", Filename.c_str());
  Result<InputGenGPUFactoryLayout> Layout =
      computeLayout(configFromHeader(Storage.Header));
  if (!Layout || Storage.Header.ArgumentBytes > Storage.Header.ObjectBytes)
    return makeError("invalid InputGen data file '%s'", Filename.c_str());

  Storage.Threads.resize(Layout.value().NumThreads);
  for (InputThread &Thread : Storage.Threads) {
    InputGenGPUInputFileThreadHeader ThreadHeader{};
    if (Error Failure = readValue(File.get(), ThreadHeader))
      return Failure;
    if (!ThreadHeader.ObjectCount ||
        ThreadHeader.ObjectCount > Storage.Header.ObjectsPerThread ||
        ThreadHeader.RelationCount + 1 != ThreadHeader.ObjectCount)
      return makeError("invalid InputGen data file '%s'", Filename.c_str());
    Thread.Objects.resize(ThreadHeader.ObjectCount);
    Thread.Relations.resize(ThreadHeader.RelationCount);
    for (size_t ObjectIndex = 0; ObjectIndex < Thread.Objects.size();
         ++ObjectIndex) {
      InputObject &Object = Thread.Objects[ObjectIndex];
      InputGenGPUInputFileObjectHeader ObjectHeader{};
      if (Error Failure = readValue(File.get(), ObjectHeader))
        return Failure;
      Object.Runs.resize(ObjectHeader.NumRuns);
      uint64_t Capacity = ObjectIndex == 0 ? Storage.Header.ArgumentBytes
                                           : Storage.Header.ObjectBytes;
      uint32_t PreviousEnd = 0;
      for (InputRun &Run : Object.Runs) {
        InputGenGPUInputFileRunHeader RunHeader{};
        if (Error Failure = readValue(File.get(), RunHeader))
          return Failure;
        if (RunHeader.Offset > Capacity ||
            RunHeader.Size > Capacity - RunHeader.Offset ||
            RunHeader.Offset < PreviousEnd)
          return makeError("invalid InputGen data file '%s'", Filename.c_str());
        Run.Offset = RunHeader.Offset;
        Run.Bytes.resize(RunHeader.Size);
        if (RunHeader.Size && std::fread(Run.Bytes.data(), 1, RunHeader.Size,
                                         File.get()) != RunHeader.Size)
          return Error("failed to read InputGen data");
        PreviousEnd = RunHeader.Offset + RunHeader.Size;
      }
    }
    for (InputGenGPUFactoryPointerRelation &Relation : Thread.Relations)
      if (Error Failure = readValue(File.get(), Relation))
        return Failure;
    if (Error Failure = validateThreadLayout(
            Thread, Storage.Header.ArgumentBytes, Storage.Header.ObjectBytes,
            Storage.Header.ObjectsPerThread))
      return Failure;
  }
  if (std::fgetc(File.get()) != EOF)
    return makeError("invalid trailing data in InputGen data file '%s'",
                     Filename.c_str());
  return Storage;
}

Result<Record> serializeFactory(const Factory &Value) {
  const FactoryConfig &Config = Value.config();
  Result<InputGenGPUFactoryLayout> LayoutOrErr = computeLayout(Config);
  if (!LayoutOrErr)
    return LayoutOrErr.error();
  const InputGenGPUFactoryLayout &Layout = LayoutOrErr.value();
  const uint8_t *Bytes = Value.data();
  Record Storage;
  Storage.Header = {
      INPUTGEN_GPU_INPUT_MAGIC,     Config.ObjectBytes, 0,
      INPUTGEN_GPU_FACTORY_VERSION, Config.NumTeams,    Config.NumThreads,
      Config.ObjectsPerThread};
  Storage.Threads.resize(Layout.NumThreads);

  for (uint32_t ThreadIndex = 0; ThreadIndex < Layout.NumThreads;
       ++ThreadIndex) {
    uint64_t SliceOffset = inputgenGPUSliceOffset(&Layout, ThreadIndex);
    if (SliceOffset + sizeof(InputGenGPUFactorySliceHeader) > Value.size())
      return Error("factory copy is truncated");
    auto *Slice = reinterpret_cast<const InputGenGPUFactorySliceHeader *>(
        Bytes + SliceOffset);
    if (Slice->Magic != INPUTGEN_GPU_FACTORY_SLICE_MAGIC)
      return makeError("device GPU thread %u has invalid factory magic",
                       ThreadIndex);
    if (Slice->Version != INPUTGEN_GPU_FACTORY_VERSION)
      return makeError("device GPU thread %u has factory version %u",
                       ThreadIndex, Slice->Version);
    if (Slice->Error)
      return makeError("device GPU thread %u reported InputGen factory error "
                       "%u",
                       ThreadIndex, Slice->Error);
    if (Slice->SliceIndex != ThreadIndex)
      return makeError("device GPU thread %u reported slice index %u",
                       ThreadIndex, Slice->SliceIndex);
    if (!Slice->ObjectCount)
      return makeError("device GPU thread %u created no argument object",
                       ThreadIndex);
    if (Slice->ObjectCount > Config.ObjectsPerThread)
      return makeError("device GPU thread %u created %u of %u objects",
                       ThreadIndex, Slice->ObjectCount,
                       Config.ObjectsPerThread);
    if (Slice->RelationCount + 1 != Slice->ObjectCount ||
        Slice->RelationCount >= Config.ObjectsPerThread)
      return makeError("device GPU thread %u has inconsistent pointer "
                       "relations",
                       ThreadIndex);
    if (Slice->ArgumentBytes > Config.ObjectBytes)
      return makeError("device GPU thread %u has invalid fixed object layout",
                       ThreadIndex);
    if (ThreadIndex == 0)
      Storage.Header.ArgumentBytes = Slice->ArgumentBytes;
    else if (Slice->ArgumentBytes != Storage.Header.ArgumentBytes)
      return makeError("device GPU thread %u has inconsistent argument size",
                       ThreadIndex);

    InputThread &Thread = Storage.Threads[ThreadIndex];
    Thread.Objects.reserve(Slice->ObjectCount);
    for (uint32_t ObjectIndex = 0; ObjectIndex < Slice->ObjectCount;
         ++ObjectIndex) {
      uint64_t ObjectOffset = inputgenGPUObjectOffset(&Layout, ObjectIndex);
      if (ObjectOffset > Layout.SliceBytes ||
          Layout.ObjectSlotBytes > Layout.SliceBytes - ObjectOffset)
        return Error("invalid object capacity in device factory");

      const uint8_t *Data = Bytes + SliceOffset + ObjectOffset;
      const uint8_t *Mask = Data + Config.ObjectBytes;
      const uint8_t *Saved = Mask + Config.ObjectBytes;
      InputObject SerializedObject;
      uint64_t Capacity =
          ObjectIndex == 0 ? Slice->ArgumentBytes : Config.ObjectBytes;
      uint32_t Offset = 0;
      while (Offset < Capacity) {
        if (!(Mask[Offset] & INPUTGEN_GPU_MASK_READ) ||
            (Mask[Offset] & INPUTGEN_GPU_MASK_POINTER)) {
          ++Offset;
          continue;
        }
        InputRun Run;
        Run.Offset = Offset;
        while (Offset < Capacity && (Mask[Offset] & INPUTGEN_GPU_MASK_READ) &&
               !(Mask[Offset] & INPUTGEN_GPU_MASK_POINTER)) {
          Run.Bytes.push_back((Mask[Offset] & INPUTGEN_GPU_MASK_WRITTEN)
                                  ? Saved[Offset]
                                  : Data[Offset]);
          ++Offset;
        }
        SerializedObject.Runs.push_back(std::move(Run));
      }
      Thread.Objects.push_back(std::move(SerializedObject));
    }

    uint64_t RelationBytes = uint64_t(Slice->RelationCount) *
                             sizeof(InputGenGPUFactoryPointerRelation);
    if (Layout.RelationTableOffset > Layout.SliceBytes ||
        RelationBytes > Layout.SliceBytes - Layout.RelationTableOffset)
      return Error("invalid pointer relationship table in device factory");
    auto *Relations =
        reinterpret_cast<const InputGenGPUFactoryPointerRelation *>(
            Bytes + SliceOffset + Layout.RelationTableOffset);
    Thread.Relations.reserve(Slice->RelationCount);
    for (uint32_t I = 0; I < Slice->RelationCount; ++I)
      Thread.Relations.push_back(Relations[I]);
    if (Error Failure =
            validateThreadLayout(Thread, Storage.Header.ArgumentBytes,
                                 Config.ObjectBytes, Config.ObjectsPerThread))
      return Failure;
  }
  return Storage;
}

} // namespace

Result<Factory> createReplayFactory(const std::string &Filename) {
  Result<Record> RecordOrErr = readRecord(Filename);
  if (!RecordOrErr)
    return RecordOrErr.error();
  const Record &Storage = RecordOrErr.value();
  FactoryConfig Config = configFromHeader(Storage.Header);
  Result<InputGenGPUFactoryLayout> LayoutOrErr = computeLayout(Config);
  if (!LayoutOrErr)
    return LayoutOrErr.error();
  const InputGenGPUFactoryLayout &Layout = LayoutOrErr.value();
  std::vector<uint8_t> Bytes(Layout.TotalBytes, 0);

  for (uint32_t ThreadIndex = 0; ThreadIndex < Layout.NumThreads;
       ++ThreadIndex) {
    const InputThread &Thread = Storage.Threads[ThreadIndex];
    if (Error Failure = validateThreadLayout(
            Thread, Storage.Header.ArgumentBytes, Storage.Header.ObjectBytes,
            Storage.Header.ObjectsPerThread))
      return Failure;
    uint8_t *SliceStart =
        Bytes.data() + inputgenGPUSliceOffset(&Layout, ThreadIndex);
    auto *Slice = reinterpret_cast<InputGenGPUFactorySliceHeader *>(SliceStart);
    Slice->Magic = INPUTGEN_GPU_FACTORY_SLICE_MAGIC;
    Slice->Version = INPUTGEN_GPU_FACTORY_VERSION;
    Slice->SliceIndex = ThreadIndex;
    Slice->ObjectCount = static_cast<uint32_t>(Thread.Objects.size());
    Slice->RelationCount = static_cast<uint32_t>(Thread.Relations.size());
    Slice->ArgumentBytes = Storage.Header.ArgumentBytes;
    for (uint32_t ObjectIndex = 0; ObjectIndex < Thread.Objects.size();
         ++ObjectIndex) {
      const InputObject &SerializedObject = Thread.Objects[ObjectIndex];
      uint64_t Capacity = ObjectIndex == 0 ? Storage.Header.ArgumentBytes
                                           : Storage.Header.ObjectBytes;
      uint64_t ObjectOffset = inputgenGPUObjectOffset(&Layout, ObjectIndex);
      if (ObjectOffset > Layout.SliceBytes ||
          Layout.ObjectSlotBytes > Layout.SliceBytes - ObjectOffset)
        return Error("InputGen replay object does not fit in its slice");
      uint8_t *Data = SliceStart + ObjectOffset;
      uint8_t *Mask = Data + Storage.Header.ObjectBytes;
      for (const InputRun &Run : SerializedObject.Runs) {
        if (Run.Offset > Capacity || Run.Bytes.size() > Capacity - Run.Offset)
          return Error("invalid replay run");
        std::memcpy(Data + Run.Offset, Run.Bytes.data(), Run.Bytes.size());
        std::memset(Mask + Run.Offset, INPUTGEN_GPU_MASK_READ,
                    Run.Bytes.size());
      }
    }
    auto *Relations = reinterpret_cast<InputGenGPUFactoryPointerRelation *>(
        SliceStart + Layout.RelationTableOffset);
    for (uint32_t I = 0; I < Thread.Relations.size(); ++I)
      Relations[I] = Thread.Relations[I];
  }

  initializeFactoryHeader(Bytes, Config, Layout, Mode::Replay);
  return Factory(Mode::Replay, Config, std::move(Bytes));
}

Error writeGenerationRecord(const std::string &Filename, const Factory &Value) {
  if (Value.ExecutionMode != Mode::Generate)
    return Error("only a generation factory can be serialized");
  Result<Record> RecordOrErr = serializeFactory(Value);
  if (!RecordOrErr)
    return RecordOrErr.error();
  return writeRecord(Filename, RecordOrErr.value());
}

Result<std::vector<ThreadResult>> inspectThreadResults(const Factory &Value) {
  const FactoryConfig &Config = Value.config();
  Result<InputGenGPUFactoryLayout> LayoutOrErr = computeLayout(Config);
  if (!LayoutOrErr)
    return LayoutOrErr.error();
  const InputGenGPUFactoryLayout &Layout = LayoutOrErr.value();
  if (Layout.TotalBytes != Value.size())
    return Error("factory copy has an invalid size");

  const uint8_t *Bytes = Value.data();
  std::vector<ThreadResult> Results;
  Results.reserve(Layout.NumThreads);
  for (uint32_t ThreadIndex = 0; ThreadIndex < Layout.NumThreads;
       ++ThreadIndex) {
    auto *Slice = reinterpret_cast<const InputGenGPUFactorySliceHeader *>(
        Bytes + inputgenGPUSliceOffset(&Layout, ThreadIndex));
    if (Slice->Magic != INPUTGEN_GPU_FACTORY_SLICE_MAGIC ||
        Slice->Version != INPUTGEN_GPU_FACTORY_VERSION ||
        Slice->SliceIndex != ThreadIndex)
      return makeError("device GPU thread %u has an invalid factory slice",
                       ThreadIndex);
    Results.push_back(
        {ThreadIndex, Slice->Error, Slice->ResultSize, Slice->ResultBits});
  }
  return Results;
}

} // namespace inputgen_gpu
