// RUN: %clang --target=amdgcn-amd-amdhsa -mcpu=%inputgen-gpu-arch -nogpulib -nogpuinc -nostdlibinc -emit-llvm -c %s -o %t.bc
// RUN: opt -passes=inputgen-gpu -inputgen-gpu-runtime-bitcode=%inputgen-gpu-entry-runtime-bc -inputgen-gpu-entry-function=vvv_foo %t.bc -o %t.linked.bc
// RUN: llc -mtriple=amdgcn-amd-amdhsa -mcpu=%inputgen-gpu-arch -filetype=obj %t.linked.bc -o %t.o
// RUN: ld.lld -flavor gnu -shared %t.o -o %t.image
// RUN: printf '{ "Name": "vvv_foo", "DeviceId": 0 }\n' > %t.json
// RUN: %inputgen-gpu generate %t.image %t.json --inputgen-data %t.inputgen | FileCheck --check-prefix=GENERATE %s
// RUN: %inputgen-gpu replay %t.image %t.json --inputgen-data %t.inputgen | FileCheck --check-prefix=REPLAY %s

// REQUIRES: amdgpu

// GENERATE: b = 81
// GENERATE: generated value = 9
// REPLAY: b = 81
// REPLAY: replay value = 9

int vvv_foo(int *a) { return (*a) * (*a); }
