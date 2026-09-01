; Verify pointer stores are diagnosed instead of producing an entry kernel
; whose logical relations would become stale.
; RUN: opt < %s -passes=inputgen-gpu -inputgen-gpu-entry-function=vvv_foo -S 2>&1 | FileCheck %s

target datalayout = "e-p:64:64"
target triple = "amdgcn-amd-amdhsa"

define hidden void @vvv_foo(ptr %slot, ptr %value) {
entry:
  store ptr %value, ptr %slot, align 8
  ret void
}

; CHECK: warning: InputGen GPU does not yet support pointer stores in 'vvv_foo'; logical relation updates must be implemented first
; CHECK-NOT: define amdgpu_kernel void @__ig_entry
