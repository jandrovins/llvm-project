// RUN: llvm-dis %inputgen-gpu-entry-runtime-bc -o - | FileCheck --check-prefix=ENTRY %s

// REQUIRES: amdgpu

// ENTRY-DAG: @inputgen_buffer = protected {{.*}}addrspace(1) global ptr
// ENTRY-DAG: @inputgen_buffer_size = protected {{.*}}addrspace(1) global i64 0
// ENTRY-DAG: @inputgen_buffer_offset = protected {{.*}}addrspace(1) global i64 0
// ENTRY-DAG: @inputgen_mode = protected {{.*}}addrspace(1) global i32 0
// ENTRY-DAG: define protected {{.*}}i64 @__instrumentor_post_load(i64 {{.*}}, i64 {{.*}}, i32 {{.*}}, i32 {{.*}})
