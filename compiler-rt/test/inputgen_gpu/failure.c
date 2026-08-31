// Verify a callback error returns aligned fallback storage instead of an
// encoded pointer that the immediately following instrumented access would
// dereference.
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
  Header->NumTeams = Header->ThreadsPerTeam = Header->NumLanes = 1;
  Header->SliceBytes = 2048;
  Header->ObjectBytes = 64;

  void *Arguments = __ig_prepare_lane(Factory, 0, 0, 8, 1);
  void *Slot = __ig_pre_load(Arguments, 0, 8, 8,
                             INPUTGEN_GPU_VALUE_POINTER);
  void *Pointer = *(void **)Slot;
  void *OutOfBounds = (char *)Pointer + 64;
  int *Fallback = (int *)__ig_pre_load(
      OutOfBounds, 0, 4, 4, INPUTGEN_GPU_VALUE_INTEGER);
  InputGenGPUFactorySliceHeader *Slice =
      (InputGenGPUFactorySliceHeader *)(Factory + ((sizeof(*Header) + 7) &
                                                   ~((uint64_t)7)));
  printf("error=%u fallback=%d\n", Slice->Error, *Fallback);
  return 0;
}

// CHECK: error=3 fallback=0
