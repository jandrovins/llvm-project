// Verify lazy pointer-slot creation and replay reconstruction.
// RUN: %inputgen-gpu-cc -I%inputgen-gpu-src \
// RUN:   %inputgen-gpu-src/inputgen_gpu_runtime_state.c %s -o %t
// RUN: %t | FileCheck %s
// REQUIRES: inputgen-gpu-host-test

#include "inputgen_gpu_runtime_internal.h"

#include <stdint.h>
#include <stdio.h>

static uint64_t getSliceBytes(uint32_t Objects) {
  uint64_t Relations =
      (uint64_t)(Objects - 1) * sizeof(InputGenGPUFactoryPointerRelation);
  uint64_t ObjectOffset =
      (sizeof(InputGenGPUFactorySliceHeader) + 7 + Relations) & ~(uint64_t)7;
  return ObjectOffset + (uint64_t)Objects * 3 * 64;
}

int main(void) {
  _Alignas(8) unsigned char Factory[4096] = {};
  InputGenGPUFactoryHeader *Header = (InputGenGPUFactoryHeader *)Factory;
  Header->Magic = INPUTGEN_GPU_FACTORY_SLICE_MAGIC;
  Header->Version = INPUTGEN_GPU_FACTORY_VERSION;
  Header->Mode = INPUTGEN_MODE_GENERATE;
  Header->NumTeams = 1;
  Header->ThreadsPerTeam = 1;
  Header->NumLanes = 1;
  Header->ObjectsPerThread = 2;
  Header->SliceBytes = getSliceBytes(Header->ObjectsPerThread);
  Header->ObjectBytes = 64;
  Header->FactoryBytes = sizeof(Factory);

  void *Arguments = __ig_prepare_thread(Factory, 8);
  void *PointerSlot =
      __ig_pre_load(Arguments, 0, 8, 8, INPUTGEN_GPU_VALUE_POINTER);
  void *Pointer = *(void **)PointerSlot;
  printf("real-pointer=%d\n", (unsigned char *)Pointer >= Factory &&
                                  (unsigned char *)Pointer < Factory + 4096);
  int *Value =
      (int *)__ig_pre_load(Pointer, 0, 4, 4, INPUTGEN_GPU_VALUE_INTEGER);

  InputGenGPUFactorySliceHeader *Slice =
      (InputGenGPUFactorySliceHeader *)(Factory + ((sizeof(*Header) + 7) &
                                                   ~((uint64_t)7)));
  printf("generate=%d objects=%u relations=%u\n", *Value, Slice->ObjectCount,
         Slice->RelationCount);

  Header->Mode = INPUTGEN_MODE_REPLAY;
  Arguments = __ig_prepare_thread(Factory, 8);
  PointerSlot = __ig_pre_load(Arguments, 0, 8, 8, INPUTGEN_GPU_VALUE_POINTER);
  Pointer = *(void **)PointerSlot;
  Value = (int *)__ig_pre_load(Pointer, 0, 4, 4, INPUTGEN_GPU_VALUE_INTEGER);
  printf("replay=%d error=%u\n", *Value, Slice->Error);

  Header->Mode = INPUTGEN_MODE_GENERATE;
  Header->ObjectsPerThread = 3;
  Header->SliceBytes = getSliceBytes(Header->ObjectsPerThread);
  Arguments = __ig_prepare_thread(Factory, 8);
  PointerSlot = __ig_pre_load(Arguments, 0, 8, 8, INPUTGEN_GPU_VALUE_POINTER);
  Pointer = *(void **)PointerSlot;
  void *NestedSlot =
      __ig_pre_load(Pointer, 0, 8, 8, INPUTGEN_GPU_VALUE_POINTER);
  void *NestedPointer = *(void **)NestedSlot;
  Value =
      (int *)__ig_pre_load(NestedPointer, 0, 4, 4, INPUTGEN_GPU_VALUE_INTEGER);
  printf("nested=%d objects=%u/%u relations=%u/%u\n", *Value,
         Slice->ObjectCount, Slice->ObjectLimit, Slice->RelationCount,
         Slice->RelationLimit);

  Header->Mode = INPUTGEN_MODE_REPLAY;
  Arguments = __ig_prepare_thread(Factory, 8);
  PointerSlot = __ig_pre_load(Arguments, 0, 8, 8, INPUTGEN_GPU_VALUE_POINTER);
  Pointer = *(void **)PointerSlot;
  NestedSlot = __ig_pre_load(Pointer, 0, 8, 8, INPUTGEN_GPU_VALUE_POINTER);
  NestedPointer = *(void **)NestedSlot;
  Value =
      (int *)__ig_pre_load(NestedPointer, 0, 4, 4, INPUTGEN_GPU_VALUE_INTEGER);
  printf("nested-replay=%d error=%u\n", *Value, Slice->Error);
  return 0;
}

// CHECK: real-pointer=1
// CHECK: generate=9 objects=2 relations=1
// CHECK: replay=9 error=0
// CHECK: nested=9 objects=3/3 relations=2/2
// CHECK: nested-replay=9 error=0
