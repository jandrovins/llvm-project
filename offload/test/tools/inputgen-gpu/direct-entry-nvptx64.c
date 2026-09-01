// This test compiles a direct-entry NVPTX64 image from compiler-rt's linked
// runtime bitcode, then invokes the launcher in generate and replay modes.
// RUN: %clang --target=nvptx64-nvidia-cuda -mcpu=%inputgen-gpu-nvptx-arch -nogpulib -nogpuinc -nostdlibinc -emit-llvm -c %s -o %t.bc
// RUN: opt -passes=inputgen-gpu -inputgen-gpu-runtime-bitcode=%inputgen-gpu-runtime-bc -inputgen-gpu-entry-function=vvv_foo %t.bc -o %t.linked.bc
// RUN: llc -mtriple=nvptx64-nvidia-cuda -mcpu=%inputgen-gpu-nvptx-arch -filetype=asm %t.linked.bc -o %t.ptx
// RUN: ptxas -arch=%inputgen-gpu-nvptx-arch %t.ptx -o %t.image
// RUN: printf '{ "Name": "vvv_foo", "DeviceId": 0 }\n' > %t.json
// RUN: %not %inputgen-gpu replay %t.image %t.json --inputgen-data %t.missing 2>&1 | FileCheck --check-prefix=MISSING-DATA %s
// RUN: %inputgen-gpu generate %t.image %t.json --device-id=0 --num-teams=1 --num-threads=1 --config-objects-per-thread=2 | FileCheck --check-prefix=GENERATE %s
// RUN: test -s %t.inputgen
// RUN: %inputgen-gpu replay %t.image %t.json --device-id=0 --num-teams=1 --num-threads=1 | FileCheck --check-prefix=REPLAY %s
// RUN: %not %inputgen-gpu replay %t.image %t.json --config-objects-per-thread=1 2>&1 | FileCheck --check-prefix=CAPACITY-CONFLICT %s
// RUN: %not %inputgen-gpu generate %t.image %t.json --inputgen-data %t.no-such-dir/value 2>&1 | FileCheck --check-prefix=OUTPUT-ERROR %s

// REQUIRES: nvptx64-nvidia-cuda, inputgen-gpu-runtime, inputgen-gpu-nvptx-image

// MISSING-DATA: error: failed to open replay data file
// GENERATE: serialized input =
// GENERATE: result[0] = 81
// REPLAY: replay result[0] = 81
// CAPACITY-CONFLICT: error: replay options conflict with the InputGen data file
// OUTPUT-ERROR: error: failed to open generated data file

int vvv_foo(int *A) { return (*A) * (*A); }
