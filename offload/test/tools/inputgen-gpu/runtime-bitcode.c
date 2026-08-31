// RUN: llvm-dis %inputgen-gpu-runtime-bc -o - | FileCheck %s
// REQUIRES: inputgen-gpu-runtime

// CHECK: define{{.*}} ptr @__ig_prepare_lane(
// CHECK: define{{.*}} ptr @__ig_pre_load(
// CHECK: define{{.*}} ptr @__ig_pre_store(
// CHECK: define{{.*}} void @__ig_store_result(
