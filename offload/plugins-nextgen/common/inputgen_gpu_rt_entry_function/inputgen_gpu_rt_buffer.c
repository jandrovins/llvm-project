//===-- InputGen GPU Runtime Buffer Globals ------------------------------===//

#include "inputgen_gpu_rt_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

int *inputgen_buffer;
uint64_t inputgen_buffer_size;
uint64_t inputgen_buffer_offset;
int inputgen_mode;

int vvv_random(void) { return 9; }

#ifdef __cplusplus
}
#endif
