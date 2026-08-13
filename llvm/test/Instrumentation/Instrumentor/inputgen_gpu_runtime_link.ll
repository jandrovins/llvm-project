; RUN: llvm-as %S/runtimes/inputgen_gpu_entry_rt.ll -o %t.rt.bc
; RUN: opt < %s -passes=inputgen-gpu -inputgen-gpu-runtime-bitcode=%t.rt.bc -inputgen-gpu-entry-function=vvv_foo -S | FileCheck %s

target datalayout = "e-m:e-p:64:64-p1:64:64-p2:32:32-p3:32:32-p4:64:64-p5:32:32-p6:32:32-p7:160:256:256:32-p8:128:128:128:48-p9:192:256:256:32-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-v2048:2048-n32:64-S32-A5-G1-ni:7:8:9"
target triple = "amdgcn-amd-amdhsa"

; CHECK-DAG: @inputgen_buffer = protected addrspace(1) global ptr null
; CHECK-DAG: @inputgen_buffer_size = protected addrspace(1) global i64 0
; CHECK-DAG: @inputgen_buffer_offset = protected addrspace(1) global i64 0
; CHECK-DAG: @inputgen_mode = protected addrspace(1) global i32 0
; CHECK-DAG: @inputgen_runtime_private = internal global i32 0
; CHECK-LABEL: define amdgpu_kernel void @__ig_entry_vvv_foo(
; CHECK: define internal i64 @__instrumentor_post_load(

define hidden i32 @vvv_foo(ptr noundef %a) {
entry:
  %v = load i32, ptr %a, align 4
  ret i32 %v
}
