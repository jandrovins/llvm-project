//===-- inputgen_gpu_factory.h - Shared GPU InputGen factory ABI ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This compiler-rt-private header is shared by the device runtime and the host
// codec.  It defines the in-memory factory and on-disk input layouts.  The
// launcher uses only the public C++ codec API, and LLVM IR generation uses
// opaque runtime calls; neither depends directly on these representations.
//
//===----------------------------------------------------------------------===//

#ifndef INPUTGEN_GPU_FACTORY_H
#define INPUTGEN_GPU_FACTORY_H

#include <stdint.h>

#define INPUTGEN_GPU_FACTORY_VERSION 5u
#define INPUTGEN_GPU_FACTORY_SLICE_MAGIC 0x4947534Cu
#define INPUTGEN_GPU_INPUT_MAGIC 0x494750554F424A31ULL

// Select whether callbacks fabricate input or require reconstructed input.
enum {
  INPUTGEN_MODE_GENERATE = 1,
  INPUTGEN_MODE_REPLAY = 2,
};

// Track input bytes, later writes, and logical pointer-slot bytes separately.
enum {
  INPUTGEN_GPU_MASK_READ = 1u,
  INPUTGEN_GPU_MASK_WRITTEN = 2u,
  INPUTGEN_GPU_MASK_POINTER = 4u,
};

// Describes the launch-wide factory allocation passed as the opaque context.
typedef struct InputGenGPUFactoryHeader {
  uint32_t Magic;
  uint32_t Version;
  uint32_t Mode;
  uint32_t ThreadsPerTeam;
  uint32_t NumGPUThreads;
  // Total per-GPU-thread object capacity, including argument object zero.
  uint32_t ObjectsPerThread;
  uint64_t ObjectBytes;
} InputGenGPUFactoryHeader;

// Connects one pointer slot to a target object without recording an address.
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

// Records one GPU thread's fixed slice, allocator state, and return value.
typedef struct InputGenGPUFactorySliceHeader {
  uint32_t Magic;
  uint32_t Version;
  uint32_t Error;
  uint32_t SliceIndex;
  uint32_t ObjectCount;
  uint32_t RelationCount;
  uint64_t ArgumentBytes;
  uint64_t ResultBits;
  // Aligned fallback storage returned after a callback error so the rewritten
  // immediate access can complete and the launcher can report the error.
  uint64_t ErrorScratch;
  uint32_t ResultSize;
} InputGenGPUFactorySliceHeader;

// Begins the sparse on-disk input record and fixes replay-compatible geometry.
typedef struct InputGenGPUInputFileHeader {
  uint64_t Magic;
  uint64_t ObjectBytes;
  uint64_t ArgumentBytes;
  uint32_t Version;
  uint32_t NumTeams;
  uint32_t NumThreads;
  uint32_t ObjectsPerThread;
} InputGenGPUInputFileHeader;

// Counts one GPU thread's serialized objects and pointer relations.
typedef struct InputGenGPUInputFileThreadHeader {
  uint32_t ObjectCount;
  uint32_t RelationCount;
} InputGenGPUInputFileThreadHeader;

// Counts the sparse byte ranges following one deterministic object slot.
typedef struct InputGenGPUInputFileObjectHeader {
  uint32_t NumRuns;
} InputGenGPUInputFileObjectHeader;

// Describes one contiguous recorded scalar-input range within an object.
typedef struct InputGenGPUInputFileRunHeader {
  uint32_t Offset;
  uint32_t Size;
} InputGenGPUInputFileRunHeader;

#endif
