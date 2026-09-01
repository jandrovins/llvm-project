; Provide entry-state definitions used by NVPTX64 direct-entry linkage tests.
target datalayout = "e-p6:32:32-i64:64-i128:128-v16:16-v32:32-n16:32:64"
target triple = "nvptx64-nvidia-cuda"

define protected ptr @__ig_prepare_thread(ptr %context, i64 %argument_bytes,
                                          i32 %pointer_count) {
  ret ptr %context
}

define protected void @__ig_store_result(i64 %bits, i32 %size) {
  ret void
}
