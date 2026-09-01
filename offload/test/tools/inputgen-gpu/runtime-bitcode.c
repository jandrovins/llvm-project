// RUN: llvm-dis %inputgen-gpu-runtime-bc -o - | FileCheck %s
// REQUIRES: inputgen-gpu-runtime

// CHECK-DAG: define{{.*}} ptr @__ig_prepare_thread(
// CHECK-DAG: define{{.*}} ptr @__ig_pre_load(
// CHECK-DAG: define{{.*}} i32 @__ig_error_pending(
// CHECK-DAG: define{{.*}} ptr @__ig_pre_store(
// CHECK-DAG: define{{.*}} void @__ig_store_result(
