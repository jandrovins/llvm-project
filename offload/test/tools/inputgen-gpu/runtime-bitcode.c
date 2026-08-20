// RUN: llvm-dis %inputgen-gpu-runtime-bc -o - | FileCheck %s
// REQUIRES: inputgen-gpu-runtime

// CHECK-DAG: @inputgen_buffer = protected {{.*}}global ptr
// CHECK-DAG: @inputgen_buffer_size = protected {{.*}}global i64 0
// CHECK-DAG: @inputgen_buffer_offset = protected {{.*}}global i64 0
// CHECK-DAG: @inputgen_mode = protected {{.*}}global i32 0
// CHECK: define protected {{.*}}i64 @__ig_post_load(i64 {{.*}}, i64 {{.*}}, i32 {{.*}}, i32 {{.*}})
