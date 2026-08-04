#ifndef INPUTGEN_GPU_RT_INTERNAL_H
#define INPUTGEN_GPU_RT_INTERNAL_H

#include <stdint.h>

#include "inputgen_gpu_rt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define INPUTGEN_MODE_GENERATE 1
#define INPUTGEN_MODE_REPLAY 2

/// One-int record/replay buffer, set up by the OpenMP test driver before the
/// target region runs.
extern int *inputgen_buffer;
extern uint64_t inputgen_buffer_size;
extern uint64_t inputgen_buffer_offset;
extern int inputgen_mode;

/// Deterministic stand-in for a random number generator; always returns 9.
int vvv_random(void);

#ifdef __cplusplus
}
#endif

#endif
