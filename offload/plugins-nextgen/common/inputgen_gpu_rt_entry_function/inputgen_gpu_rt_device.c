//===-- InputGen GPU Runtime Device Callbacks ----------------------------===//

#include "inputgen_gpu_rt_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Instrumentor post-load callback. Only 4-byte integer loads are
/// intercepted; every other load is passed through unmodified so pointer
/// loads are never corrupted.
int64_t __instrumentor_post_load(int64_t value, int64_t value_size,
                                 int32_t value_type_id, int32_t id) {
  (void)id;

  if (value_type_id != IntegerTyID || value_size != 4)
    return value;

  if (!inputgen_buffer)
    __builtin_trap();
  if (inputgen_buffer_offset + sizeof(int) > inputgen_buffer_size)
    __builtin_trap();

  int *slot = (int *)((char *)inputgen_buffer + inputgen_buffer_offset);

  if (inputgen_mode == INPUTGEN_MODE_GENERATE) {
    int generated = vvv_random();
    *slot = generated;
    return generated;
  }

  if (inputgen_mode == INPUTGEN_MODE_REPLAY)
    return *slot;

  __builtin_trap();
}

#ifdef __cplusplus
}
#endif
