; Verify generated entries reconstruct scalar arguments through pre-loads.
; RUN: opt < %s -passes=inputgen-gpu -inputgen-gpu-entry-function=vvv_foo -S | FileCheck %s

target datalayout = "e-m:e-p:64:64-p1:64:64-p2:32:32-p3:32:32-p4:64:64-p5:32:32-p6:32:32-p7:160:256:256:32-p8:128:128:128:48-p9:192:256:256:32-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-v2048:2048-n32:64-S32-A5-G1-ni:7:8:9"
target triple = "amdgcn-amd-amdhsa"

define hidden i32 @vvv_foo(i32 %x, i64 %y) {
entry:
  %t = trunc i64 %y to i32
  %r = add i32 %x, %t
  ret i32 %r
}

; CHECK-LABEL: define amdgpu_kernel void @__ig_entry(
; CHECK: [[ARGUMENTS:%.*]] = call ptr @__ig_prepare_lane(ptr %context, i64 {{%.*}}, i64 {{%.*}}, i64 16, i32 0)
; CHECK: call ptr @__ig_pre_load(ptr {{%.*}}, i32 0, i64 4, i64 4, i32 12)
; CHECK: [[X:%.*]] = load i32, ptr {{%.*}}, align 4
; CHECK: call ptr @__ig_pre_load(ptr {{%.*}}, i32 0, i64 8, i64 8, i32 12)
; CHECK: [[Y:%.*]] = load i64, ptr {{%.*}}, align 8
; CHECK: [[RESULT_VAL:%.*]] = call i32 @vvv_foo(i32 [[X]], i64 [[Y]])
; CHECK: call void @__ig_store_result(i64 {{%.*}}, i32 4)
