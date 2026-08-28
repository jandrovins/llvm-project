//===-- InputGen GPU Entry Runtime Device Callbacks ----------------------===//

#include "inputgen_gpu_entry_internal.h"

InputGenGPUFactorySliceHeader *__ig_current_slice(void);
InputGenGPUFactoryObjectHeader *__ig_get_object(uint32_t);
InputGenGPUFactoryObjectHeader *__ig_allocate_object(uint32_t);
InputGenGPUFactoryPointerRelation *__ig_relations(void);

static void setError(InputGenGPUFactorySliceHeader *Slice, uint32_t Error) {
  if (Slice && Slice->Error == INPUTGEN_GPU_FACTORY_ERROR_NONE)
    Slice->Error = Error;
}

static uint64_t generatedBits(int32_t TypeId, int64_t Size, int *IsValid) {
  *IsValid = 1;
  if (TypeId == IntegerTyID)
    return 9;
  if (TypeId == FloatTyID && Size == 4)
    return 0x41100000ULL;
  if (TypeId == DoubleTyID && Size == 8)
    return 0x4022000000000000ULL;
  *IsValid = 0;
  return 0;
}

static InputGenGPUFactoryObjectHeader *
decodePointer(void *Pointer, int32_t PointerAS, int64_t Size,
              uint32_t *ObjectIndex, uint32_t *OffsetOut) {
  InputGenGPUFactorySliceHeader *Slice = __ig_current_slice();
  if (!Slice || PointerAS != 0 || Size <= 0 || Size > 8)
    return 0;
  uint64_t Bits = (uint64_t)(uintptr_t)Pointer;
  if ((Bits >> 60) != INPUTGEN_GPU_VPTR_MAGIC) {
    setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_ACCESS);
    return 0;
  }
  uint32_t Index = (uint32_t)((Bits >> INPUTGEN_GPU_VPTR_OFFSET_BITS) &
                              INPUTGEN_GPU_VPTR_OBJECT_MASK);
  int64_t Offset = (int64_t)(Bits & (INPUTGEN_GPU_VPTR_OFFSET_BIAS * 2 - 1)) -
                   (int64_t)INPUTGEN_GPU_VPTR_OFFSET_BIAS;
  InputGenGPUFactoryObjectHeader *Object = __ig_get_object(Index);
  if (!Object || Offset < 0 || (uint64_t)Offset > Object->Capacity ||
      (uint64_t)Size > Object->Capacity - (uint64_t)Offset)
    return 0;
  *ObjectIndex = Index;
  *OffsetOut = (uint32_t)Offset;
  return Object;
}

static void *dataAddress(InputGenGPUFactoryObjectHeader *Object,
                         uint32_t Offset) {
  return (char *)(Object + 1) + Offset;
}

static InputGenGPUFactoryPointerRelation *
findRelation(InputGenGPUFactorySliceHeader *Slice, uint32_t Owner,
             uint32_t Offset) {
  InputGenGPUFactoryPointerRelation *Relations = __ig_relations();
  for (uint32_t I = 0; Relations && I < Slice->RelationCount; ++I)
    if (Relations[I].OwnerObject == Owner && Relations[I].SlotOffset == Offset)
      return &Relations[I];
  return 0;
}

static void *encodePointer(uint32_t ObjectIndex, int64_t Offset) {
  uint64_t Field = (uint64_t)(Offset + (int64_t)INPUTGEN_GPU_VPTR_OFFSET_BIAS);
  return (void *)(uintptr_t)(((uint64_t)INPUTGEN_GPU_VPTR_MAGIC << 60) |
                             ((uint64_t)ObjectIndex
                              << INPUTGEN_GPU_VPTR_OFFSET_BITS) |
                             (Field & (INPUTGEN_GPU_VPTR_OFFSET_BIAS * 2 - 1)));
}

void *__ig_pre_load(void *Pointer, int32_t PointerAS, int64_t ValueSize,
                    int64_t Alignment, int32_t ValueTypeId) {
  (void)Alignment;
  InputGenGPUFactorySliceHeader *Slice = __ig_current_slice();
  uint32_t Owner = 0, Offset = 0;
  InputGenGPUFactoryObjectHeader *Object =
      decodePointer(Pointer, PointerAS, ValueSize, &Owner, &Offset);
  if (!Object)
    return Pointer;

  char *Data = (char *)(Object + 1);
  unsigned char *Mask = (unsigned char *)(Data + Object->Capacity);
  if (ValueTypeId == PointerTyID) {
    if (ValueSize != 8) {
      setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_TYPE);
      return Pointer;
    }
    InputGenGPUFactoryPointerRelation *Relation =
        findRelation(Slice, Owner, Offset);
    if (!Relation) {
      if (Slice->Mode != INPUTGEN_MODE_GENERATE ||
          Slice->RelationCount == Slice->RelationLimit) {
        setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_REPLAY);
        return Pointer;
      }
      uint32_t Target = Slice->ObjectCount;
      if (!__ig_allocate_object(Target))
        return Pointer;
      Relation = &__ig_relations()[Slice->RelationCount++];
      Relation->OwnerObject = Owner;
      Relation->SlotOffset = Offset;
      Relation->TargetObject = Target;
      Relation->TargetOffset = 0;
    }
    if (!__ig_get_object(Relation->TargetObject))
      return Pointer;
    *(void **)(Data + Offset) =
        encodePointer(Relation->TargetObject, Relation->TargetOffset);
    for (uint32_t I = 0; I < (uint32_t)ValueSize; ++I)
      Mask[Offset + I] |= INPUTGEN_GPU_MASK_READ | INPUTGEN_GPU_MASK_POINTER;
    return dataAddress(Object, Offset);
  }

  int IsValid = 0;
  uint64_t Bits = generatedBits(ValueTypeId, ValueSize, &IsValid);
  if (!IsValid) {
    setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_TYPE);
    return Pointer;
  }
  for (uint32_t I = 0; I < (uint32_t)ValueSize; ++I) {
    unsigned char State = Mask[Offset + I];
    if (State & (INPUTGEN_GPU_MASK_READ | INPUTGEN_GPU_MASK_WRITTEN))
      continue;
    if (Slice->Mode == INPUTGEN_MODE_REPLAY) {
      setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_REPLAY);
      return Pointer;
    }
    Data[Offset + I] = (char)(Bits >> (I * 8));
    Mask[Offset + I] = State | INPUTGEN_GPU_MASK_READ;
  }
  return dataAddress(Object, Offset);
}

void *__ig_pre_store(void *Pointer, int32_t PointerAS, int64_t ValueSize,
                     int64_t Alignment, int32_t ValueTypeId) {
  (void)Alignment;
  (void)ValueTypeId;
  uint32_t Owner = 0, Offset = 0;
  InputGenGPUFactoryObjectHeader *Object =
      decodePointer(Pointer, PointerAS, ValueSize, &Owner, &Offset);
  if (!Object)
    return Pointer;
  char *Data = (char *)(Object + 1);
  unsigned char *Mask = (unsigned char *)(Data + Object->Capacity);
  char *Saved = (char *)(Mask + Object->Capacity);
  for (uint32_t I = 0; I < (uint32_t)ValueSize; ++I) {
    unsigned char State = Mask[Offset + I];
    if ((State & INPUTGEN_GPU_MASK_READ) &&
        !(State & INPUTGEN_GPU_MASK_WRITTEN))
      Saved[Offset + I] = Data[Offset + I];
    Mask[Offset + I] = State | INPUTGEN_GPU_MASK_WRITTEN;
  }
  return dataAddress(Object, Offset);
}
