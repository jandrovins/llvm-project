//===-- InputGen GPU Entry Runtime Device Callbacks ----------------------===//

#include "inputgen_gpu_entry_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Instrumentor post-load callback. Only 4-byte integer loads are intercepted;
/// every other load is passed through unmodified so pointer loads are never
/// corrupted.
int64_t __instrumentor_post_load(int64_t value, int64_t value_size,
                                 int32_t value_type_id, int32_t id) {
  (void)id;

  if (value_type_id != IntegerTyID || value_size != 4)
    return value;

  if (!InputGenEntryBuffer)
    __builtin_trap();
  if (InputGenEntryBufferOffset + sizeof(int) > InputGenEntryBufferSize)
    __builtin_trap();

  int *slot = (int *)((char *)InputGenEntryBuffer + InputGenEntryBufferOffset);

  if (InputGenEntryMode == INPUTGEN_MODE_GENERATE) {
    int generated = inputgen_entry_random();
    *slot = generated;
    return generated;
  }

  if (InputGenEntryMode == INPUTGEN_MODE_REPLAY)
    return *slot;

  __builtin_trap();
}

#ifdef __cplusplus
}
#endif
