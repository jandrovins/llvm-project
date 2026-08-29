; Verify the AMDGPU-only factory entry rejects NVPTX without changing IR.
; RUN: opt < %s -passes=inputgen-gpu -inputgen-gpu-entry-function=vvv_foo -disable-output 2>&1 | FileCheck %s

target triple = "nvptx64-nvidia-cuda"

define hidden i32 @vvv_foo(ptr noundef %a) {
entry:
  %v = load i32, ptr %a, align 4
  ret i32 %v
}

; CHECK: InputGen GPU objects currently require AMDGPU, not target 'nvptx64-nvidia-cuda'
