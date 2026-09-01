; Verify entry generation uses one opaque context and ordinary argument loads.
; RUN: opt < %s -passes=inputgen-gpu -inputgen-gpu-entry-function=vvv_foo -S | FileCheck %s

target datalayout = "e-m:e-p:64:64-p1:64:64-p2:32:32-p3:32:32-p4:64:64-p5:32:32-p6:32:32-p7:160:256:256:32-p8:128:128:128:48-p9:192:256:256:32-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-v2048:2048-n32:64-S32-A5-G1-ni:7:8:9"
target triple = "amdgcn-amd-amdhsa"

define hidden i32 @vvv_foo(ptr noundef %a) {
; CHECK-LABEL: define hidden i32 @vvv_foo(
; CHECK-NOT: __ig_post_base_pointer_info
; CHECK: [[ADDRESS:%.*]] = call ptr @__ig_pre_load(ptr %a, i32 0, i64 4, i64 4, i32 1)
; CHECK: call i32 @__ig_error_pending()
; CHECK: [[LOAD:%.*]] = load i32, ptr [[ADDRESS]], align 4
; CHECK: ret i32 [[LOAD]]
entry:
  %v = load i32, ptr %a, align 4
  ret i32 %v
}

; CHECK-LABEL: define amdgpu_kernel void @__ig_entry(
; CHECK: [[ARGUMENTS:%.*]] = call ptr @__ig_prepare_thread(ptr %context, i64 8, i32 1)
; CHECK: [[SLOT:%.*]] = getelementptr inbounds i8, ptr [[ARGUMENTS]], i64 0
; CHECK: [[SLOT_ADDRESS:%.*]] = call ptr @__ig_pre_load(ptr [[SLOT]], i32 0, i64 8, i64 8, i32 4)
; CHECK: call i32 @__ig_error_pending()
; CHECK: [[ARG:%.*]] = load ptr, ptr [[SLOT_ADDRESS]], align 8
; CHECK: [[RESULT_VAL:%.*]] = call i32 @vvv_foo(ptr [[ARG]])
; CHECK: call i32 @__ig_error_pending()
; CHECK: [[RESULT_BITS:%.*]] = zext i32 [[RESULT_VAL]] to i64
; CHECK: call void @__ig_store_result(i64 [[RESULT_BITS]], i32 4)
; CHECK: ret void
