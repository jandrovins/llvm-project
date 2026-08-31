//===-- InputGen GPU Runtime State ---------------------------------------===//

#include "inputgen_gpu_runtime_internal.h"

// The AMDGPU backend cannot lower private-address-space globals. Keep the one
// launch-wide context word in AS1 and derive each GPU thread's slice from its
// hardware IDs. User-visible pointers remain encoded AS0 values.
static __attribute__((address_space(1))) uint64_t FactoryContextBits;

static uint64_t alignTo(uint64_t Value, uint64_t Alignment);

// Recover the launch-wide factory selected by this GPU thread's entry wrapper.
static InputGenGPUFactoryHeader *currentFactory(void) {
  return (InputGenGPUFactoryHeader *)(uintptr_t)FactoryContextBits;
}

static InputGenGPUFactorySliceHeader *currentSlice(void) {
  InputGenGPUFactoryHeader *Factory = currentFactory();
  if (!Factory)
    return 0;
#if defined(__AMDGCN__)
  uint64_t WorkgroupIndex = __builtin_amdgcn_workgroup_id_x();
  uint64_t WorkitemIndex = __builtin_amdgcn_workitem_id_x();
#else
  uint64_t WorkgroupIndex = 0;
  uint64_t WorkitemIndex = 0;
#endif
  // Map the hardware workgroup/workitem pair to this GPU thread's slice.
  if (WorkitemIndex >= Factory->ThreadsPerTeam ||
      WorkgroupIndex > (UINT64_MAX - WorkitemIndex) / Factory->ThreadsPerTeam)
    return 0;
  uint64_t LaneIndex = WorkgroupIndex * Factory->ThreadsPerTeam + WorkitemIndex;
  if (LaneIndex >= Factory->NumLanes)
    return 0;
  return (InputGenGPUFactorySliceHeader *)((char *)Factory +
                                           alignTo(sizeof(*Factory), 8) +
                                           LaneIndex * Factory->SliceBytes);
}

static uint64_t alignTo(uint64_t Value, uint64_t Alignment) {
  return (Value + Alignment - 1) & ~(Alignment - 1);
}

static void setError(InputGenGPUFactorySliceHeader *Slice, uint32_t Error) {
  // Preserve the first error so the launcher reports the original failure.
  if (Slice->Error == INPUTGEN_GPU_FACTORY_ERROR_NONE)
    Slice->Error = Error;
}

static uint64_t *objectTable(InputGenGPUFactorySliceHeader *Slice) {
  return (uint64_t *)((char *)Slice + Slice->ObjectTableOffset);
}

static int rangeInSlice(uint64_t Offset, uint64_t Size, uint64_t SliceBytes) {
  return Offset <= SliceBytes && Size <= SliceBytes - Offset;
}

static InputGenGPUFactoryPointerRelation *
relationTable(InputGenGPUFactorySliceHeader *Slice) {
  return (InputGenGPUFactoryPointerRelation *)((char *)Slice +
                                               Slice->RelationTableOffset);
}

static InputGenGPUFactoryObjectHeader *
getObject(InputGenGPUFactorySliceHeader *Slice, uint32_t ObjectIndex) {
  // Resolve a logical object index through the per-slice offset table.
  if (ObjectIndex >= Slice->ObjectCount) {
    setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_LAYOUT);
    return 0;
  }
  uint64_t Offset = objectTable(Slice)[ObjectIndex];
  InputGenGPUFactoryObjectHeader *Object =
      Offset ? (InputGenGPUFactoryObjectHeader *)((char *)Slice + Offset) : 0;
  if (!Object || Object->Magic != INPUTGEN_GPU_FACTORY_OBJECT_MAGIC ||
      Object->ObjectIndex != ObjectIndex ||
      !rangeInSlice(Offset, sizeof(*Object) + 3ull * Object->Capacity,
                    currentFactory()->SliceBytes)) {
    setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_LAYOUT);
    return 0;
  }
  return Object;
}

static InputGenGPUFactoryObjectHeader *
allocateObject(InputGenGPUFactorySliceHeader *Slice, uint64_t SliceBytes,
               uint32_t ObjectIndex, uint64_t Capacity) {
  // Reserve the next fixed-capacity object record without growing the slice.
  if (ObjectIndex != Slice->ObjectCount || ObjectIndex >= Slice->ObjectLimit ||
      ObjectIndex > INPUTGEN_GPU_VPTR_OBJECT_MASK || Capacity > UINT32_MAX) {
    setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_CAPACITY);
    return 0;
  }
  uint64_t Offset = alignTo(Slice->NextOffset, 8);
  uint64_t Total = sizeof(InputGenGPUFactoryObjectHeader) + 3 * Capacity;
  if (Offset > SliceBytes || Total > SliceBytes - Offset) {
    setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_CAPACITY);
    return 0;
  }
  InputGenGPUFactoryObjectHeader *Object =
      (InputGenGPUFactoryObjectHeader *)((char *)Slice + Offset);
  Object->Magic = INPUTGEN_GPU_FACTORY_OBJECT_MAGIC;
  Object->ObjectIndex = ObjectIndex;
  Object->Capacity = (uint32_t)Capacity;
  Object->SliceOffset = (uint32_t)Offset;
  objectTable(Slice)[ObjectIndex] = Offset;
  Slice->NextOffset = Offset + Total;
  ++Slice->ObjectCount;
  return Object;
}

static void *encodePointer(uint32_t ObjectIndex, int64_t Offset) {
  // Keep object identity and pointer arithmetic in an AS0 handle value.
  uint64_t Field = (uint64_t)(Offset + (int64_t)INPUTGEN_GPU_VPTR_OFFSET_BIAS);
  uint64_t Bits = ((uint64_t)INPUTGEN_GPU_VPTR_MAGIC << 60) |
                  ((uint64_t)ObjectIndex << INPUTGEN_GPU_VPTR_OFFSET_BITS) |
                  (Field & (INPUTGEN_GPU_VPTR_OFFSET_BIAS * 2 - 1));
  return (void *)(uintptr_t)Bits;
}

void *__ig_prepare_lane(void *Context, uint64_t WorkgroupIndex,
                        uint64_t WorkitemIndex, uint64_t ArgumentBytes,
                        uint32_t PointerArgumentCount) {
  InputGenGPUFactoryHeader *Factory = (InputGenGPUFactoryHeader *)Context;
  if (!Factory || Factory->Magic != INPUTGEN_GPU_FACTORY_SLICE_MAGIC ||
      Factory->Version != INPUTGEN_GPU_FACTORY_VERSION ||
      WorkitemIndex >= Factory->ThreadsPerTeam ||
      PointerArgumentCount == UINT32_MAX)
    return 0;
  if (WorkgroupIndex > (UINT64_MAX - WorkitemIndex) / Factory->ThreadsPerTeam)
    return 0;
  uint64_t LaneIndex = WorkgroupIndex * Factory->ThreadsPerTeam + WorkitemIndex;
  if (LaneIndex >= Factory->NumLanes)
    return 0;
  InputGenGPUFactorySliceHeader *Slice =
      (InputGenGPUFactorySliceHeader *)((char *)Factory +
                                        alignTo(sizeof(*Factory), 8) +
                                        LaneIndex * Factory->SliceBytes);
  FactoryContextBits = (uint64_t)(uintptr_t)Factory;

  if (Factory->Mode == INPUTGEN_MODE_GENERATE) {
    // Initialize tables and object zero; pointer loads allocate later objects.
    __builtin_memset(Slice, 0, Factory->SliceBytes);
    Slice->Magic = INPUTGEN_GPU_FACTORY_SLICE_MAGIC;
    Slice->Version = INPUTGEN_GPU_FACTORY_VERSION;
    Slice->Mode = Factory->Mode;
    Slice->SliceIndex = (uint32_t)LaneIndex;
    Slice->ObjectLimit = PointerArgumentCount + 1;
    Slice->RelationLimit = PointerArgumentCount;
    Slice->ArgumentBytes = ArgumentBytes;
    Slice->ObjectBytes = Factory->ObjectBytes;
    Slice->ObjectTableOffset = alignTo(sizeof(*Slice), 8);
    Slice->RelationTableOffset =
        Slice->ObjectTableOffset +
        (uint64_t)Slice->ObjectLimit * sizeof(uint64_t);
    Slice->NextOffset =
        alignTo(Slice->RelationTableOffset +
                    (uint64_t)Slice->RelationLimit *
                        sizeof(InputGenGPUFactoryPointerRelation),
                8);
    if (Slice->NextOffset > Factory->SliceBytes) {
      setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_CAPACITY);
      return 0;
    }
    if (!allocateObject(Slice, Factory->SliceBytes, 0, ArgumentBytes))
      return 0;
  } else if (Factory->Mode == INPUTGEN_MODE_REPLAY) {
    // Accept only the host-reconstructed layout from the matching recording.
    if (Slice->Magic != INPUTGEN_GPU_FACTORY_SLICE_MAGIC ||
        Slice->Version != INPUTGEN_GPU_FACTORY_VERSION ||
        Slice->SliceIndex != LaneIndex ||
        Slice->ArgumentBytes != ArgumentBytes ||
        Slice->ObjectCount != PointerArgumentCount + 1 ||
        Slice->ObjectLimit != PointerArgumentCount + 1 ||
        Slice->RelationCount != PointerArgumentCount ||
        Slice->RelationLimit != PointerArgumentCount ||
        !rangeInSlice(Slice->ObjectTableOffset,
                      (uint64_t)Slice->ObjectLimit * sizeof(uint64_t),
                      Factory->SliceBytes) ||
        !rangeInSlice(Slice->RelationTableOffset,
                      (uint64_t)Slice->RelationLimit *
                          sizeof(InputGenGPUFactoryPointerRelation),
                      Factory->SliceBytes)) {
      setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_LAYOUT);
      return 0;
    }
    Slice->Mode = Factory->Mode;
  } else {
    return 0;
  }
  return encodePointer(0, 0);
}

void __ig_store_result(uint64_t Bits, uint32_t Size) {
  // Keep each user-function return value in its owning GPU thread's slice.
  InputGenGPUFactorySliceHeader *Slice = currentSlice();
  if (Slice) {
    Slice->ResultBits = Bits;
    Slice->ResultSize = Size;
  }
}

InputGenGPUFactorySliceHeader *__ig_current_slice(void) {
  return currentSlice();
}
InputGenGPUFactoryObjectHeader *__ig_get_object(uint32_t Index) {
  InputGenGPUFactorySliceHeader *Slice = currentSlice();
  return Slice ? getObject(Slice, Index) : 0;
}
InputGenGPUFactoryObjectHeader *__ig_allocate_object(uint32_t Index) {
  InputGenGPUFactorySliceHeader *Slice = currentSlice();
  InputGenGPUFactoryHeader *Factory = currentFactory();
  return Slice && Factory ? allocateObject(Slice, Factory->SliceBytes, Index,
                                           Slice->ObjectBytes)
                          : 0;
}
InputGenGPUFactoryPointerRelation *__ig_relations(void) {
  InputGenGPUFactorySliceHeader *Slice = currentSlice();
  return Slice ? relationTable(Slice) : 0;
}
