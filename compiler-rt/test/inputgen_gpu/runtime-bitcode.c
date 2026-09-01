// Verify the compiler-rt artifact is a raw module consumed by InputGenGPU.
// RUN: llvm-link %inputgen-gpu-runtime-bc -o %t.bc
// RUN: llvm-nm --defined-only %t.bc | FileCheck %s
// REQUIRES: inputgen-gpu-runtime

// CHECK-DAG: __ig_prepare_lane
// CHECK-DAG: __ig_pre_load
// CHECK-DAG: __ig_pre_store
// CHECK-DAG: __ig_error_pending
// CHECK-DAG: __ig_store_result
