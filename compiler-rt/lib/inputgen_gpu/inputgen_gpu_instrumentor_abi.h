//===-- InputGen GPU Instrumentor ABI ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef INPUTGEN_GPU_INSTRUMENTOR_ABI_H
#define INPUTGEN_GPU_INSTRUMENTOR_ABI_H

#include <stdint.h>

// Stable InputGen GPU value kinds.  Do not use llvm::Type::TypeID values here:
// they are an implementation detail of LLVM C++ and can change independently
// of this device ABI.
enum {
  INPUTGEN_GPU_VALUE_INTEGER = 1,
  INPUTGEN_GPU_VALUE_FLOAT = 2,
  INPUTGEN_GPU_VALUE_DOUBLE = 3,
  INPUTGEN_GPU_VALUE_POINTER = 4,
};

#endif
