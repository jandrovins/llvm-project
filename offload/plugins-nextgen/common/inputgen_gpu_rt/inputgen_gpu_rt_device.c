//===-- InputGen GPU Runtime Device Callbacks ----------------------------===//

#include "inputgen_gpu_rt_internal.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void __instrumentor_pre_module(char *module_name, char *target_triple,
                               int32_t id) {
  IG_LINE_BEGIN(line);
  ig_line_str(&line, "module pre -- ");
  IG_FIELD_STR(line, "module_name: ", module_name);
  IG_FIELD_STR(line, ", target_triple: ", target_triple);
  IG_FIELD_I32(line, ", id: ", id);
  ig_finish(&line);
}

void __instrumentor_post_module(char *module_name, char *target_triple,
                                int32_t id) {
  IG_LINE_BEGIN(line);
  ig_line_str(&line, "module post -- ");
  IG_FIELD_STR(line, "module_name: ", module_name);
  IG_FIELD_STR(line, ", target_triple: ", target_triple);
  IG_FIELD_I32(line, ", id: ", id);
  ig_finish(&line);
}

void *__instrumentor_pre_global(
    void *address, int32_t address_space, int64_t declared_size,
    int64_t alignment, char *name, int64_t initial_value, int8_t is_constant,
    int8_t is_definition, int32_t id) {
  IG_LINE_BEGIN(line);
  ig_line_str(&line, "global pre -- ");
  IG_FIELD_PTR(line, "address: ", address);
  IG_FIELD_I32(line, ", address_space: ", address_space);
  IG_FIELD_I64(line, ", declared_size: ", declared_size);
  IG_FIELD_I64(line, ", alignment: ", alignment);
  IG_FIELD_STR(line, ", name: ", name);
  IG_FIELD_I64(line, ", initial_value: ", initial_value);
  IG_FIELD_I32(line, ", is_constant: ", is_constant);
  IG_FIELD_I32(line, ", is_definition: ", is_definition);
  IG_FIELD_I32(line, ", id: ", id);
  ig_finish(&line);
  return address;
}

void *__instrumentor_pre_global_ind(
    void *address, int32_t address_space, int64_t declared_size,
    int64_t alignment, char *name, int64_t *initial_value_ptr,
    int8_t is_constant, int8_t is_definition, int32_t id) {
  IG_LINE_BEGIN(line);
  ig_line_str(&line, "global pre -- ");
  IG_FIELD_PTR(line, "address: ", address);
  IG_FIELD_I32(line, ", address_space: ", address_space);
  IG_FIELD_I64(line, ", declared_size: ", declared_size);
  IG_FIELD_I64(line, ", alignment: ", alignment);
  IG_FIELD_STR(line, ", name: ", name);
  IG_FIELD_PTR(line, ", initial_value: ", initial_value_ptr);
  IG_FIELD_I32(line, ", is_constant: ", is_constant);
  IG_FIELD_I32(line, ", is_definition: ", is_definition);
  IG_FIELD_I32(line, ", id: ", id);
  ig_finish(&line);
  return address;
}

void __instrumentor_post_global(
    void *address, int32_t address_space, int64_t declared_size,
    int64_t alignment, char *name, int64_t initial_value, int8_t is_constant,
    int8_t is_definition, int32_t id) {
  IG_LINE_BEGIN(line);
  ig_line_str(&line, "global post -- ");
  IG_FIELD_PTR(line, "address: ", address);
  IG_FIELD_I32(line, ", address_space: ", address_space);
  IG_FIELD_I64(line, ", declared_size: ", declared_size);
  IG_FIELD_I64(line, ", alignment: ", alignment);
  IG_FIELD_STR(line, ", name: ", name);
  IG_FIELD_I64(line, ", initial_value: ", initial_value);
  IG_FIELD_I32(line, ", is_constant: ", is_constant);
  IG_FIELD_I32(line, ", is_definition: ", is_definition);
  IG_FIELD_I32(line, ", id: ", id);
  ig_finish(&line);
}

void __instrumentor_post_global_ind(
    void *address, int32_t address_space, int64_t declared_size,
    int64_t alignment, char *name, int64_t *initial_value_ptr,
    int8_t is_constant, int8_t is_definition, int32_t id) {
  IG_LINE_BEGIN(line);
  ig_line_str(&line, "global post -- ");
  IG_FIELD_PTR(line, "address: ", address);
  IG_FIELD_I32(line, ", address_space: ", address_space);
  IG_FIELD_I64(line, ", declared_size: ", declared_size);
  IG_FIELD_I64(line, ", alignment: ", alignment);
  IG_FIELD_STR(line, ", name: ", name);
  IG_FIELD_PTR(line, ", initial_value: ", initial_value_ptr);
  IG_FIELD_I32(line, ", is_constant: ", is_constant);
  IG_FIELD_I32(line, ", is_definition: ", is_definition);
  IG_FIELD_I32(line, ", id: ", id);
  ig_finish(&line);
}

void *__instrumentor_pre_load(
    void *pointer, int32_t pointer_as, void *base_pointer_info,
    int64_t value_size, int64_t alignment, int32_t value_type_id,
    int32_t value_sub_type_id, int32_t atomicity_ordering,
    int8_t sync_scope_id, int8_t is_volatile, int32_t id) {
  IG_LINE_BEGIN(line);
  ig_line_str(&line, "load pre -- ");
  IG_FIELD_PTR(line, "pointer: ", pointer);
  IG_FIELD_I32(line, ", pointer_as: ", pointer_as);
  IG_FIELD_PTR(line, ", base_pointer_info: ", base_pointer_info);
  IG_FIELD_I64(line, ", value_size: ", value_size);
  IG_FIELD_I64(line, ", alignment: ", alignment);
  IG_FIELD_STR(line, ", value_type_id: ", ig_type_name(value_type_id));
  IG_FIELD_STR(line, ", value_sub_type_id: ", ig_type_name(value_sub_type_id));
  IG_FIELD_I32(line, ", atomicity_ordering: ", atomicity_ordering);
  IG_FIELD_I32(line, ", sync_scope_id: ", sync_scope_id);
  IG_FIELD_I32(line, ", is_volatile: ", is_volatile);
  IG_FIELD_I32(line, ", id: ", id);
  ig_finish(&line);
  return pointer;
}

void *__instrumentor_pre_store(
    void *pointer, int32_t pointer_as, void *base_pointer_info, int64_t value,
    int64_t value_size, int64_t alignment, int32_t value_type_id,
    int32_t value_sub_type_id, int32_t atomicity_ordering,
    int8_t sync_scope_id, int8_t is_volatile, int32_t id) {
  IG_LINE_BEGIN(line);
  ig_line_str(&line, "store pre -- ");
  IG_FIELD_PTR(line, "pointer: ", pointer);
  IG_FIELD_I32(line, ", pointer_as: ", pointer_as);
  IG_FIELD_PTR(line, ", base_pointer_info: ", base_pointer_info);
  IG_FIELD_I64(line, ", value: ", value);
  IG_FIELD_I64(line, ", value_size: ", value_size);
  IG_FIELD_I64(line, ", alignment: ", alignment);
  IG_FIELD_STR(line, ", value_type_id: ", ig_type_name(value_type_id));
  IG_FIELD_STR(line, ", value_sub_type_id: ", ig_type_name(value_sub_type_id));
  IG_FIELD_I32(line, ", atomicity_ordering: ", atomicity_ordering);
  IG_FIELD_I32(line, ", sync_scope_id: ", sync_scope_id);
  IG_FIELD_I32(line, ", is_volatile: ", is_volatile);
  IG_FIELD_I32(line, ", id: ", id);
  ig_finish(&line);
  return pointer;
}

void *__instrumentor_pre_store_ind(
    void *pointer, int32_t pointer_as, void *base_pointer_info,
    int64_t *value_ptr, int64_t value_size, int64_t alignment,
    int32_t value_type_id, int32_t value_sub_type_id,
    int32_t atomicity_ordering, int8_t sync_scope_id, int8_t is_volatile,
    int32_t id) {
  IG_LINE_BEGIN(line);
  ig_line_str(&line, "store pre -- ");
  IG_FIELD_PTR(line, "pointer: ", pointer);
  IG_FIELD_I32(line, ", pointer_as: ", pointer_as);
  IG_FIELD_PTR(line, ", base_pointer_info: ", base_pointer_info);
  IG_FIELD_PTR(line, ", value: ", value_ptr);
  IG_FIELD_I64(line, ", value_size: ", value_size);
  IG_FIELD_I64(line, ", alignment: ", alignment);
  IG_FIELD_STR(line, ", value_type_id: ", ig_type_name(value_type_id));
  IG_FIELD_STR(line, ", value_sub_type_id: ", ig_type_name(value_sub_type_id));
  IG_FIELD_I32(line, ", atomicity_ordering: ", atomicity_ordering);
  IG_FIELD_I32(line, ", sync_scope_id: ", sync_scope_id);
  IG_FIELD_I32(line, ", is_volatile: ", is_volatile);
  IG_FIELD_I32(line, ", id: ", id);
  ig_finish(&line);
  return pointer;
}

int64_t __instrumentor_post_load(
    void *pointer, int32_t pointer_as, void *base_pointer_info, int64_t value,
    int64_t value_size, int64_t alignment, int32_t value_type_id,
    int32_t value_sub_type_id, int32_t atomicity_ordering,
    int8_t sync_scope_id, int8_t is_volatile, int32_t id) {
  IG_LINE_BEGIN(line);
  ig_line_str(&line, "load post -- ");
  IG_FIELD_PTR(line, "pointer: ", pointer);
  IG_FIELD_I32(line, ", pointer_as: ", pointer_as);
  IG_FIELD_PTR(line, ", base_pointer_info: ", base_pointer_info);
  IG_FIELD_I64(line, ", value: ", value);
  IG_FIELD_I64(line, ", value_size: ", value_size);
  IG_FIELD_I64(line, ", alignment: ", alignment);
  IG_FIELD_STR(line, ", value_type_id: ", ig_type_name(value_type_id));
  IG_FIELD_STR(line, ", value_sub_type_id: ", ig_type_name(value_sub_type_id));
  IG_FIELD_I32(line, ", atomicity_ordering: ", atomicity_ordering);
  IG_FIELD_I32(line, ", sync_scope_id: ", sync_scope_id);
  IG_FIELD_I32(line, ", is_volatile: ", is_volatile);
  IG_FIELD_I32(line, ", id: ", id);
  ig_finish(&line);
  return value;
}

void __instrumentor_post_load_ind(
    void *pointer, int32_t pointer_as, void *base_pointer_info,
    int64_t *value_ptr, int64_t value_size, int64_t alignment,
    int32_t value_type_id, int32_t value_sub_type_id,
    int32_t atomicity_ordering, int8_t sync_scope_id, int8_t is_volatile,
    int32_t id) {
  IG_LINE_BEGIN(line);
  ig_line_str(&line, "load post -- ");
  IG_FIELD_PTR(line, "pointer: ", pointer);
  IG_FIELD_I32(line, ", pointer_as: ", pointer_as);
  IG_FIELD_PTR(line, ", base_pointer_info: ", base_pointer_info);
  IG_FIELD_PTR(line, ", value: ", value_ptr);
  IG_FIELD_I64(line, ", value_size: ", value_size);
  IG_FIELD_I64(line, ", alignment: ", alignment);
  IG_FIELD_STR(line, ", value_type_id: ", ig_type_name(value_type_id));
  IG_FIELD_STR(line, ", value_sub_type_id: ", ig_type_name(value_sub_type_id));
  IG_FIELD_I32(line, ", atomicity_ordering: ", atomicity_ordering);
  IG_FIELD_I32(line, ", sync_scope_id: ", sync_scope_id);
  IG_FIELD_I32(line, ", is_volatile: ", is_volatile);
  IG_FIELD_I32(line, ", id: ", id);
  ig_finish(&line);
}

void __instrumentor_post_store(
    void *pointer, int32_t pointer_as, void *base_pointer_info, int64_t value,
    int64_t value_size, int64_t alignment, int32_t value_type_id,
    int32_t value_sub_type_id, int32_t atomicity_ordering,
    int8_t sync_scope_id, int8_t is_volatile, int32_t id) {
  IG_LINE_BEGIN(line);
  ig_line_str(&line, "store post -- ");
  IG_FIELD_PTR(line, "pointer: ", pointer);
  IG_FIELD_I32(line, ", pointer_as: ", pointer_as);
  IG_FIELD_PTR(line, ", base_pointer_info: ", base_pointer_info);
  IG_FIELD_I64(line, ", value: ", value);
  IG_FIELD_I64(line, ", value_size: ", value_size);
  IG_FIELD_I64(line, ", alignment: ", alignment);
  IG_FIELD_STR(line, ", value_type_id: ", ig_type_name(value_type_id));
  IG_FIELD_STR(line, ", value_sub_type_id: ", ig_type_name(value_sub_type_id));
  IG_FIELD_I32(line, ", atomicity_ordering: ", atomicity_ordering);
  IG_FIELD_I32(line, ", sync_scope_id: ", sync_scope_id);
  IG_FIELD_I32(line, ", is_volatile: ", is_volatile);
  IG_FIELD_I32(line, ", id: ", id);
  ig_finish(&line);
}

void __instrumentor_post_store_ind(
    void *pointer, int32_t pointer_as, void *base_pointer_info,
    int64_t *value_ptr, int64_t value_size, int64_t alignment,
    int32_t value_type_id, int32_t value_sub_type_id,
    int32_t atomicity_ordering, int8_t sync_scope_id, int8_t is_volatile,
    int32_t id) {
  IG_LINE_BEGIN(line);
  ig_line_str(&line, "store post -- ");
  IG_FIELD_PTR(line, "pointer: ", pointer);
  IG_FIELD_I32(line, ", pointer_as: ", pointer_as);
  IG_FIELD_PTR(line, ", base_pointer_info: ", base_pointer_info);
  IG_FIELD_PTR(line, ", value: ", value_ptr);
  IG_FIELD_I64(line, ", value_size: ", value_size);
  IG_FIELD_I64(line, ", alignment: ", alignment);
  IG_FIELD_STR(line, ", value_type_id: ", ig_type_name(value_type_id));
  IG_FIELD_STR(line, ", value_sub_type_id: ", ig_type_name(value_sub_type_id));
  IG_FIELD_I32(line, ", atomicity_ordering: ", atomicity_ordering);
  IG_FIELD_I32(line, ", sync_scope_id: ", sync_scope_id);
  IG_FIELD_I32(line, ", is_volatile: ", is_volatile);
  IG_FIELD_I32(line, ", id: ", id);
  ig_finish(&line);
}

void *__instrumentor_post_base_pointer_info(
    void *base_pointer, int32_t base_pointer_kind, int32_t id) {
  IG_LINE_BEGIN(line);
  ig_line_str(&line, "base_pointer_info post -- ");
  IG_FIELD_PTR(line, "base_pointer: ", base_pointer);
  IG_FIELD_I32(line, ", base_pointer_kind: ", base_pointer_kind);
  IG_FIELD_I32(line, ", id: ", id);
  ig_finish(&line);
  return base_pointer;
}

#ifdef __cplusplus
}
#endif
