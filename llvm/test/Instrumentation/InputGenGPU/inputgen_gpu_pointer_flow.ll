; Verify pointer slots and direct helper accesses are both instrumented.
; RUN: opt < %s -passes=inputgen-gpu -inputgen-gpu-entry-function=vvv_foo -S | FileCheck %s

target datalayout = "e-m:e-p:64:64-p1:64:64-p2:32:32-p3:32:32-p4:64:64-p5:32:32-p6:32:32-p7:160:256:256:32-p8:128:128:128:48-p9:192:256:256:32-i64:64-v16:16-v24:32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-v2048:2048-n32:64-S32-A5-G1-ni:7:8:9"
target triple = "amdgcn-amd-amdhsa"

define internal i32 @helper(ptr %p) {
; CHECK-LABEL: define internal i32 @helper(
; CHECK: [[HELPER_ADDRESS:%.*]] = call ptr @__ig_pre_load(ptr %p, i32 0, i64 4, i64 4, i32 1)
; CHECK: [[HELPER_VALUE:%.*]] = load i32, ptr [[HELPER_ADDRESS]], align 4
; CHECK: ret i32 [[HELPER_VALUE]]
entry:
  %value = load i32, ptr %p, align 4
  ret i32 %value
}

define hidden i32 @vvv_foo(ptr %s) {
; CHECK-LABEL: define hidden i32 @vvv_foo(
; CHECK: [[SLOT_ADDRESS:%.*]] = call ptr @__ig_pre_load(ptr %s, i32 0, i64 8, i64 8, i32 4)
; CHECK: [[POINTER:%.*]] = load ptr, ptr [[SLOT_ADDRESS]], align 8
; CHECK: call i32 @helper(ptr [[POINTER]])
entry:
  %p = load ptr, ptr %s, align 8
  %value = call i32 @helper(ptr %p)
  ret i32 %value
}
