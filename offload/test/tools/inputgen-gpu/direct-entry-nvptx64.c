// Build an NVPTX64 direct-entry image from the shared reduction source.
// RUN: %clang --target=nvptx64-nvidia-cuda -mcpu=%inputgen-gpu-nvptx-arch -nogpulib -nogpuinc -nostdlibinc -emit-llvm -c %s -o %t.bc
// RUN: opt -passes=inputgen-gpu -inputgen-gpu-runtime-bitcode=%inputgen-gpu-runtime-bc -inputgen-gpu-entry-function=vvv_foo %t.bc -o %t.linked.bc
// RUN: llc -mtriple=nvptx64-nvidia-cuda -mcpu=%inputgen-gpu-nvptx-arch -filetype=asm %t.linked.bc -o %t.ptx
// RUN: ptxas -arch=%inputgen-gpu-nvptx-arch %t.ptx -o %t.image
// RUN: %not %inputgen-gpu replay %t.image --inputgen-data %t.missing 2>&1 | FileCheck --check-prefix=MISSING-DATA %S/direct-entry-reduction.h
// RUN: %inputgen-gpu generate %t.image --device-id=0 --num-teams=1 --num-threads=1 --objects-per-thread=3 | FileCheck --check-prefix=GENERATE %S/direct-entry-reduction.h
// RUN: test -s %t.inputgen
// RUN: %inputgen-gpu replay %t.image --device-id=0 --num-teams=1 --num-threads=1 | FileCheck --check-prefix=REPLAY %S/direct-entry-reduction.h
// RUN: %not %inputgen-gpu replay %t.image --objects-per-thread=2 2>&1 | FileCheck --check-prefix=CAPACITY-CONFLICT %S/direct-entry-reduction.h
// RUN: %not %inputgen-gpu generate %t.image --inputgen-data %t.no-such-dir/value 2>&1 | FileCheck --check-prefix=OUTPUT-ERROR %S/direct-entry-reduction.h

// REQUIRES: nvptx64-nvidia-cuda, inputgen-gpu-runtime, inputgen-gpu-launcher, inputgen-gpu-nvptx-image

#include "direct-entry-reduction.h"
