// This test compiles a direct-entry AMDGPU image from compiler-rt's linked
// runtime bitcode, then invokes the launcher in generate and replay modes.
// RUN: %clang --target=amdgcn-amd-amdhsa -mcpu=%inputgen-gpu-amdgpu-arch -nogpulib -nogpuinc -nostdlibinc -emit-llvm -c %s -o %t.bc
// RUN: opt -passes=inputgen-gpu -inputgen-gpu-runtime-bitcode=%inputgen-gpu-runtime-bc -inputgen-gpu-entry-function=vvv_foo %t.bc -o %t.linked.bc
// RUN: llc -mtriple=amdgcn-amd-amdhsa -mcpu=%inputgen-gpu-amdgpu-arch -filetype=obj %t.linked.bc -o %t.o
// RUN: ld.lld -flavor gnu -shared %t.o -o %t.image
// RUN: %not %inputgen-gpu replay %t.image --inputgen-data %t.missing 2>&1 | FileCheck --check-prefix=MISSING-DATA %s
// RUN: %inputgen-gpu generate %t.image --device-id=0 --num-teams=1 --num-threads=1 --config-objects-per-thread=2 | FileCheck --check-prefix=GENERATE %s
// RUN: test -s %t.inputgen
// RUN: %inputgen-gpu replay %t.image --device-id=0 --num-teams=1 --num-threads=1 | FileCheck --check-prefix=REPLAY %s
// RUN: %not %inputgen-gpu replay %t.image --config-objects-per-thread=1 2>&1 | FileCheck --check-prefix=CAPACITY-CONFLICT %s
// RUN: %not %inputgen-gpu generate %t.image --inputgen-data %t.no-such-dir/value 2>&1 | FileCheck --check-prefix=OUTPUT-ERROR %s

// REQUIRES: amdgpu, inputgen-gpu-runtime, inputgen-gpu-amdgpu-image

// MISSING-DATA: error: failed to open replay data file
// GENERATE: serialized input =
// GENERATE: result[0] = 81
// REPLAY: replay result[0] = 81
// CAPACITY-CONFLICT: error: replay options conflict with the InputGen data file
// OUTPUT-ERROR: error: failed to open generated data file

struct Inputs {
  int *Values;
  int Count;
};

__attribute__((noinline)) int reduce(int *Values, int Count) {
  int Sum = 0;
  for (int I = 0; I < Count; ++I)
    Sum += Values[I];
  return Sum;
}

int vvv_foo(struct Inputs *Input) {
  return reduce(Input->Values, Input->Count);
}
