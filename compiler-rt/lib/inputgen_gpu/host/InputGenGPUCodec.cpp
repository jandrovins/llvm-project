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

constexpr uint64_t ResultStride = 8;
constexpr uint64_t FactoryHeaderBytes =
    (sizeof(InputGenGPUFactoryHeader) + 7) & ~uint64_t(7);

struct InputRun {
  uint32_t Offset = 0;
  std::vector<uint8_t> Bytes;
};

struct InputObject {
  uint32_t Capacity = 0;
  std::vector<InputRun> Runs;
};

struct InputThread {
  std::vector<InputObject> Objects;
  std::vector<InputGenGPUInputFileRelation> Relations;
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

uint64_t alignTo(uint64_t Value, uint64_t Alignment) {
  return (Value + Alignment - 1) & ~(Alignment - 1);
}

Result<uint32_t> getNumThreads(uint32_t NumTeams, uint32_t NumThreads) {
  if (!NumTeams || !NumThreads ||
      NumTeams > std::numeric_limits<uint32_t>::max() / NumThreads)
    return Error("invalid launch geometry");
  return NumTeams * NumThreads;
}

Result<uint64_t> getFactorySize(uint32_t NumThreads, uint64_t SliceBytes) {
  if (!SliceBytes ||
      NumThreads > (std::numeric_limits<uint64_t>::max() - FactoryHeaderBytes) /
                       SliceBytes)
    return Error("factory allocation size overflows");
  return FactoryHeaderBytes + uint64_t(NumThreads) * SliceBytes;
}

Error validateConfig(const FactoryConfig &Config) {
  Result<uint32_t> Threads = getNumThreads(Config.NumTeams, Config.NumThreads);
  if (!Threads)
    return Threads.error();
  if (!Config.SliceBytes || Config.SliceBytes % 8 ||
      Config.SliceBytes < sizeof(InputGenGPUFactorySliceHeader) ||
      Config.SliceBytes > std::numeric_limits<uint32_t>::max())
    return makeError("factory bytes per GPU thread must be an 8-byte-aligned "
                     "value between %zu and %u",
                     sizeof(InputGenGPUFactorySliceHeader),
                     std::numeric_limits<uint32_t>::max());
  if (!Config.ObjectBytes ||
      Config.ObjectBytes > std::numeric_limits<uint32_t>::max())
    return makeError("object bytes must be a nonzero value no greater than %u",
                     std::numeric_limits<uint32_t>::max());
  if (!Config.ObjectsPerThread)
    return Error("configured objects per GPU thread must be nonzero");
  uint64_t MinimumTableBytes =
      alignTo(sizeof(InputGenGPUFactorySliceHeader), 8) +
      uint64_t(Config.ObjectsPerThread) * sizeof(uint64_t) +
      uint64_t(Config.ObjectsPerThread - 1) *
          sizeof(InputGenGPUFactoryPointerRelation);
  if (MinimumTableBytes > Config.SliceBytes)
    return Error("configured object tables do not fit in each GPU thread's "
                 "factory slice");
  return Error();
}

Error validateThreadLayout(const InputThread &Thread, uint64_t SliceBytes,
                           uint32_t ObjectLimit, uint32_t RelationLimit) {
  if (Thread.Objects.empty() || Thread.Objects.size() > ObjectLimit ||
      Thread.Relations.size() > RelationLimit ||
      Thread.Relations.size() + 1 != Thread.Objects.size() ||
      RelationLimit + 1 != ObjectLimit)
    return Error("invalid InputGen object relationship layout");

  for (const InputGenGPUInputFileRelation &Relation : Thread.Relations) {
    if (Relation.OwnerObject >= Thread.Objects.size() ||
        Relation.TargetObject >= Thread.Objects.size() ||
        Relation.SlotOffset > Thread.Objects[Relation.OwnerObject].Capacity ||
        sizeof(uint64_t) > Thread.Objects[Relation.OwnerObject].Capacity -
                               Relation.SlotOffset ||
        Relation.TargetOffset > Thread.Objects[Relation.TargetObject].Capacity)
      return Error("invalid InputGen pointer relationship");
  }

  uint64_t LayoutBytes =
      alignTo(sizeof(InputGenGPUFactorySliceHeader), 8) +
      uint64_t(ObjectLimit) * sizeof(uint64_t) +
      uint64_t(RelationLimit) * sizeof(InputGenGPUFactoryPointerRelation);
  return LayoutBytes <= SliceBytes
             ? Error()
             : Error("InputGen tables do not fit in slice");
}

FactoryConfig configFromHeader(const InputGenGPUInputFileHeader &Header) {
  return FactoryConfig{Header.NumTeams,  Header.NumThreads, Header.SliceBytes,
                       Header.ObjectBytes, Header.ObjectsPerThread};
}

void initializeFactoryHeader(std::vector<uint8_t> &Bytes,
                             const FactoryConfig &Config, Mode ExecutionMode) {
  auto *Header = reinterpret_cast<InputGenGPUFactoryHeader *>(Bytes.data());
  Header->Magic = INPUTGEN_GPU_FACTORY_SLICE_MAGIC;
  Header->Version = INPUTGEN_GPU_FACTORY_VERSION;
  Header->Mode = static_cast<uint32_t>(ExecutionMode);
  Header->NumTeams = Config.NumTeams;
  Header->ThreadsPerTeam = Config.NumThreads;
  Header->NumLanes = Config.NumTeams * Config.NumThreads;
  Header->ObjectsPerThread = Config.ObjectsPerThread;
  Header->SliceBytes = Config.SliceBytes;
  Header->ObjectBytes = Config.ObjectBytes;
  Header->FactoryBytes = Bytes.size();
}

} // namespace

Result<Factory> createGenerationFactory(const FactoryConfig &RequestedConfig) {
  FactoryConfig Config = RequestedConfig;
  if (Error Failure = validateConfig(Config))
    return Failure;
  Result<uint32_t> Threads = getNumThreads(Config.NumTeams, Config.NumThreads);
  Result<uint64_t> Size = getFactorySize(Threads.value(), Config.SliceBytes);
  if (!Size)
    return Size.error();
  std::vector<uint8_t> Bytes(Size.value(), 0);
  initializeFactoryHeader(Bytes, Config, Mode::Generate);
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
    InputGenGPUInputFileLaneHeader ThreadHeader{
        static_cast<uint32_t>(Thread.Objects.size()),
        static_cast<uint32_t>(Thread.Relations.size())};
    if (Error Failure = writeValue(File.get(), ThreadHeader))
      return Failure;
    for (const InputObject &Object : Thread.Objects) {
      InputGenGPUInputFileObjectHeader ObjectHeader{
          Object.Capacity, static_cast<uint32_t>(Object.Runs.size())};
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
    for (const InputGenGPUInputFileRelation &Relation : Thread.Relations)
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
  Result<uint32_t> Threads =
      getNumThreads(Storage.Header.NumTeams, Storage.Header.NumThreads);
  if (!Threads || Threads.value() != Storage.Header.NumLanes ||
      !Storage.Header.SliceBytes || !Storage.Header.ObjectBytes ||
      !Storage.Header.ObjectsPerThread ||
      Storage.Header.ObjectLimit != Storage.Header.ObjectsPerThread ||
      Storage.Header.RelationLimit + 1 != Storage.Header.ObjectLimit ||
      Storage.Header.ResultStride != ResultStride)
    return makeError("invalid InputGen data file '%s'", Filename.c_str());

  Storage.Threads.resize(Storage.Header.NumLanes);
  for (InputThread &Thread : Storage.Threads) {
    InputGenGPUInputFileLaneHeader ThreadHeader{};
    if (Error Failure = readValue(File.get(), ThreadHeader))
      return Failure;
    if (!ThreadHeader.ObjectCount)
      return makeError("invalid InputGen data file '%s'", Filename.c_str());
    Thread.Objects.resize(ThreadHeader.ObjectCount);
    Thread.Relations.resize(ThreadHeader.RelationCount);
    for (size_t ObjectIndex = 0; ObjectIndex < Thread.Objects.size();
         ++ObjectIndex) {
      InputObject &Object = Thread.Objects[ObjectIndex];
      InputGenGPUInputFileObjectHeader ObjectHeader{};
      if (Error Failure = readValue(File.get(), ObjectHeader))
        return Failure;
      if (!ObjectHeader.Capacity && ObjectIndex != 0)
        return makeError("invalid InputGen data file '%s'", Filename.c_str());
      Object.Capacity = ObjectHeader.Capacity;
      Object.Runs.resize(ObjectHeader.NumRuns);
      uint32_t PreviousEnd = 0;
      for (InputRun &Run : Object.Runs) {
        InputGenGPUInputFileRunHeader RunHeader{};
        if (Error Failure = readValue(File.get(), RunHeader))
          return Failure;
        if (RunHeader.Offset > Object.Capacity ||
            RunHeader.Size > Object.Capacity - RunHeader.Offset ||
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
    for (InputGenGPUInputFileRelation &Relation : Thread.Relations)
      if (Error Failure = readValue(File.get(), Relation))
        return Failure;
    if (Error Failure =
            validateThreadLayout(Thread, Storage.Header.SliceBytes,
                                 Storage.Header.ObjectLimit,
                                 Storage.Header.RelationLimit))
      return Failure;
  }
  if (std::fgetc(File.get()) != EOF)
    return makeError("invalid trailing data in InputGen data file '%s'",
                     Filename.c_str());
  return Storage;
}

Result<Record> serializeFactory(const Factory &Value) {
  const FactoryConfig &Config = Value.config();
  if (Error Failure = validateConfig(Config))
    return Failure;

  Result<uint32_t> NumThreads =
      getNumThreads(Config.NumTeams, Config.NumThreads);
  if (!NumThreads)
    return NumThreads.error();
  const uint8_t *Bytes = Value.data();
  Record Storage;
  Storage.Header = {INPUTGEN_GPU_INPUT_MAGIC,
                     INPUTGEN_GPU_FACTORY_VERSION,
                     Config.NumTeams,
                     Config.NumThreads,
                     NumThreads.value(),
                     Config.ObjectsPerThread,
                     0,
                     0,
                     Config.SliceBytes,
                     Config.ObjectBytes,
                     ResultStride};
  Storage.Threads.resize(NumThreads.value());

  for (uint32_t ThreadIndex = 0; ThreadIndex < NumThreads.value();
       ++ThreadIndex) {
    uint64_t SliceOffset =
        FactoryHeaderBytes + uint64_t(ThreadIndex) * Config.SliceBytes;
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
    if (Slice->ObjectCount > Slice->ObjectLimit)
      return makeError("device GPU thread %u created %u of %u objects",
                       ThreadIndex, Slice->ObjectCount, Slice->ObjectLimit);
    if (Slice->RelationCount + 1 != Slice->ObjectCount ||
        Slice->RelationCount > Slice->RelationLimit ||
        Slice->RelationLimit + 1 != Slice->ObjectLimit)
      return makeError("device GPU thread %u has inconsistent pointer "
                       "relations",
                       ThreadIndex);
    if (Slice->ObjectLimit != Config.ObjectsPerThread)
      return makeError("device GPU thread %u has invalid object capacity",
                       ThreadIndex);
    if (ThreadIndex == 0) {
      Storage.Header.ObjectLimit = Slice->ObjectLimit;
      Storage.Header.RelationLimit = Slice->RelationLimit;
    } else if (Slice->ObjectLimit != Storage.Header.ObjectLimit ||
               Slice->RelationLimit != Storage.Header.RelationLimit) {
      return makeError("device GPU thread %u has inconsistent capacity",
                       ThreadIndex);
    }

    InputThread &Thread = Storage.Threads[ThreadIndex];
    Thread.Objects.reserve(Slice->ObjectCount);
    for (uint32_t ObjectIndex = 0; ObjectIndex < Slice->ObjectCount;
         ++ObjectIndex) {
      uint64_t TableOffset =
          Slice->ObjectTableOffset + uint64_t(ObjectIndex) * sizeof(uint64_t);
      if (TableOffset > Config.SliceBytes ||
          sizeof(uint64_t) > Config.SliceBytes - TableOffset)
        return Error("invalid object table in device factory");
      uint64_t ObjectOffset = *reinterpret_cast<const uint64_t *>(
          Bytes + SliceOffset + TableOffset);
      if (ObjectOffset > Config.SliceBytes ||
          sizeof(InputGenGPUFactoryObjectHeader) >
              Config.SliceBytes - ObjectOffset)
        return Error("invalid object layout in device factory");
      auto *Object = reinterpret_cast<const InputGenGPUFactoryObjectHeader *>(
          Bytes + SliceOffset + ObjectOffset);
      if (Object->Magic != INPUTGEN_GPU_FACTORY_OBJECT_MAGIC ||
          Object->ObjectIndex != ObjectIndex)
        return Error("invalid object layout in device factory");
      uint64_t TotalSize = sizeof(*Object) + 3ull * Object->Capacity;
      if (TotalSize > Config.SliceBytes - ObjectOffset)
        return Error("invalid object capacity in device factory");

      const uint8_t *Data = reinterpret_cast<const uint8_t *>(Object + 1);
      const uint8_t *Mask = Data + Object->Capacity;
      const uint8_t *Saved = Mask + Object->Capacity;
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
      Thread.Objects.push_back(std::move(SerializedObject));
    }

    uint64_t RelationBytes = uint64_t(Slice->RelationCount) *
                             sizeof(InputGenGPUFactoryPointerRelation);
    if (Slice->RelationTableOffset > Config.SliceBytes ||
        RelationBytes > Config.SliceBytes - Slice->RelationTableOffset)
      return Error("invalid pointer relationship table in device factory");
    auto *Relations =
        reinterpret_cast<const InputGenGPUFactoryPointerRelation *>(
            Bytes + SliceOffset + Slice->RelationTableOffset);
    Thread.Relations.reserve(Slice->RelationCount);
    for (uint32_t I = 0; I < Slice->RelationCount; ++I)
      Thread.Relations.push_back(
          {Relations[I].OwnerObject, Relations[I].SlotOffset,
           Relations[I].TargetObject, Relations[I].TargetOffset});
    if (Error Failure = validateThreadLayout(Thread, Config.SliceBytes,
                                             Storage.Header.ObjectLimit,
                                             Storage.Header.RelationLimit))
      return Failure;
  }
  return Storage;
}

} // namespace

Result<Factory> createReplayFactory(const std::string &Filename,
                                    const ReplayRequest &Request) {
  Result<Record> RecordOrErr = readRecord(Filename);
  if (!RecordOrErr)
    return RecordOrErr.error();
  const Record &Storage = RecordOrErr.value();
  if ((Request.NumTeams && *Request.NumTeams != Storage.Header.NumTeams) ||
      (Request.NumThreads &&
       *Request.NumThreads != Storage.Header.NumThreads) ||
      (Request.SliceBytes &&
       *Request.SliceBytes != Storage.Header.SliceBytes) ||
      (Request.ObjectBytes &&
       *Request.ObjectBytes != Storage.Header.ObjectBytes) ||
      (Request.ObjectsPerThread &&
       *Request.ObjectsPerThread != Storage.Header.ObjectsPerThread))
    return Error("replay options conflict with the InputGen data file");

  FactoryConfig Config = configFromHeader(Storage.Header);
  if (Error Failure = validateConfig(Config))
    return Failure;
  Result<uint64_t> Size =
      getFactorySize(Storage.Header.NumLanes, Storage.Header.SliceBytes);
  if (!Size)
    return Size.error();
  std::vector<uint8_t> Bytes(Size.value(), 0);

  for (uint32_t ThreadIndex = 0; ThreadIndex < Storage.Header.NumLanes;
       ++ThreadIndex) {
    const InputThread &Thread = Storage.Threads[ThreadIndex];
    if (Error Failure = validateThreadLayout(Thread, Storage.Header.SliceBytes,
                                             Storage.Header.ObjectLimit,
                                             Storage.Header.RelationLimit))
      return Failure;
    uint8_t *SliceStart = Bytes.data() + FactoryHeaderBytes +
                          uint64_t(ThreadIndex) * Storage.Header.SliceBytes;
    auto *Slice = reinterpret_cast<InputGenGPUFactorySliceHeader *>(SliceStart);
    Slice->Magic = INPUTGEN_GPU_FACTORY_SLICE_MAGIC;
    Slice->Version = INPUTGEN_GPU_FACTORY_VERSION;
    Slice->Mode = INPUTGEN_MODE_REPLAY;
    Slice->SliceIndex = ThreadIndex;
    Slice->ObjectCount = static_cast<uint32_t>(Thread.Objects.size());
    Slice->ObjectLimit = Storage.Header.ObjectLimit;
    Slice->RelationCount = static_cast<uint32_t>(Thread.Relations.size());
    Slice->RelationLimit = Storage.Header.RelationLimit;
    Slice->ArgumentBytes = Thread.Objects.front().Capacity;
    Slice->ObjectBytes = Storage.Header.ObjectBytes;
    Slice->ObjectTableOffset = alignTo(sizeof(*Slice), 8);
    Slice->RelationTableOffset =
        Slice->ObjectTableOffset +
        uint64_t(Slice->ObjectLimit) * sizeof(uint64_t);
    uint64_t ObjectOffset =
        alignTo(Slice->RelationTableOffset +
                    uint64_t(Slice->RelationLimit) *
                        sizeof(InputGenGPUFactoryPointerRelation),
                8);
    for (uint32_t ObjectIndex = 0; ObjectIndex < Thread.Objects.size();
         ++ObjectIndex) {
      const InputObject &SerializedObject = Thread.Objects[ObjectIndex];
      uint64_t TotalSize = sizeof(InputGenGPUFactoryObjectHeader) +
                           3ull * SerializedObject.Capacity;
      if ((!SerializedObject.Capacity && ObjectIndex != 0) ||
          ObjectOffset > Storage.Header.SliceBytes ||
          TotalSize > Storage.Header.SliceBytes - ObjectOffset)
        return Error("InputGen replay object does not fit in its slice");
      auto *Object = reinterpret_cast<InputGenGPUFactoryObjectHeader *>(
          SliceStart + ObjectOffset);
      Object->Magic = INPUTGEN_GPU_FACTORY_OBJECT_MAGIC;
      Object->ObjectIndex = ObjectIndex;
      Object->Capacity = SerializedObject.Capacity;
      Object->SliceOffset = ObjectOffset;
      *reinterpret_cast<uint64_t *>(SliceStart + Slice->ObjectTableOffset +
                                    ObjectIndex * sizeof(uint64_t)) =
          ObjectOffset;
      uint8_t *Data = reinterpret_cast<uint8_t *>(Object + 1);
      uint8_t *Mask = Data + Object->Capacity;
      for (const InputRun &Run : SerializedObject.Runs) {
        if (Run.Offset > Object->Capacity ||
            Run.Bytes.size() > Object->Capacity - Run.Offset)
          return Error("invalid replay run");
        std::memcpy(Data + Run.Offset, Run.Bytes.data(), Run.Bytes.size());
        std::memset(Mask + Run.Offset, INPUTGEN_GPU_MASK_READ,
                    Run.Bytes.size());
      }
      ObjectOffset = alignTo(ObjectOffset + TotalSize, 8);
    }
    auto *Relations = reinterpret_cast<InputGenGPUFactoryPointerRelation *>(
        SliceStart + Slice->RelationTableOffset);
    for (uint32_t I = 0; I < Thread.Relations.size(); ++I)
      Relations[I] = {
          Thread.Relations[I].OwnerObject, Thread.Relations[I].SlotOffset,
          Thread.Relations[I].TargetObject, Thread.Relations[I].TargetOffset};
    Slice->NextOffset = ObjectOffset;
  }

  initializeFactoryHeader(Bytes, Config, Mode::Replay);
  return Factory(Mode::Replay, Config, std::move(Bytes));
}

Error writeGenerationRecord(const std::string &Filename,
                            const Factory &Value) {
  if (Value.ExecutionMode != Mode::Generate)
    return Error("only a generation factory can be serialized");
  Result<Record> RecordOrErr = serializeFactory(Value);
  if (!RecordOrErr)
    return RecordOrErr.error();
  return writeRecord(Filename, RecordOrErr.value());
}

Result<std::vector<ThreadResult>> inspectThreadResults(const Factory &Value) {
  const FactoryConfig &Config = Value.config();
  Result<uint32_t> NumThreads =
      getNumThreads(Config.NumTeams, Config.NumThreads);
  if (!NumThreads)
    return NumThreads.error();
  const uint8_t *Bytes = Value.data();
  Result<uint64_t> ExpectedSize =
      getFactorySize(NumThreads.value(), Config.SliceBytes);
  if (!ExpectedSize || ExpectedSize.value() != Value.size())
    return Error("factory copy has an invalid size");

  std::vector<ThreadResult> Results;
  Results.reserve(NumThreads.value());
  for (uint32_t ThreadIndex = 0; ThreadIndex < NumThreads.value();
       ++ThreadIndex) {
    uint64_t Offset =
        FactoryHeaderBytes + uint64_t(ThreadIndex) * Config.SliceBytes;
    auto *Slice = reinterpret_cast<const InputGenGPUFactorySliceHeader *>(
        Bytes + Offset);
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
