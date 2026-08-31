// Verify lazy pointer-slot creation and replay reconstruction.
// RUN: %inputgen-gpu-cc -I%inputgen-gpu-src %inputgen-gpu-src/inputgen_gpu_runtime_state.c \
// RUN:   %inputgen-gpu-src/inputgen_gpu_runtime_callbacks.c %s -o %t
// RUN: %t | FileCheck %s
// REQUIRES: inputgen-gpu-host-test

#include "inputgen_gpu_runtime_internal.h"

#include <stdint.h>
#include <stdio.h>

int main(void) {
  _Alignas(8) unsigned char Factory[4096] = {};
  InputGenGPUFactoryHeader *Header = (InputGenGPUFactoryHeader *)Factory;
  Header->Magic = INPUTGEN_GPU_FACTORY_SLICE_MAGIC;
  Header->Version = INPUTGEN_GPU_FACTORY_VERSION;
  Header->Mode = INPUTGEN_MODE_GENERATE;
  Header->NumTeams = 1;
  Header->ThreadsPerTeam = 1;
  Header->NumLanes = 1;
  Header->SliceBytes = 2048;
  Header->ObjectBytes = 64;
  Header->FactoryBytes = sizeof(Factory);

  void *Arguments = __ig_prepare_lane(Factory, 0, 0, 8, 1);
  void *PointerSlot = __ig_pre_load(Arguments, 0, 8, 8, PointerTyID);
  void *Pointer = *(void **)PointerSlot;
  int *Value = (int *)__ig_pre_load(Pointer, 0, 4, 4, IntegerTyID);

  InputGenGPUFactorySliceHeader *Slice =
      (InputGenGPUFactorySliceHeader *)(Factory + ((sizeof(*Header) + 7) &
                                                   ~((uint64_t)7)));
  printf("generate=%d objects=%u relations=%u\n", *Value, Slice->ObjectCount,
         Slice->RelationCount);

  Header->Mode = INPUTGEN_MODE_REPLAY;
  Arguments = __ig_prepare_lane(Factory, 0, 0, 8, 1);
  PointerSlot = __ig_pre_load(Arguments, 0, 8, 8, PointerTyID);
  Pointer = *(void **)PointerSlot;
  Value = (int *)__ig_pre_load(Pointer, 0, 4, 4, IntegerTyID);
  printf("replay=%d error=%u\n", *Value, Slice->Error);
  return 0;
}

// CHECK: generate=9 objects=2 relations=1
// CHECK: replay=9 error=0
