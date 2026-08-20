// This test lowers the direct-entry image to PTX, assembles a CUDA ELF/CUBIN,
// and invokes the backend-neutral launcher in generate and replay modes.
// RUN: %clang --target=nvptx64-nvidia-cuda -march=%inputgen-gpu-nvptx-arch -nogpulib -nogpuinc -nostdlibinc -emit-llvm -c %s -o %t.bc
// RUN: opt -passes=inputgen-gpu -inputgen-gpu-runtime-bitcode=%inputgen-gpu-state-bc -inputgen-gpu-runtime-bitcode=%inputgen-gpu-callbacks-bc -inputgen-gpu-entry-function=vvv_foo %t.bc -o %t.linked.bc
// RUN: llc -mtriple=nvptx64-nvidia-cuda -mcpu=%inputgen-gpu-nvptx-arch -filetype=asm %t.linked.bc -o %t.ptx
// RUN: ptxas --gpu-name %inputgen-gpu-nvptx-arch --output-file %t.cubin %t.ptx
// RUN: printf '{ "Name": "vvv_foo", "DeviceId": 0 }\n' > %t.json
// RUN: %inputgen-gpu generate %t.cubin %t.json --device-id=0 --num-teams=1 --num-threads=1 | FileCheck --check-prefix=GENERATE %s
// RUN: test -s %t.inputgen
// RUN: %inputgen-gpu replay %t.cubin %t.json --device-id=0 --num-teams=1 --num-threads=1 | FileCheck --check-prefix=REPLAY %s

// REQUIRES: nvidiagpu, inputgen-gpu-runtime, inputgen-gpu-nvptx-image

// GENERATE: b = 81
// GENERATE: generated value = 9
// REPLAY: b = 81
// REPLAY: replay value = 9

int vvv_foo(int *A) { return (*A) * (*A); }
