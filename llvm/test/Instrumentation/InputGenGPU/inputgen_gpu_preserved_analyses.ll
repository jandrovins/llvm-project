; Verify wrapper creation invalidates analyses even when the selected function
; contains no instrumentable memory operation.
; RUN: opt < %s -passes='require<callgraph>,inputgen-gpu,print-callgraph' -inputgen-gpu-entry-function=empty -disable-output 2>&1 | FileCheck %s

target triple = "amdgcn-amd-amdhsa"

define void @empty() {
  ret void
}

; CHECK: Call graph node for function: '__ig_entry'
