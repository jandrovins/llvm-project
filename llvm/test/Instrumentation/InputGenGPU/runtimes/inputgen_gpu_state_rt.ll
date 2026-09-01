; Target-neutral entry-state definitions used by direct-entry linkage tests.

define protected ptr @__ig_prepare_thread(ptr %context, i64 %argument_bytes) {
  ret ptr %context
}

define protected void @__ig_store_result(i64 %bits, i32 %size) {
  ret void
}
