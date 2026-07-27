//===-- InputGen GPU Runtime Buffer Utilities ----------------------------===//

#include "inputgen_gpu_rt_internal.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

char InputGenStringBuffer[INPUTGEN_GPU_RT_BUFFER_SIZE]
    __asm__(INPUTGEN_STRING_BUFFER_SYMBOL);
uint64_t InputGenStringBufferOffset 
    __asm__(INPUTGEN_STRING_BUFFER_OFFSET_SYMBOL) = 0;
uint64_t InputGenStringBufferRecords
    __asm__(INPUTGEN_STRING_BUFFER_RECORDS_SYMBOL) = 0;
uint64_t InputGenStringBufferDropped
    __asm__(INPUTGEN_STRING_BUFFER_DROPPED_SYMBOL) = 0;

uint64_t ig_atomic_add_u64(uint64_t *addr, uint64_t value) {
  return __atomic_fetch_add(addr, value, __ATOMIC_RELAXED);
}

void ig_line_char(IgLine *line, char c) {
  if (line->pos + 1u < line->cap)
    line->data[line->pos++] = c;
}

void ig_line_str(IgLine *line, const char *value) {
  if (!value) {
    ig_line_str(line, "(null)");
    return;
  }

  while (*value)
    ig_line_char(line, *value++);
}

static void ig_line_u64(IgLine *line, uint64_t value) {
  char tmp[32];
  uint32_t len = 0;

  if (value == 0) {
    ig_line_char(line, '0');
    return;
  }

  while (value != 0 && len < (uint32_t)sizeof(tmp)) {
    tmp[len++] = (char)('0' + (value % 10u));
    value /= 10u;
  }

  while (len != 0)
    ig_line_char(line, tmp[--len]);
}

void ig_line_i64(IgLine *line, int64_t value) {
  if (value < 0) {
    ig_line_char(line, '-');
    ig_line_u64(line, (uint64_t)(-(value + 1)) + 1u);
    return;
  }

  ig_line_u64(line, (uint64_t)value);
}

void ig_line_i32(IgLine *line, int32_t value) {
  ig_line_i64(line, (int64_t)value);
}

static void ig_line_hex_u64(IgLine *line, uint64_t value) {
  int started = 0;

  ig_line_str(line, "0x");
  for (int shift = 60; shift >= 0; shift -= 4) {
    uint32_t nibble = (uint32_t)((value >> (uint32_t)shift) & 0xfu);
    if (nibble != 0 || started || shift == 0) {
      ig_line_char(line,
                   (char)(nibble < 10u ? '0' + nibble : 'a' + nibble - 10u));
      started = 1;
    }
  }
}

void ig_line_ptr(IgLine *line, const void *value) {
  ig_line_hex_u64(line, (uint64_t)(uintptr_t)value);
}

const char *ig_type_name(int32_t type_id) {
  switch (type_id) {
  case -1:
    return "none";
  case HalfTyID:
    return "half";
  case BFloatTyID:
    return "bfloat";
  case FloatTyID:
    return "float";
  case DoubleTyID:
    return "double";
  case X86_FP80TyID:
    return "x86_fp80";
  case FP128TyID:
    return "fp128";
  case PPC_FP128TyID:
    return "ppc_fp128";
  case VoidTyID:
    return "void";
  case LabelTyID:
    return "label";
  case MetadataTyID:
    return "metadata";
  case X86_AMXTyID:
    return "x86_amx";
  case TokenTyID:
    return "token";
  case IntegerTyID:
  case ByteTyID:
    return "integer";
  case FunctionTyID:
    return "function";
  case PointerTyID:
    return "pointer";
  case StructTyID:
    return "struct";
  case ArrayTyID:
    return "array";
  case FixedVectorTyID:
    return "fixed_vector";
  case ScalableVectorTyID:
    return "scalable_vector";
  case TypedPointerTyID:
    return "typed_pointer";
  case TargetExtTyID:
    return "target_ext";
  default:
    return "unknown";
  }
}

void ig_emit_line(const char *line, uint32_t len) {
  if (len == 0)
    return;

  uint64_t pos = ig_atomic_add_u64(&InputGenStringBufferOffset, len);
  if (pos + len > (uint64_t)INPUTGEN_GPU_RT_BUFFER_SIZE) {
    ig_atomic_add_u64(&InputGenStringBufferDropped, 1u);
    return;
  }

  for (uint32_t i = 0; i < len; ++i)
    InputGenStringBuffer[pos + i] = line[i];

  ig_atomic_add_u64(&InputGenStringBufferRecords, 1u);
}

void ig_finish(IgLine *line) {
  if (line->pos + 1u < line->cap)
    line->data[line->pos++] = '\n';
  else if (line->cap != 0)
    line->data[line->cap - 1u] = '\n';

  ig_emit_line(line->data, line->pos);
}

#ifdef __cplusplus
}
#endif
