; Verify NVPTX64 runtime bitcode provides the same direct-entry callbacks.
; RUN: llvm-as %S/runtimes/inputgen_gpu_nvptx64_state_rt.ll -o %t.state.bc
; RUN: llvm-as %S/runtimes/inputgen_gpu_nvptx64_callbacks_rt.ll -o %t.callbacks.bc
; RUN: opt < %s -passes=inputgen-gpu -inputgen-gpu-runtime-bitcode=%t.state.bc -inputgen-gpu-runtime-bitcode=%t.callbacks.bc -inputgen-gpu-entry-function=vvv_foo -S | FileCheck %s

target datalayout = "e-p6:32:32-i64:64-i128:128-v16:16-v32:32-n16:32:64"
target triple = "nvptx64-nvidia-cuda"

; CHECK-DAG: @inputgen_runtime_private = internal global i32 0
; CHECK-LABEL: define hidden i32 @vvv_foo(
; CHECK: call ptr @__ig_pre_load(
; CHECK-LABEL: define ptx_kernel void @__ig_entry(
; CHECK: define protected ptr @__ig_prepare_thread(
; CHECK: define protected void @__ig_store_result(
; CHECK: define protected ptr @__ig_pre_load(

define hidden i32 @vvv_foo(ptr noundef %a) {
entry:
  %v = load i32, ptr %a, align 4
  ret i32 %v
}
