//===-- InputGen GPU Runtime State ---------------------------------------===//

#include "inputgen_gpu_runtime_internal.h"

// Keep the launch-wide context word in AS1, a device-global address space on
// the supported targets, and derive each GPU thread's slice from its hardware
// IDs. User-visible pointers are generic AS0 addresses into the fixed slice.
static __attribute__((address_space(1))) uint64_t FactoryContextBits;

static uint64_t alignTo(uint64_t Value, uint64_t Alignment);

// Recover the launch-wide factory selected by this GPU thread's entry wrapper.
static InputGenGPUFactoryHeader *currentFactory(void) {
  return (InputGenGPUFactoryHeader *)(uintptr_t)FactoryContextBits;
}

static InputGenGPUFactorySliceHeader *
getCurrentSlice(InputGenGPUFactoryHeader *Factory, uint64_t *ThreadIndex) {
  if (!Factory)
    return 0;
#if defined(__AMDGCN__)
  uint64_t WorkgroupIndex = __builtin_amdgcn_workgroup_id_x();
  uint64_t WorkitemIndex = __builtin_amdgcn_workitem_id_x();
#elif defined(__NVPTX__)
  uint64_t WorkgroupIndex = __nvvm_read_ptx_sreg_ctaid_x();
  uint64_t WorkitemIndex = __nvvm_read_ptx_sreg_tid_x();
#else
  uint64_t WorkgroupIndex = 0;
  uint64_t WorkitemIndex = 0;
#endif
  // Map the hardware workgroup/workitem pair to this GPU thread's slice.
  if (WorkitemIndex >= Factory->ThreadsPerTeam ||
      WorkgroupIndex > (UINT64_MAX - WorkitemIndex) / Factory->ThreadsPerTeam)
    return 0;
  uint64_t Index = WorkgroupIndex * Factory->ThreadsPerTeam + WorkitemIndex;
  if (Index >= Factory->NumLanes)
    return 0;
  if (ThreadIndex)
    *ThreadIndex = Index;
  return (InputGenGPUFactorySliceHeader *)((char *)Factory +
                                           alignTo(sizeof(*Factory), 8) +
                                           Index * Factory->SliceBytes);
}

static InputGenGPUFactorySliceHeader *currentSlice(void) {
  return getCurrentSlice(currentFactory(), 0);
}

static uint64_t alignTo(uint64_t Value, uint64_t Alignment) {
  return (Value + Alignment - 1) & ~(Alignment - 1);
}

static int getFixedSliceBytes(uint64_t ObjectBytes, uint32_t ObjectLimit,
                              uint64_t *SliceBytes) {
  if (!ObjectBytes || ObjectBytes > UINT32_MAX || (ObjectBytes & 7) ||
      !ObjectLimit)
    return 0;
  uint64_t RelationBytes =
      (uint64_t)(ObjectLimit - 1) * sizeof(InputGenGPUFactoryPointerRelation);
  uint64_t ObjectOffset = alignTo(
      alignTo(sizeof(InputGenGPUFactorySliceHeader), 8) + RelationBytes, 8);
  uint64_t SlotBytes = 3 * ObjectBytes;
  if ((uint64_t)ObjectLimit > (UINT64_MAX - ObjectOffset) / SlotBytes)
    return 0;
  *SliceBytes = ObjectOffset + (uint64_t)ObjectLimit * SlotBytes;
  return 1;
}

static void setError(InputGenGPUFactorySliceHeader *Slice, uint32_t Error) {
  // Preserve the first error so the launcher reports the original failure.
  if (Slice && Slice->Error == INPUTGEN_GPU_FACTORY_ERROR_NONE)
    Slice->Error = Error;
}

static int rangeInSlice(uint64_t Offset, uint64_t Size, uint64_t SliceBytes) {
  return Offset <= SliceBytes && Size <= SliceBytes - Offset;
}

static InputGenGPUFactoryPointerRelation *
relationTable(InputGenGPUFactorySliceHeader *Slice) {
  return (InputGenGPUFactoryPointerRelation *)((char *)Slice +
                                               Slice->RelationTableOffset);
}

static uint64_t objectStorageOffset(InputGenGPUFactorySliceHeader *Slice) {
  return alignTo(Slice->RelationTableOffset +
                     (uint64_t)Slice->RelationLimit *
                         sizeof(InputGenGPUFactoryPointerRelation),
                 8);
}

static uint64_t objectCapacity(InputGenGPUFactorySliceHeader *Slice,
                               uint32_t ObjectIndex) {
  return ObjectIndex == 0 ? Slice->ArgumentBytes : Slice->ObjectBytes;
}

static char *getObject(InputGenGPUFactorySliceHeader *Slice,
                       uint32_t ObjectIndex) {
  // Resolve a logical object index to its deterministic fixed slot.
  if (ObjectIndex >= Slice->ObjectCount) {
    setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_LAYOUT);
    return 0;
  }
  uint64_t SlotBytes = 3 * Slice->ObjectBytes;
  uint64_t Offset = objectStorageOffset(Slice) + ObjectIndex * SlotBytes;
  if (!rangeInSlice(Offset, SlotBytes, currentFactory()->SliceBytes)) {
    setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_LAYOUT);
    return 0;
  }
  return (char *)Slice + Offset;
}

static char *allocateObject(InputGenGPUFactorySliceHeader *Slice,
                            uint32_t ObjectIndex) {
  // Make the next preallocated slot logically live without growing the slice.
  if (ObjectIndex != Slice->ObjectCount || ObjectIndex >= Slice->ObjectLimit) {
    setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_CAPACITY);
    return 0;
  }
  ++Slice->ObjectCount;
  return getObject(Slice, ObjectIndex);
}

void *__ig_prepare_thread(void *Context, uint64_t ArgumentBytes) {
  InputGenGPUFactoryHeader *Factory = (InputGenGPUFactoryHeader *)Context;
  uint64_t ExpectedSliceBytes = 0;
  if (!Factory || Factory->Magic != INPUTGEN_GPU_FACTORY_SLICE_MAGIC ||
      Factory->Version != INPUTGEN_GPU_FACTORY_VERSION ||
      !getFixedSliceBytes(Factory->ObjectBytes, Factory->ObjectsPerThread,
                          &ExpectedSliceBytes) ||
      Factory->SliceBytes != ExpectedSliceBytes ||
      ArgumentBytes > Factory->ObjectBytes)
    return 0;
  FactoryContextBits = (uint64_t)(uintptr_t)Factory;
  uint64_t ThreadIndex = 0;
  InputGenGPUFactorySliceHeader *Slice = getCurrentSlice(Factory, &ThreadIndex);
  if (!Slice)
    return 0;

  if (Factory->Mode == INPUTGEN_MODE_GENERATE) {
    // Initialize tables and object zero; pointer loads allocate later objects.
    __builtin_memset(Slice, 0, Factory->SliceBytes);
    Slice->Magic = INPUTGEN_GPU_FACTORY_SLICE_MAGIC;
    Slice->Version = INPUTGEN_GPU_FACTORY_VERSION;
    Slice->Mode = Factory->Mode;
    Slice->SliceIndex = (uint32_t)ThreadIndex;
    Slice->ObjectLimit = Factory->ObjectsPerThread;
    Slice->RelationLimit = Slice->ObjectLimit - 1;
    Slice->ArgumentBytes = ArgumentBytes;
    Slice->ObjectBytes = Factory->ObjectBytes;
    Slice->RelationTableOffset = alignTo(sizeof(*Slice), 8);
    uint64_t ObjectStorageOffset = objectStorageOffset(Slice);
    uint64_t ObjectStorageBytes =
        (uint64_t)Slice->ObjectLimit * 3 * Slice->ObjectBytes;
    if (!rangeInSlice(ObjectStorageOffset, ObjectStorageBytes,
                      Factory->SliceBytes)) {
      setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_CAPACITY);
      return 0;
    }
    if (!allocateObject(Slice, 0))
      return 0;
  } else if (Factory->Mode == INPUTGEN_MODE_REPLAY) {
    // Accept only the host-reconstructed layout from the matching recording.
    if (Slice->Magic != INPUTGEN_GPU_FACTORY_SLICE_MAGIC ||
        Slice->Version != INPUTGEN_GPU_FACTORY_VERSION ||
        Slice->SliceIndex != ThreadIndex ||
        Slice->ArgumentBytes != ArgumentBytes ||
        Slice->ObjectBytes != Factory->ObjectBytes ||
        Slice->ObjectLimit != Factory->ObjectsPerThread ||
        Slice->RelationLimit != Slice->ObjectLimit - 1 || !Slice->ObjectCount ||
        Slice->ObjectCount > Slice->ObjectLimit ||
        Slice->RelationCount > Slice->RelationLimit ||
        Slice->RelationCount + 1 != Slice->ObjectCount ||
        Slice->RelationTableOffset != alignTo(sizeof(*Slice), 8) ||
        !rangeInSlice(Slice->RelationTableOffset,
                      (uint64_t)Slice->RelationLimit *
                          sizeof(InputGenGPUFactoryPointerRelation),
                      Factory->SliceBytes) ||
        !rangeInSlice(objectStorageOffset(Slice),
                      (uint64_t)Slice->ObjectLimit * 3 * Slice->ObjectBytes,
                      Factory->SliceBytes)) {
      setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_LAYOUT);
      return 0;
    }
    Slice->Mode = Factory->Mode;
  } else {
    return 0;
  }
  return getObject(Slice, 0);
}

void __ig_store_result(uint64_t Bits, uint32_t Size) {
  // Keep each user-function return value in its owning GPU thread's slice.
  InputGenGPUFactorySliceHeader *Slice = currentSlice();
  if (Slice) {
    Slice->ResultBits = Bits;
    Slice->ResultSize = Size;
  }
}

int32_t __ig_error_pending(void) {
  InputGenGPUFactorySliceHeader *Slice = currentSlice();
  return Slice && Slice->Error != INPUTGEN_GPU_FACTORY_ERROR_NONE;
}

static void *failureAddress(InputGenGPUFactorySliceHeader *Slice,
                            void *OriginalPointer) {
  // A replaced access must still receive a dereferenceable address after a
  // recorded callback error. The slice header is naturally 8-byte aligned and
  // all supported access sizes are at most eight bytes.
  return Slice ? (void *)&Slice->ErrorScratch : OriginalPointer;
}

static uint64_t generatedBits(int32_t TypeId, int64_t Size, int *IsValid) {
  // Fabricate the deterministic scalar value used for unseen generated input.
  *IsValid = 1;
  if (TypeId == INPUTGEN_GPU_VALUE_INTEGER)
    return 9;
  if (TypeId == INPUTGEN_GPU_VALUE_FLOAT && Size == 4)
    return 0x41100000ULL;
  if (TypeId == INPUTGEN_GPU_VALUE_DOUBLE && Size == 8)
    return 0x4022000000000000ULL;
  *IsValid = 0;
  return 0;
}

static char *findObject(void *Pointer, int32_t PointerAS, int64_t Size,
                        int64_t Alignment, uint32_t *ObjectIndex,
                        uint32_t *OffsetOut, int *IsFactoryAddress) {
  // Resolve a real AS0 address against allocated object-data ranges in the
  // current GPU thread's fixed slice. External addresses pass through.
  InputGenGPUFactorySliceHeader *Slice = currentSlice();
  *IsFactoryAddress = 0;
  if (!Slice || PointerAS != 0)
    return 0;

  uintptr_t Address = (uintptr_t)Pointer;
  uintptr_t SliceBegin = (uintptr_t)Slice;
  uint64_t SliceBytes = currentFactory()->SliceBytes;
  if (Address >= SliceBegin && Address - SliceBegin < SliceBytes)
    *IsFactoryAddress = 1;

  if (Size <= 0 || Size > 8 || Alignment <= 0 ||
      ((uint64_t)Alignment & ((uint64_t)Alignment - 1)) != 0) {
    if (*IsFactoryAddress)
      setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_ACCESS);
    return 0;
  }

  for (uint32_t Index = 0; Index < Slice->ObjectCount; ++Index) {
    char *Data = getObject(Slice, Index);
    if (!Data)
      return 0;
    uintptr_t DataAddress = (uintptr_t)Data;
    if (Address < DataAddress)
      continue;
    uint64_t Capacity = objectCapacity(Slice, Index);
    uint64_t Offset = Address - DataAddress;
    if (Offset > Capacity || (uint64_t)Size > Capacity - Offset)
      continue;
    if ((Address & ((uintptr_t)Alignment - 1)) != 0) {
      setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_ACCESS);
      return 0;
    }
    *IsFactoryAddress = 1;
    *ObjectIndex = Index;
    *OffsetOut = (uint32_t)Offset;
    return Data;
  }

  if (*IsFactoryAddress)
    setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_ACCESS);
  return 0;
}

static void *dataAddress(char *Object, uint32_t Offset) {
  return Object + Offset;
}

static InputGenGPUFactoryPointerRelation *
findRelation(InputGenGPUFactorySliceHeader *Slice, uint32_t Owner,
             uint32_t Offset) {
  // Find the target object already assigned to this pointer slot.
  InputGenGPUFactoryPointerRelation *Relations = relationTable(Slice);
  for (uint32_t I = 0; I < Slice->RelationCount; ++I)
    if (Relations[I].OwnerObject == Owner && Relations[I].SlotOffset == Offset)
      return &Relations[I];
  return 0;
}

void *__ig_pre_load(void *Pointer, int32_t PointerAS, int64_t ValueSize,
                    int64_t Alignment, int32_t ValueTypeId) {
  InputGenGPUFactorySliceHeader *Slice = currentSlice();
  uint32_t Owner = 0, Offset = 0;
  int IsFactoryAddress = 0;
  char *Object = findObject(Pointer, PointerAS, ValueSize, Alignment, &Owner,
                            &Offset, &IsFactoryAddress);
  if (!Object)
    return IsFactoryAddress ? failureAddress(Slice, Pointer) : Pointer;

  char *Data = Object;
  unsigned char *Mask = (unsigned char *)(Data + Slice->ObjectBytes);
  if (ValueTypeId == INPUTGEN_GPU_VALUE_POINTER) {
    // Pointer slots record object relationships, never a serialized address.
    if (ValueSize != (int64_t)sizeof(void *)) {
      setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_TYPE);
      return failureAddress(Slice, Pointer);
    }
    InputGenGPUFactoryPointerRelation *Relation =
        findRelation(Slice, Owner, Offset);
    if (!Relation) {
      // Generation reserves the target object; replay requires its relation.
      if (Slice->Mode != INPUTGEN_MODE_GENERATE) {
        setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_REPLAY);
        return failureAddress(Slice, Pointer);
      }
      if (Slice->RelationCount == Slice->RelationLimit) {
        setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_CAPACITY);
        return failureAddress(Slice, Pointer);
      }
      uint32_t Target = Slice->ObjectCount;
      if (!allocateObject(Slice, Target))
        return failureAddress(Slice, Pointer);
      Relation = &relationTable(Slice)[Slice->RelationCount++];
      Relation->OwnerObject = Owner;
      Relation->SlotOffset = Offset;
      Relation->TargetObject = Target;
      Relation->TargetOffset = 0;
    }
    char *Target = getObject(Slice, Relation->TargetObject);
    if (!Target ||
        Relation->TargetOffset > objectCapacity(Slice, Relation->TargetObject))
      return failureAddress(Slice, Pointer);
    *(void **)(Data + Offset) = dataAddress(Target, Relation->TargetOffset);
    for (uint32_t I = 0; I < (uint32_t)ValueSize; ++I)
      Mask[Offset + I] |= INPUTGEN_GPU_MASK_READ | INPUTGEN_GPU_MASK_POINTER;
    return dataAddress(Object, Offset);
  }

  int IsValid = 0;
  uint64_t Bits = generatedBits(ValueTypeId, ValueSize, &IsValid);
  if (!IsValid) {
    setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_TYPE);
    return failureAddress(Slice, Pointer);
  }
  for (uint32_t I = 0; I < (uint32_t)ValueSize; ++I) {
    unsigned char State = Mask[Offset + I];
    if (State & (INPUTGEN_GPU_MASK_READ | INPUTGEN_GPU_MASK_WRITTEN))
      continue;
    if (Slice->Mode == INPUTGEN_MODE_REPLAY) {
      setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_REPLAY);
      return failureAddress(Slice, Pointer);
    }
    // First reads become input; a prior write supplies the current value.
    Data[Offset + I] = (char)(Bits >> (I * 8));
    Mask[Offset + I] = State | INPUTGEN_GPU_MASK_READ;
  }
  return dataAddress(Object, Offset);
}

void *__ig_pre_store(void *Pointer, int32_t PointerAS, int64_t ValueSize,
                     int64_t Alignment, int32_t ValueTypeId) {
  InputGenGPUFactorySliceHeader *Slice = currentSlice();
  if (ValueTypeId == INPUTGEN_GPU_VALUE_POINTER) {
    setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_TYPE);
    return failureAddress(Slice, Pointer);
  }
  uint32_t Owner = 0, Offset = 0;
  int IsFactoryAddress = 0;
  char *Object = findObject(Pointer, PointerAS, ValueSize, Alignment, &Owner,
                            &Offset, &IsFactoryAddress);
  if (!Object)
    return IsFactoryAddress ? failureAddress(Slice, Pointer) : Pointer;
  char *Data = Object;
  unsigned char *Mask = (unsigned char *)(Data + Slice->ObjectBytes);
  char *Saved = (char *)(Mask + Slice->ObjectBytes);
  // Preserve input before its first overwrite so serialization keeps the read.
  for (uint32_t I = 0; I < (uint32_t)ValueSize; ++I) {
    unsigned char State = Mask[Offset + I];
    if ((State & INPUTGEN_GPU_MASK_READ) &&
        !(State & INPUTGEN_GPU_MASK_WRITTEN))
      Saved[Offset + I] = Data[Offset + I];
    Mask[Offset + I] = State | INPUTGEN_GPU_MASK_WRITTEN;
  }
  return dataAddress(Object, Offset);
}
