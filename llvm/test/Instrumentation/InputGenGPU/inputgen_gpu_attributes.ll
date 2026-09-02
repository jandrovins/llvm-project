; Verify instrumentation drops the attributes its runtime calls invalidate.
;
; Clang infers these from the uninstrumented body: the function touches only
; memory reachable from its arguments, never writes through the pointers, and
; captures neither.  The runtime calls this pass inserts make all three false --
; they read the launch-wide factory context, write the slice header and the
; relation table, and a pre-load fabricates a value into the very object it is
; about to hand back.  A stale memory(argmem:) in particular entitles an
; optimizer to move or delete the accesses the instrumentation exists to
; observe, so the attributes must go with the transformation, not survive it.
;
; The "amdgpu-no-*" attributes matter for a second reason.  Each asserts the
; function needs none of some implicit kernel input, and the runtime reads the
; hardware workgroup and workitem IDs to find this thread's slice.
; AMDGPUAttributor seeds an existing attribute as a known fact rather than
; re-deriving it, so a stale one is believed and the caller never passes the
; inputs the runtime needs.
;
; RUN: opt < %s -mtriple=amdgcn-amd-amdhsa -passes=inputgen-gpu \
; RUN:   -inputgen-gpu-entry-function=vvv_foo -S | FileCheck %s

target datalayout = "e-p:64:64-i64:64-n32:64"

; The whole direct-call closure is instrumented, so the whole closure is
; cleaned -- not just the entry function.
define internal i32 @helper(ptr noundef readonly captures(none) %p) #1 {
entry:
  %v = load i32, ptr %p, align 4
  ret i32 %v
}

define hidden i32 @vvv_foo(ptr noundef readonly captures(none) %a, ptr noundef readonly captures(none) %b) #0 {
entry:
  %x = load i32, ptr %a, align 4
  %y = call i32 @helper(ptr %b)
  %r = add i32 %x, %y
  ret i32 %r
}

attributes #0 = { nofree norecurse nosync nounwind memory(argmem: readwrite) "amdgpu-no-workitem-id-x" "amdgpu-no-workgroup-id-x" "amdgpu-no-implicitarg-ptr" "amdgpu-agpr-alloc"="0" "target-cpu"="gfx942" }
attributes #1 = { nofree norecurse nosync nounwind memory(argmem: read) "amdgpu-no-workitem-id-x" "amdgpu-no-workgroup-id-x" "target-cpu"="gfx942" }

; Parameter attributes are gone from both functions; nothing else about the
; signatures changes.
; CHECK: define internal i32 @helper(ptr noundef %p)
; CHECK-NOT: readonly
; CHECK-NOT: captures

; CHECK: define hidden i32 @vvv_foo(ptr noundef %a, ptr noundef %b)
; CHECK-NOT: readonly
; CHECK-NOT: captures

; Function-level memory effects and nosync are gone, and so is every
; "amdgpu-no-*" assertion.  Target attributes that stay true -- the cpu, the
; AGPR budget -- are left alone.
; CHECK: attributes #{{[0-9]+}} = { nofree norecurse nounwind "amdgpu-agpr-alloc"="0" "target-cpu"="gfx942" }
; CHECK: attributes #{{[0-9]+}} = { nofree norecurse nounwind "target-cpu"="gfx942" }
; CHECK-NOT: memory(
; CHECK-NOT: nosync
; CHECK-NOT: amdgpu-no-
