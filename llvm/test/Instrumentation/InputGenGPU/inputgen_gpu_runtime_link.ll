; Verify one target-neutral runtime fixture links for AMDGPU and NVPTX64.
; RUN: llvm-as %S/runtimes/inputgen_gpu_state_rt.ll -o %t.state.bc
; RUN: llvm-as %S/runtimes/inputgen_gpu_callbacks_rt.ll -o %t.callbacks.bc
; RUN: opt < %s -mtriple=amdgcn-amd-amdhsa -passes=inputgen-gpu -inputgen-gpu-runtime-bitcode=%t.state.bc -inputgen-gpu-runtime-bitcode=%t.callbacks.bc -inputgen-gpu-entry-function=vvv_foo -S | FileCheck %s --check-prefixes=COMMON,AMDGPU
; RUN: opt < %s -mtriple=nvptx64-nvidia-cuda -passes=inputgen-gpu -inputgen-gpu-runtime-bitcode=%t.state.bc -inputgen-gpu-runtime-bitcode=%t.callbacks.bc -inputgen-gpu-entry-function=vvv_foo -S | FileCheck %s --check-prefixes=COMMON,NVPTX

target datalayout = "e-p:64:64-i64:64-n32:64"

; COMMON-DAG: @inputgen_runtime_private = internal global i32 0
; COMMON-LABEL: define hidden i32 @vvv_foo(
; COMMON: call ptr @__ig_pre_load(
; AMDGPU-LABEL: define amdgpu_kernel void @__ig_entry(
; AMDGPU: define protected ptr @__ig_prepare_thread(
; AMDGPU: define protected void @__ig_store_result(
; AMDGPU: define protected ptr @__ig_pre_load(
; NVPTX-LABEL: define ptx_kernel void @__ig_entry(
; NVPTX: define protected ptr @__ig_prepare_thread(
; NVPTX: define protected void @__ig_store_result(
; NVPTX: define protected ptr @__ig_pre_load(

define hidden i32 @vvv_foo(ptr noundef %a) {
entry:
  %v = load i32, ptr %a, align 4
  ret i32 %v
}
