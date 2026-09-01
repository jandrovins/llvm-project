; Verify one entry input gives AMDGPU and NVPTX64 their kernel conventions.
; RUN: opt < %s -mtriple=amdgcn-amd-amdhsa -passes=inputgen-gpu -inputgen-gpu-entry-function=vvv_foo -S | FileCheck %s --check-prefixes=COMMON,AMDGPU
; RUN: opt < %s -mtriple=nvptx64-nvidia-cuda -passes=inputgen-gpu -inputgen-gpu-entry-function=vvv_foo -S | FileCheck %s --check-prefixes=COMMON,NVPTX

target datalayout = "e-p:64:64-i64:64-n32:64"

define hidden i32 @vvv_foo(ptr noundef %a) {
; COMMON-LABEL: define hidden i32 @vvv_foo(
; COMMON-NOT: __ig_post_base_pointer_info
; COMMON: [[ADDRESS:%.*]] = call ptr @__ig_pre_load(ptr %a, i32 0, i64 4, i64 4, i32 1)
; COMMON: call i32 @__ig_error_pending()
; COMMON: [[LOAD:%.*]] = load i32, ptr [[ADDRESS]], align 4
; COMMON: ret i32 [[LOAD]]
entry:
  %v = load i32, ptr %a, align 4
  ret i32 %v
}

; AMDGPU-LABEL: define amdgpu_kernel void @__ig_entry(
; NVPTX-LABEL: define ptx_kernel void @__ig_entry(
; COMMON: [[ARGUMENTS:%.*]] = call ptr @__ig_prepare_thread(ptr %context, i64 8)
; COMMON: [[SLOT:%.*]] = getelementptr inbounds i8, ptr [[ARGUMENTS]], i64 0
; COMMON: [[SLOT_ADDRESS:%.*]] = call ptr @__ig_pre_load(ptr [[SLOT]], i32 0, i64 8, i64 8, i32 4)
; COMMON: call i32 @__ig_error_pending()
; COMMON: [[ARG:%.*]] = load ptr, ptr [[SLOT_ADDRESS]], align 8
; COMMON: [[RESULT_VAL:%.*]] = call i32 @vvv_foo(ptr [[ARG]])
; COMMON: call i32 @__ig_error_pending()
; COMMON: [[RESULT_BITS:%.*]] = zext i32 [[RESULT_VAL]] to i64
; COMMON: call void @__ig_store_result(i64 [[RESULT_BITS]], i32 4)
; COMMON: ret void
