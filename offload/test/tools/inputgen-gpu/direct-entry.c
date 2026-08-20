// This test compiles a direct-entry AMDGPU image from PR 4's split runtime
// bitcodes, then invokes the launcher in generate and replay modes.
// RUN: %clang --target=amdgcn-amd-amdhsa -mcpu=%inputgen-gpu-amdgpu-arch -nogpulib -nogpuinc -nostdlibinc -emit-llvm -c %s -o %t.bc
// RUN: opt -passes=inputgen-gpu -inputgen-gpu-runtime-bitcode=%inputgen-gpu-state-bc -inputgen-gpu-runtime-bitcode=%inputgen-gpu-callbacks-bc -inputgen-gpu-entry-function=vvv_foo %t.bc -o %t.linked.bc
// RUN: llc -mtriple=amdgcn-amd-amdhsa -mcpu=%inputgen-gpu-amdgpu-arch -filetype=obj %t.linked.bc -o %t.o
// RUN: ld.lld -flavor gnu -shared %t.o -o %t.image
// RUN: printf '{ "Name": "vvv_foo", "DeviceId": 0 }\n' > %t.json
// RUN: %not %inputgen-gpu replay %t.image %t.json --inputgen-data %t.missing 2>&1 | FileCheck --check-prefix=MISSING-DATA %s
// RUN: %inputgen-gpu generate %t.image %t.json --device-id=0 --num-teams=1 --num-threads=1 | FileCheck --check-prefix=GENERATE %s
// RUN: test -s %t.inputgen
// RUN: %inputgen-gpu replay %t.image %t.json --device-id=0 --num-teams=1 --num-threads=1 | FileCheck --check-prefix=REPLAY %s
// RUN: %not %inputgen-gpu generate %t.image %t.json --inputgen-data %t.no-such-dir/value 2>&1 | FileCheck --check-prefix=OUTPUT-ERROR %s

// REQUIRES: amdgpu, inputgen-gpu-runtime, inputgen-gpu-amdgpu-image

// MISSING-DATA: error: failed to open replay data file
// GENERATE: b = 81
// GENERATE: generated value = 9
// REPLAY: b = 81
// REPLAY: replay value = 9
// OUTPUT-ERROR: error: failed to open generated data file

int vvv_foo(int *A) { return (*A) * (*A); }
