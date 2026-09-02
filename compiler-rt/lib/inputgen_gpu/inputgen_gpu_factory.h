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

// The single definition of the factory's byte layout.  The host codec writes
// the buffer and the device runtime reads it, so both must derive every offset
// from this struct; neither may compute one independently.  A slice is
//
//   slice header | relation table | object 0 | object 1 | ...
//
// and each object slot is three equal-sized regions: data | mask | saved.
typedef struct InputGenGPUFactoryLayout {
  // NumTeams * NumThreads: one slice exists per GPU thread.
  uint32_t NumThreads;
  // Data capacity of a single object, from the factory header.
  uint64_t ObjectBytes;
  // Total object slots per slice, including argument object zero.
  uint32_t ObjectsPerThread;
  // Slice-relative start of the InputGenGPUFactoryPointerRelation table.
  uint64_t RelationTableOffset;
  // Slice-relative start of the first object slot.
  uint64_t ObjectStorageOffset;
  // Bytes per object slot: 3 * ObjectBytes for data, mask, and saved.
  uint64_t ObjectSlotBytes;
  // Distance between consecutive slices, and the size of one slice.
  uint64_t SliceBytes;
  // Whole factory allocation: aligned factory header + NumThreads slices.
  uint64_t TotalBytes;
} InputGenGPUFactoryLayout;

static inline uint64_t inputgenGPUAlignTo(uint64_t Value, uint64_t Alignment) {
  return (Value + Alignment - 1) & ~(Alignment - 1);
}

// Derives the layout for one geometry.  Returns 1 and fills *Layout, or 0 and
// leaves *Layout unmodified when the geometry admits no valid layout.
static inline int inputgenGPUComputeLayout(uint32_t NumTeams,
                                           uint32_t NumThreads,
                                           uint64_t ObjectBytes,
                                           uint32_t ObjectsPerThread,
                                           InputGenGPUFactoryLayout *Layout) {
  // Object data must be a nonzero multiple of eight so the three equal-sized
  // regions of a slot stay 8-byte aligned, and must fit the 32-bit offsets
  // used by the relation table and the on-disk record.
  if (!NumTeams || !NumThreads || NumTeams > UINT32_MAX / NumThreads ||
      !ObjectBytes || ObjectBytes > UINT32_MAX || (ObjectBytes & 7) ||
      !ObjectsPerThread)
    return 0;

  uint64_t RelationTableOffset =
      inputgenGPUAlignTo(sizeof(InputGenGPUFactorySliceHeader), 8);
  uint64_t RelationBytes = (uint64_t)(ObjectsPerThread - 1) *
                           sizeof(InputGenGPUFactoryPointerRelation);
  uint64_t ObjectStorageOffset =
      inputgenGPUAlignTo(RelationTableOffset + RelationBytes, 8);
  uint64_t ObjectSlotBytes = 3 * ObjectBytes;
  if ((uint64_t)ObjectsPerThread >
      (UINT64_MAX - ObjectStorageOffset) / ObjectSlotBytes)
    return 0;
  uint64_t SliceBytes =
      ObjectStorageOffset + (uint64_t)ObjectsPerThread * ObjectSlotBytes;

  uint64_t FactoryHeaderBytes =
      inputgenGPUAlignTo(sizeof(InputGenGPUFactoryHeader), 8);
  uint32_t TotalThreads = NumTeams * NumThreads;
  if ((uint64_t)TotalThreads > (UINT64_MAX - FactoryHeaderBytes) / SliceBytes)
    return 0;

  Layout->NumThreads = TotalThreads;
  Layout->ObjectBytes = ObjectBytes;
  Layout->ObjectsPerThread = ObjectsPerThread;
  Layout->RelationTableOffset = RelationTableOffset;
  Layout->ObjectStorageOffset = ObjectStorageOffset;
  Layout->ObjectSlotBytes = ObjectSlotBytes;
  Layout->SliceBytes = SliceBytes;
  Layout->TotalBytes =
      FactoryHeaderBytes + (uint64_t)TotalThreads * SliceBytes;
  return 1;
}

// Factory-relative offset of one GPU thread's slice.
static inline uint64_t
inputgenGPUSliceOffset(const InputGenGPUFactoryLayout *Layout,
                       uint32_t ThreadIndex) {
  return inputgenGPUAlignTo(sizeof(InputGenGPUFactoryHeader), 8) +
         (uint64_t)ThreadIndex * Layout->SliceBytes;
}

// Slice-relative offset of one object slot's data region.  The mask region
// follows at + ObjectBytes and the saved region at + 2 * ObjectBytes.
static inline uint64_t
inputgenGPUObjectOffset(const InputGenGPUFactoryLayout *Layout,
                        uint32_t ObjectIndex) {
  return Layout->ObjectStorageOffset +
         (uint64_t)ObjectIndex * Layout->ObjectSlotBytes;
}

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
