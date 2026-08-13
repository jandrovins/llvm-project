// RUN: %clang --target=amdgcn-amd-amdhsa -mcpu=%inputgen-gpu-arch \
// RUN:   -DINPUTGEN_GPU_RT_DEVICE=1 -I%inputgen-gpu-src \
// RUN:   -I%inputgen-gpu-interface-include -I%inputgen-gpu-llvm-include \
// RUN:   -O3 -g -Wall -Wextra -std=c11 \
// RUN:   -nogpulib -nostdlibinc -fconvergent-functions -fvisibility=protected \
// RUN:   -flto -c -emit-llvm %inputgen-gpu-src/inputgen_gpu_entry_callbacks.c \
// RUN:   -o %t.callbacks.bc
// RUN: %clang --target=amdgcn-amd-amdhsa -mcpu=%inputgen-gpu-arch \
// RUN:   -DINPUTGEN_GPU_RT_DEVICE=1 -I%inputgen-gpu-src \
// RUN:   -I%inputgen-gpu-interface-include -I%inputgen-gpu-llvm-include \
// RUN:   -O3 -g -Wall -Wextra -std=c11 \
// RUN:   -nogpulib -nostdlibinc -fconvergent-functions -fvisibility=protected \
// RUN:   -flto -c -emit-llvm %inputgen-gpu-src/inputgen_gpu_entry_state.c \
// RUN:   -o %t.state.bc
// RUN: %llvm-link %t.callbacks.bc %t.state.bc -o %t.runtime.bc
// RUN: %llvm-dis %t.runtime.bc -o - | FileCheck %s

// REQUIRES: amdgpu

// CHECK-DAG: @inputgen_buffer = protected {{.*}}addrspace(1) global ptr
// CHECK-DAG: @inputgen_buffer_size = protected {{.*}}addrspace(1) global i64 0
// CHECK-DAG: @inputgen_buffer_offset = protected {{.*}}addrspace(1) global i64 0
// CHECK-DAG: @inputgen_mode = protected {{.*}}addrspace(1) global i32 0
// CHECK-DAG: define protected {{.*}}i64 @__instrumentor_post_load(i64 {{.*}}, i64 {{.*}}, i32 {{.*}}, i32 {{.*}})
