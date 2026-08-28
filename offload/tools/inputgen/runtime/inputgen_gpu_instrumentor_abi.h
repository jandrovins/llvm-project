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

// llvm::Type::TypeID values. Keep this minimal device ABI independent of LLVM
// C++ and only expose the scalar types supported by the first object model.
enum { FloatTyID = 2, DoubleTyID = 3, IntegerTyID = 12, PointerTyID = 14 };

#endif
