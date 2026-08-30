; Provide a pre-load callback runtime fixture for linkage tests.
target datalayout = "e-m:e-p:64:64-p1:64:64-p2:32:32-p3:32:32-p4:64:64-p5:32:32-p6:32:32-p7:160:256:256:32-p8:128:128:128:48-p9:192:256:256:32-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256:256-v512:512-v1024:1024-v2048:2048-n32:64-S32-A5-G1-ni:7:8:9"
target triple = "amdgcn-amd-amdhsa"

@inputgen_runtime_private = internal global i32 0

define protected ptr @__ig_pre_load(ptr %pointer, i32 %pointer_as,
                                    i64 %value_size, i64 %alignment,
                                    i32 %value_type_id) {
entry:
  %private = load i32, ptr @inputgen_runtime_private, align 4
  %next = add i32 %private, 1
  store i32 %next, ptr @inputgen_runtime_private, align 4
  ret ptr %pointer
}
