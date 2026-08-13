#ifndef OPENMP_LIBOMPTARGET_PLUGINS_NEXTGEN_COMMON_INPUTGENINTERFACE_H
#define OPENMP_LIBOMPTARGET_PLUGINS_NEXTGEN_COMMON_INPUTGENINTERFACE_H

#include <stdint.h>

enum {
#define INPUTGEN_GPU_ABI_MODE(Name, Value) Name = Value,
#include "llvm/Frontend/Offloading/InputGenGPUABI.def"
};

#ifdef __cplusplus
namespace llvm {
namespace omp {
namespace target {
namespace plugin {
namespace inputgen {

#define INPUTGEN_GPU_ENTRY_STATE(Variable, Constant, CType, Symbol)          \
  inline constexpr char Constant[] = Symbol;
#include "llvm/Frontend/Offloading/InputGenGPUABI.def"

} // namespace inputgen
} // namespace plugin
} // namespace target
} // namespace omp
} // namespace llvm
#endif

#endif
