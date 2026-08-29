//===-- inputgen_gpu_factory.h - Private GPU InputGen factory ABI ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This header is intentionally private to the InputGen GPU launcher and the
// device runtime. LLVM IR generation uses opaque runtime calls and must not
// depend on these layout details.
//
//===----------------------------------------------------------------------===//

#ifndef INPUTGEN_GPU_FACTORY_H
#define INPUTGEN_GPU_FACTORY_H

#include <stdint.h>

#define INPUTGEN_GPU_FACTORY_VERSION 2u
#define INPUTGEN_GPU_FACTORY_SLICE_MAGIC 0x4947534Cu
#define INPUTGEN_GPU_FACTORY_OBJECT_MAGIC 0x49474F42u
#define INPUTGEN_GPU_INPUT_MAGIC 0x494750554F424A31ULL

enum {
  INPUTGEN_MODE_GENERATE = 1,
  INPUTGEN_MODE_REPLAY = 2,
};

enum {
  INPUTGEN_GPU_MASK_READ = 1u,
  INPUTGEN_GPU_MASK_WRITTEN = 2u,
  INPUTGEN_GPU_MASK_POINTER = 4u,
};

/* Program-visible runtime pointers are AS0 values, not device addresses. */
#define INPUTGEN_GPU_VPTR_MAGIC UINT64_C(0xA)
#define INPUTGEN_GPU_VPTR_OFFSET_BITS 40u
#define INPUTGEN_GPU_VPTR_OFFSET_BIAS (UINT64_C(1) << 39)
#define INPUTGEN_GPU_VPTR_OBJECT_MASK ((UINT64_C(1) << 20) - 1)

typedef struct InputGenGPUFactoryHeader {
  uint32_t Magic;
  uint32_t Version;
  uint32_t Mode;
  uint32_t NumTeams;
  uint32_t ThreadsPerTeam;
  uint32_t NumLanes;
  uint64_t SliceBytes;
  uint64_t ObjectBytes;
  uint64_t FactoryBytes;
} InputGenGPUFactoryHeader;

typedef struct InputGenGPUFactoryPointerRelation {
  uint32_t OwnerObject;
  uint32_t SlotOffset;
  uint32_t TargetObject;
  uint32_t TargetOffset;
} InputGenGPUFactoryPointerRelation;

enum {
  INPUTGEN_GPU_FACTORY_ERROR_NONE = 0u,
  INPUTGEN_GPU_FACTORY_ERROR_LAYOUT = 1u,
  INPUTGEN_GPU_FACTORY_ERROR_CAPACITY = 2u,
  INPUTGEN_GPU_FACTORY_ERROR_ACCESS = 3u,
  INPUTGEN_GPU_FACTORY_ERROR_REPLAY = 4u,
  INPUTGEN_GPU_FACTORY_ERROR_TYPE = 5u,
};

typedef struct InputGenGPUFactorySliceHeader {
  uint32_t Magic;
  uint32_t Version;
  uint32_t Mode;
  uint32_t Error;
  uint32_t SliceIndex;
  uint32_t ObjectCount;
  uint32_t ObjectLimit;
  uint32_t RelationCount;
  uint32_t RelationLimit;
  uint32_t Reserved;
  uint64_t ArgumentBytes;
  uint64_t ObjectBytes;
  uint64_t ObjectTableOffset;
  uint64_t RelationTableOffset;
  uint64_t NextOffset;
  uint64_t ResultBits;
  uint32_t ResultSize;
  uint32_t ResultReserved;
} InputGenGPUFactorySliceHeader;

typedef struct InputGenGPUFactoryObjectHeader {
  uint32_t Magic;
  uint32_t ObjectIndex;
  uint32_t Capacity;
  uint32_t SliceOffset;
} InputGenGPUFactoryObjectHeader;

typedef struct InputGenGPUInputFileHeader {
  uint64_t Magic;
  uint32_t Version;
  uint32_t NumTeams;
  uint32_t NumThreads;
  uint32_t NumLanes;
  uint64_t SliceBytes;
  uint64_t ObjectBytes;
  uint64_t ResultStride;
} InputGenGPUInputFileHeader;

typedef struct InputGenGPUInputFileLaneHeader {
  uint32_t ObjectCount;
  uint32_t RelationCount;
} InputGenGPUInputFileLaneHeader;

typedef struct InputGenGPUInputFileRelation {
  uint32_t OwnerObject;
  uint32_t SlotOffset;
  uint32_t TargetObject;
  uint32_t TargetOffset;
} InputGenGPUInputFileRelation;

typedef struct InputGenGPUInputFileObjectHeader {
  uint32_t Capacity;
  uint32_t NumRuns;
} InputGenGPUInputFileObjectHeader;

typedef struct InputGenGPUInputFileRunHeader {
  uint32_t Offset;
  uint32_t Size;
} InputGenGPUInputFileRunHeader;

#endif
