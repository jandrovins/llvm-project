//===-- InputGen GPU Entry Runtime State ---------------------------------===//

#include "inputgen_gpu_entry_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define INPUTGEN_GPU_ENTRY_STATE(Variable, Constant, CType, Symbol)          \
  CType Variable __asm__(Symbol);
#include "llvm/Frontend/Offloading/InputGenGPUABI.def"

int inputgen_entry_random(void) { return 9; }

#ifdef __cplusplus
}
#endif
