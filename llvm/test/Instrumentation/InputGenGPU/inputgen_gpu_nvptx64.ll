; Verify NVPTX64 entry generation uses the same opaque context and argument
; object model as AMDGPU, with NVVM block and thread IDs.
; RUN: opt < %s -passes=inputgen-gpu -inputgen-gpu-entry-function=vvv_foo -S | FileCheck %s

target datalayout = "e-p6:32:32-i64:64-i128:128-v16:16-v32:32-n16:32:64"
target triple = "nvptx64-nvidia-cuda"

define hidden i32 @vvv_foo(ptr noundef %a) {
; CHECK-LABEL: define hidden i32 @vvv_foo(
; CHECK-NOT: __ig_post_base_pointer_info
; CHECK: [[ADDRESS:%.*]] = call ptr @__ig_pre_load(ptr %a, i32 0, i64 4, i64 4, i32 12)
; CHECK: [[LOAD:%.*]] = load i32, ptr [[ADDRESS]], align 4
; CHECK: ret i32 [[LOAD]]
entry:
  %v = load i32, ptr %a, align 4
  ret i32 %v
}

; CHECK-LABEL: define ptx_kernel void @__ig_entry(
; CHECK: [[WORKGROUP32:%.*]] = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
; CHECK: [[WORKGROUP:%.*]] = zext i32 [[WORKGROUP32]] to i64
; CHECK: [[WORKITEM32:%.*]] = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
; CHECK: [[WORKITEM:%.*]] = zext i32 [[WORKITEM32]] to i64
; CHECK: [[ARGUMENTS:%.*]] = call ptr @__ig_prepare_lane(ptr %context, i64 [[WORKGROUP]], i64 [[WORKITEM]], i64 8, i32 1)
; CHECK: [[SLOT:%.*]] = getelementptr inbounds i8, ptr [[ARGUMENTS]], i64 0
; CHECK: [[SLOT_ADDRESS:%.*]] = call ptr @__ig_pre_load(ptr [[SLOT]], i32 0, i64 8, i64 8, i32 15)
; CHECK: [[ARG:%.*]] = load ptr, ptr [[SLOT_ADDRESS]], align 8
; CHECK: [[RESULT_VAL:%.*]] = call i32 @vvv_foo(ptr [[ARG]])
; CHECK: [[RESULT_BITS:%.*]] = zext i32 [[RESULT_VAL]] to i64
; CHECK: call void @__ig_store_result(i64 [[RESULT_BITS]], i32 4)
; CHECK: ret void
