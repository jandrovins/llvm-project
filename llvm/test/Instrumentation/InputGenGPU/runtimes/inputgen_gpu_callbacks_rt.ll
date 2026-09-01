; Target-neutral pre-load callback fixture used by linkage tests.

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
