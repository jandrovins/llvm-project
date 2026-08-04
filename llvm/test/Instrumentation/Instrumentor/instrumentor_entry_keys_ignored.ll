; RUN: opt < %s -passes=instrumentor -instrumentor-read-config-files=%S/instrumentor_entry_keys_ignored_config.json -S 2>&1 | FileCheck %s

target datalayout = "e-m:e-p:64:64-p1:64:64-p2:32:32-p3:32:32-p4:64:64-p5:32:32-p6:32:32-p7:160:256:256:32-p8:128:128:128:48-p9:192:256:256:32-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-v2048:2048-n32:64-S32-A5-G1-ni:7:8:9"
target triple = "amdgcn-amd-amdhsa"

; CHECK: configuration key 'entry_function' not found and ignored
; CHECK: configuration key 'entry_point_name' not found and ignored
; CHECK-NOT: define {{.*}}void @__instrumentor_entry

define hidden i32 @vvv_foo(ptr noundef %a) {
entry:
  %v = load i32, ptr %a, align 4
  ret i32 %v
}
