//===-- InputGen GPU Runtime Internals -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef INPUTGEN_GPU_RUNTIME_INTERNAL_H
#define INPUTGEN_GPU_RUNTIME_INTERNAL_H

#include <stdint.h>

#include "inputgen_gpu_factory.h"
#include "inputgen_gpu_instrumentor_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initializes or validates this GPU thread's slice and returns object zero.
void *__ig_prepare_lane(void *Context, uint64_t WorkgroupIndex,
                        uint64_t WorkitemIndex, uint64_t ArgumentBytes,
                        uint32_t PointerArgumentCount);
// Records one non-void user-function result in the current slice header.
void __ig_store_result(uint64_t Bits, uint32_t Size);
// Replaces an encoded load address with checked object data memory.
void *__ig_pre_load(void *Pointer, int32_t PointerAS, int64_t ValueSize,
                    int64_t Alignment, int32_t ValueTypeId);
// Replaces an encoded store address with checked object data memory.
void *__ig_pre_store(void *Pointer, int32_t PointerAS, int64_t ValueSize,
                     int64_t Alignment, int32_t ValueTypeId);

#ifdef __cplusplus
}
#endif

#endif
