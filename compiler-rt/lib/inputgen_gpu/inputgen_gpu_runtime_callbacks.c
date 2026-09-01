//===-- InputGen GPU Runtime Device Callbacks ----------------------------===//

#include "inputgen_gpu_runtime_internal.h"

InputGenGPUFactorySliceHeader *__ig_current_slice(void);
InputGenGPUFactoryObjectHeader *__ig_get_object(uint32_t);
InputGenGPUFactoryObjectHeader *__ig_allocate_object(uint32_t);
InputGenGPUFactoryPointerRelation *__ig_relations(void);
uint64_t __ig_current_slice_bytes(void);

static void setError(InputGenGPUFactorySliceHeader *Slice, uint32_t Error) {
  if (Slice && Slice->Error == INPUTGEN_GPU_FACTORY_ERROR_NONE)
    Slice->Error = Error;
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

static InputGenGPUFactoryObjectHeader *
findObject(void *Pointer, int32_t PointerAS, int64_t Size, int64_t Alignment,
           uint32_t *ObjectIndex, uint32_t *OffsetOut, int *IsFactoryAddress) {
  // Resolve a real AS0 address against allocated object-data ranges in the
  // current GPU thread's fixed slice. External addresses pass through.
  InputGenGPUFactorySliceHeader *Slice = __ig_current_slice();
  *IsFactoryAddress = 0;
  if (!Slice || PointerAS != 0)
    return 0;

  uintptr_t Address = (uintptr_t)Pointer;
  uintptr_t SliceBegin = (uintptr_t)Slice;
  uint64_t SliceBytes = __ig_current_slice_bytes();
  if (Address >= SliceBegin && Address - SliceBegin < SliceBytes)
    *IsFactoryAddress = 1;

  if (Size <= 0 || Size > 8 || Alignment <= 0 ||
      ((uint64_t)Alignment & ((uint64_t)Alignment - 1)) != 0) {
    if (*IsFactoryAddress)
      setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_ACCESS);
    return 0;
  }

  for (uint32_t Index = 0; Index < Slice->ObjectCount; ++Index) {
    InputGenGPUFactoryObjectHeader *Object = __ig_get_object(Index);
    if (!Object)
      return 0;
    uintptr_t Data = (uintptr_t)(Object + 1);
    if (Address < Data)
      continue;
    uint64_t Offset = Address - Data;
    if (Offset > Object->Capacity || (uint64_t)Size > Object->Capacity - Offset)
      continue;
    if ((Address & ((uintptr_t)Alignment - 1)) != 0) {
      setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_ACCESS);
      return 0;
    }
    *IsFactoryAddress = 1;
    *ObjectIndex = Index;
    *OffsetOut = (uint32_t)Offset;
    return Object;
  }

  if (*IsFactoryAddress)
    setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_ACCESS);
  return 0;
}

static void *dataAddress(InputGenGPUFactoryObjectHeader *Object,
                         uint32_t Offset) {
  return (char *)(Object + 1) + Offset;
}

static InputGenGPUFactoryPointerRelation *
findRelation(InputGenGPUFactorySliceHeader *Slice, uint32_t Owner,
             uint32_t Offset) {
  // Find the target object already assigned to this pointer slot.
  InputGenGPUFactoryPointerRelation *Relations = __ig_relations();
  for (uint32_t I = 0; Relations && I < Slice->RelationCount; ++I)
    if (Relations[I].OwnerObject == Owner && Relations[I].SlotOffset == Offset)
      return &Relations[I];
  return 0;
}

void *__ig_pre_load(void *Pointer, int32_t PointerAS, int64_t ValueSize,
                    int64_t Alignment, int32_t ValueTypeId) {
  InputGenGPUFactorySliceHeader *Slice = __ig_current_slice();
  uint32_t Owner = 0, Offset = 0;
  int IsFactoryAddress = 0;
  InputGenGPUFactoryObjectHeader *Object =
      findObject(Pointer, PointerAS, ValueSize, Alignment, &Owner, &Offset,
                 &IsFactoryAddress);
  if (!Object)
    return IsFactoryAddress ? failureAddress(Slice, Pointer) : Pointer;

  char *Data = (char *)(Object + 1);
  unsigned char *Mask = (unsigned char *)(Data + Object->Capacity);
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
      if (!__ig_allocate_object(Target))
        return failureAddress(Slice, Pointer);
      Relation = &__ig_relations()[Slice->RelationCount++];
      Relation->OwnerObject = Owner;
      Relation->SlotOffset = Offset;
      Relation->TargetObject = Target;
      Relation->TargetOffset = 0;
    }
    InputGenGPUFactoryObjectHeader *Target =
        __ig_get_object(Relation->TargetObject);
    if (!Target || Relation->TargetOffset > Target->Capacity)
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
  InputGenGPUFactorySliceHeader *Slice = __ig_current_slice();
  if (ValueTypeId == INPUTGEN_GPU_VALUE_POINTER) {
    setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_TYPE);
    return failureAddress(Slice, Pointer);
  }
  uint32_t Owner = 0, Offset = 0;
  int IsFactoryAddress = 0;
  InputGenGPUFactoryObjectHeader *Object =
      findObject(Pointer, PointerAS, ValueSize, Alignment, &Owner, &Offset,
                 &IsFactoryAddress);
  if (!Object)
    return IsFactoryAddress ? failureAddress(Slice, Pointer) : Pointer;
  char *Data = (char *)(Object + 1);
  unsigned char *Mask = (unsigned char *)(Data + Object->Capacity);
  char *Saved = (char *)(Mask + Object->Capacity);
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
