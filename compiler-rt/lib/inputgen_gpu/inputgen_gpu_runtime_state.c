//===-- InputGen GPU Runtime State ---------------------------------------===//

#include "inputgen_gpu_runtime_internal.h"

// Keep the launch-wide context word in AS1, a device-global address space on
// the supported targets, and derive each GPU thread's slice from its hardware
// IDs. User-visible pointers are generic AS0 addresses into the fixed slice.
static __attribute__((address_space(1))) uint64_t FactoryContextBits;

// Everything a callback needs about the current GPU thread's slice, resolved
// once on entry. Deriving the layout here rather than at each use keeps the
// object-scan loop in findObject from recomputing it per object.
typedef struct {
  InputGenGPUFactoryHeader *Factory;
  InputGenGPUFactorySliceHeader *Slice;
  InputGenGPUFactoryLayout Layout;
} SliceContext;

// Recover the launch-wide factory selected by this GPU thread's entry wrapper.
static InputGenGPUFactoryHeader *currentFactory(void) {
  return (InputGenGPUFactoryHeader *)(uintptr_t)FactoryContextBits;
}

static int factoryLayout(const InputGenGPUFactoryHeader *Factory,
                         InputGenGPUFactoryLayout *Layout) {
  // The header stores the already-multiplied thread count, so pass it as the
  // team count against a single thread per team; the product is what the
  // layout needs. ThreadsPerTeam is validated separately, by resolveContext,
  // because only the hardware-ID mapping depends on it.
  return inputgenGPUComputeLayout(Factory->NumGPUThreads, 1,
                                  Factory->ObjectBytes,
                                  Factory->ObjectsPerThread,
                                  Layout) == INPUTGEN_GPU_LAYOUT_OK;
}

// Resolve the factory, its layout, and this GPU thread's slice. Returns 0 and
// leaves *Ctx unusable when the geometry or the hardware IDs do not match.
static int resolveContext(InputGenGPUFactoryHeader *Factory, SliceContext *Ctx,
                          uint64_t *ThreadIndex) {
  if (!Factory || !Factory->ThreadsPerTeam ||
      !factoryLayout(Factory, &Ctx->Layout))
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
  if (Index >= Factory->NumGPUThreads)
    return 0;
  if (ThreadIndex)
    *ThreadIndex = Index;
  Ctx->Factory = Factory;
  Ctx->Slice = (InputGenGPUFactorySliceHeader *)((char *)Factory +
                                                 inputgenGPUSliceOffset(
                                                     &Ctx->Layout,
                                                     (uint32_t)Index));
  return 1;
}

static int currentContext(SliceContext *Ctx) {
  return resolveContext(currentFactory(), Ctx, 0);
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
relationTable(const SliceContext *Ctx) {
  return (InputGenGPUFactoryPointerRelation *)((char *)Ctx->Slice +
                                               Ctx->Layout.RelationTableOffset);
}

static uint64_t objectCapacity(const SliceContext *Ctx, uint32_t ObjectIndex) {
  return ObjectIndex == 0 ? Ctx->Slice->ArgumentBytes : Ctx->Layout.ObjectBytes;
}

static char *getObject(const SliceContext *Ctx, uint32_t ObjectIndex) {
  // Resolve a logical object index to its deterministic fixed slot. The bound
  // is ObjectCount rather than ObjectsPerThread because slots past the
  // allocation watermark are not yet logically live.
  if (ObjectIndex >= Ctx->Slice->ObjectCount) {
    setError(Ctx->Slice, INPUTGEN_GPU_FACTORY_ERROR_LAYOUT);
    return 0;
  }
  uint64_t Offset = inputgenGPUObjectOffset(&Ctx->Layout, ObjectIndex);
  if (!rangeInSlice(Offset, Ctx->Layout.ObjectSlotBytes,
                    Ctx->Layout.SliceBytes)) {
    setError(Ctx->Slice, INPUTGEN_GPU_FACTORY_ERROR_LAYOUT);
    return 0;
  }
  return (char *)Ctx->Slice + Offset;
}

static char *allocateObject(const SliceContext *Ctx, uint32_t ObjectIndex) {
  // Make the next preallocated slot logically live without growing the slice.
  if (ObjectIndex != Ctx->Slice->ObjectCount ||
      ObjectIndex >= Ctx->Layout.ObjectsPerThread) {
    setError(Ctx->Slice, INPUTGEN_GPU_FACTORY_ERROR_CAPACITY);
    return 0;
  }
  ++Ctx->Slice->ObjectCount;
  return getObject(Ctx, ObjectIndex);
}

void *__ig_prepare_thread(void *Context, uint64_t ArgumentBytes) {
  InputGenGPUFactoryHeader *Factory = (InputGenGPUFactoryHeader *)Context;
  SliceContext Ctx;
  uint64_t ThreadIndex = 0;
  if (!Factory || Factory->Magic != INPUTGEN_GPU_FACTORY_SLICE_MAGIC ||
      Factory->Version != INPUTGEN_GPU_FACTORY_VERSION ||
      !Factory->NumGPUThreads || ArgumentBytes > Factory->ObjectBytes ||
      !resolveContext(Factory, &Ctx, &ThreadIndex))
    return 0;
  FactoryContextBits = (uint64_t)(uintptr_t)Factory;
  InputGenGPUFactorySliceHeader *Slice = Ctx.Slice;

  if (Factory->Mode == INPUTGEN_MODE_GENERATE) {
    // Initialize tables and object zero; pointer loads allocate later objects.
    __builtin_memset(Slice, 0, Ctx.Layout.SliceBytes);
    Slice->Magic = INPUTGEN_GPU_FACTORY_SLICE_MAGIC;
    Slice->Version = INPUTGEN_GPU_FACTORY_VERSION;
    Slice->SliceIndex = (uint32_t)ThreadIndex;
    Slice->ArgumentBytes = ArgumentBytes;
    if (!allocateObject(&Ctx, 0))
      return 0;
  } else if (Factory->Mode == INPUTGEN_MODE_REPLAY) {
    // Accept only the host-reconstructed layout from the matching recording.
    if (Slice->Magic != INPUTGEN_GPU_FACTORY_SLICE_MAGIC ||
        Slice->Version != INPUTGEN_GPU_FACTORY_VERSION ||
        Slice->SliceIndex != ThreadIndex ||
        Slice->ArgumentBytes != ArgumentBytes || !Slice->ObjectCount ||
        Slice->ObjectCount > Ctx.Layout.ObjectsPerThread ||
        Slice->RelationCount >= Ctx.Layout.ObjectsPerThread ||
        Slice->RelationCount + 1 != Slice->ObjectCount) {
      setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_LAYOUT);
      return 0;
    }
  } else {
    return 0;
  }
  return getObject(&Ctx, 0);
}

void __ig_store_result(uint64_t Bits, uint32_t Size) {
  // Keep each user-function return value in its owning GPU thread's slice.
  SliceContext Ctx;
  if (currentContext(&Ctx)) {
    Ctx.Slice->ResultBits = Bits;
    Ctx.Slice->ResultSize = Size;
  }
}

int32_t __ig_error_pending(void) {
  SliceContext Ctx;
  return currentContext(&Ctx) &&
         Ctx.Slice->Error != INPUTGEN_GPU_FACTORY_ERROR_NONE;
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

static char *findObject(const SliceContext *Ctx, void *Pointer,
                        int32_t PointerAS, int64_t Size, int64_t Alignment,
                        uint32_t *ObjectIndex, uint32_t *OffsetOut,
                        int *IsFactoryAddress) {
  // Resolve a real AS0 address against allocated object-data ranges in the
  // current GPU thread's fixed slice. External addresses pass through.
  InputGenGPUFactorySliceHeader *Slice = Ctx->Slice;
  *IsFactoryAddress = 0;
  if (PointerAS != 0)
    return 0;

  uintptr_t Address = (uintptr_t)Pointer;
  uintptr_t SliceBegin = (uintptr_t)Slice;
  if (Address >= SliceBegin && Address - SliceBegin < Ctx->Layout.SliceBytes)
    *IsFactoryAddress = 1;

  if (Size <= 0 || Size > 8 || Alignment <= 0 ||
      ((uint64_t)Alignment & ((uint64_t)Alignment - 1)) != 0) {
    if (*IsFactoryAddress)
      setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_ACCESS);
    return 0;
  }

  for (uint32_t Index = 0; Index < Slice->ObjectCount; ++Index) {
    char *Data = getObject(Ctx, Index);
    if (!Data)
      return 0;
    uintptr_t DataAddress = (uintptr_t)Data;
    if (Address < DataAddress)
      continue;
    uint64_t Capacity = objectCapacity(Ctx, Index);
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

static InputGenGPUFactoryPointerRelation *
findRelation(const SliceContext *Ctx, uint32_t Owner, uint32_t Offset) {
  // Find the target object already assigned to this pointer slot.
  InputGenGPUFactoryPointerRelation *Relations = relationTable(Ctx);
  for (uint32_t I = 0; I < Ctx->Slice->RelationCount; ++I)
    if (Relations[I].OwnerObject == Owner && Relations[I].SlotOffset == Offset)
      return &Relations[I];
  return 0;
}

void *__ig_pre_load(void *Pointer, int32_t PointerAS, int64_t ValueSize,
                    int64_t Alignment, int32_t ValueTypeId) {
  SliceContext Ctx;
  if (!currentContext(&Ctx))
    return Pointer;
  InputGenGPUFactorySliceHeader *Slice = Ctx.Slice;
  uint32_t Owner = 0, Offset = 0;
  int IsFactoryAddress = 0;
  char *Object = findObject(&Ctx, Pointer, PointerAS, ValueSize, Alignment,
                            &Owner, &Offset, &IsFactoryAddress);
  if (!Object)
    return IsFactoryAddress ? failureAddress(Slice, Pointer) : Pointer;

  char *Data = Object;
  unsigned char *Mask = (unsigned char *)(Data + Ctx.Layout.ObjectBytes);
  if (ValueTypeId == INPUTGEN_GPU_VALUE_POINTER) {
    // Pointer slots record object relationships, never a serialized address.
    if (ValueSize != (int64_t)sizeof(void *)) {
      setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_TYPE);
      return failureAddress(Slice, Pointer);
    }
    InputGenGPUFactoryPointerRelation *Relation =
        findRelation(&Ctx, Owner, Offset);
    if (!Relation) {
      // Generation reserves the target object; replay requires its relation.
      if (Ctx.Factory->Mode != INPUTGEN_MODE_GENERATE) {
        setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_REPLAY);
        return failureAddress(Slice, Pointer);
      }
      if (Slice->RelationCount + 1 >= Ctx.Layout.ObjectsPerThread) {
        setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_CAPACITY);
        return failureAddress(Slice, Pointer);
      }
      uint32_t Target = Slice->ObjectCount;
      if (!allocateObject(&Ctx, Target))
        return failureAddress(Slice, Pointer);
      Relation = &relationTable(&Ctx)[Slice->RelationCount++];
      Relation->OwnerObject = Owner;
      Relation->SlotOffset = Offset;
      Relation->TargetObject = Target;
      Relation->TargetOffset = 0;
    }
    char *Target = getObject(&Ctx, Relation->TargetObject);
    if (!Target ||
        Relation->TargetOffset > objectCapacity(&Ctx, Relation->TargetObject))
      return failureAddress(Slice, Pointer);
    *(void **)(Data + Offset) = Target + Relation->TargetOffset;
    for (uint32_t I = 0; I < (uint32_t)ValueSize; ++I)
      Mask[Offset + I] |= INPUTGEN_GPU_MASK_READ | INPUTGEN_GPU_MASK_POINTER;
    return Object + Offset;
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
    if (Ctx.Factory->Mode == INPUTGEN_MODE_REPLAY) {
      setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_REPLAY);
      return failureAddress(Slice, Pointer);
    }
    // First reads become input; a prior write supplies the current value.
    Data[Offset + I] = (char)(Bits >> (I * 8));
    Mask[Offset + I] = State | INPUTGEN_GPU_MASK_READ;
  }
  return Object + Offset;
}

void *__ig_pre_store(void *Pointer, int32_t PointerAS, int64_t ValueSize,
                     int64_t Alignment, int32_t ValueTypeId) {
  SliceContext Ctx;
  if (!currentContext(&Ctx))
    return Pointer;
  InputGenGPUFactorySliceHeader *Slice = Ctx.Slice;
  if (ValueTypeId == INPUTGEN_GPU_VALUE_POINTER) {
    setError(Slice, INPUTGEN_GPU_FACTORY_ERROR_TYPE);
    return failureAddress(Slice, Pointer);
  }
  uint32_t Owner = 0, Offset = 0;
  int IsFactoryAddress = 0;
  char *Object = findObject(&Ctx, Pointer, PointerAS, ValueSize, Alignment,
                            &Owner, &Offset, &IsFactoryAddress);
  if (!Object)
    return IsFactoryAddress ? failureAddress(Slice, Pointer) : Pointer;
  char *Data = Object;
  unsigned char *Mask = (unsigned char *)(Data + Ctx.Layout.ObjectBytes);
  char *Saved = (char *)(Mask + Ctx.Layout.ObjectBytes);
  // Preserve input before its first overwrite so serialization keeps the read.
  for (uint32_t I = 0; I < (uint32_t)ValueSize; ++I) {
    unsigned char State = Mask[Offset + I];
    if ((State & INPUTGEN_GPU_MASK_READ) &&
        !(State & INPUTGEN_GPU_MASK_WRITTEN))
      Saved[Offset + I] = Data[Offset + I];
    Mask[Offset + I] = State | INPUTGEN_GPU_MASK_WRITTEN;
  }
  return Object + Offset;
}
