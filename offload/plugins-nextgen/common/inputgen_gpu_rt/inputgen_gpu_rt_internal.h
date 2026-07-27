#ifndef INPUTGEN_GPU_RT_INTERNAL_H
#define INPUTGEN_GPU_RT_INTERNAL_H

#include <stdint.h>

#include "InputGenInterface.hpp"

#ifndef INPUTGEN_GPU_RT_BUFFER_SIZE
#define INPUTGEN_GPU_RT_BUFFER_SIZE INPUTGEN_STRING_BUFFER_DEFAULT_SIZE
#endif

#ifndef INPUTGEN_GPU_RT_MAX_LINE
#define INPUTGEN_GPU_RT_MAX_LINE 512u
#endif

#include "inputgen_gpu_rt.h"

#ifdef __cplusplus
extern "C" {
#endif

extern char InputGenStringBuffer[INPUTGEN_GPU_RT_BUFFER_SIZE]
    __asm__(INPUTGEN_STRING_BUFFER_SYMBOL);
extern uint64_t InputGenStringBufferOffset
    __asm__(INPUTGEN_STRING_BUFFER_OFFSET_SYMBOL);
extern uint64_t InputGenStringBufferRecords
    __asm__(INPUTGEN_STRING_BUFFER_RECORDS_SYMBOL);
extern uint64_t InputGenStringBufferDropped
    __asm__(INPUTGEN_STRING_BUFFER_DROPPED_SYMBOL);

typedef struct {
  char *data;
  uint32_t pos;
  uint32_t cap;
} IgLine;

uint64_t ig_atomic_add_u64(uint64_t *addr, uint64_t value);
void ig_line_char(IgLine *line, char c);
void ig_line_str(IgLine *line, const char *value);
void ig_line_i32(IgLine *line, int32_t value);
void ig_line_i64(IgLine *line, int64_t value);
void ig_line_ptr(IgLine *line, const void *value);
const char *ig_type_name(int32_t type_id);
void ig_emit_line(const char *line, uint32_t len);
void ig_finish(IgLine *line);

#ifdef __cplusplus
}
#endif

#define IG_LINE_BEGIN(NAME)                                                    \
  char NAME##_storage[INPUTGEN_GPU_RT_MAX_LINE];                               \
  IgLine NAME = {NAME##_storage, 0u, INPUTGEN_GPU_RT_MAX_LINE}

#define IG_FIELD_STR(LINE, LABEL, VALUE)                                       \
  do {                                                                         \
    ig_line_str(&(LINE), LABEL);                                               \
    ig_line_str(&(LINE), VALUE);                                               \
  } while (0)

#define IG_FIELD_I32(LINE, LABEL, VALUE)                                       \
  do {                                                                         \
    ig_line_str(&(LINE), LABEL);                                               \
    ig_line_i32(&(LINE), (int32_t)(VALUE));                                    \
  } while (0)

#define IG_FIELD_I64(LINE, LABEL, VALUE)                                       \
  do {                                                                         \
    ig_line_str(&(LINE), LABEL);                                               \
    ig_line_i64(&(LINE), (int64_t)(VALUE));                                    \
  } while (0)

#define IG_FIELD_PTR(LINE, LABEL, VALUE)                                       \
  do {                                                                         \
    ig_line_str(&(LINE), LABEL);                                               \
    ig_line_ptr(&(LINE), (const void *)(VALUE));                               \
  } while (0)

#endif
