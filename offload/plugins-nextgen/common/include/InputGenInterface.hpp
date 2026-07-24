#ifndef OPENMP_LIBOMPTARGET_PLUGINS_NEXTGEN_COMMON_INPUTGENINTERFACE_H
#define OPENMP_LIBOMPTARGET_PLUGINS_NEXTGEN_COMMON_INPUTGENINTERFACE_H

#include <stdint.h>

#define INPUTGEN_ENABLE_ENVVAR "LIBOMPTARGET_INPUTGEN"

#define INPUTGEN_STRING_BUFFER_SYMBOL "__instrumentor_gpu_log_buffer"
#define INPUTGEN_STRING_BUFFER_OFFSET_SYMBOL "__instrumentor_gpu_log_offset"
#define INPUTGEN_STRING_BUFFER_RECORDS_SYMBOL "__instrumentor_gpu_log_records"
#define INPUTGEN_STRING_BUFFER_DROPPED_SYMBOL "__instrumentor_gpu_log_dropped"

#ifndef INPUTGEN_STRING_BUFFER_DEFAULT_SIZE
#define INPUTGEN_STRING_BUFFER_DEFAULT_SIZE (1u << 20)
#endif

typedef struct {
  uint64_t Offset;
  uint64_t Records;
  uint64_t Dropped;
} InputGenStringBufferCountersTy;

#ifdef __cplusplus

#include "llvm/Support/Error.h"

namespace llvm {
namespace omp {
namespace target {
namespace plugin {

struct AsyncInfoWrapperTy;
struct GenericDeviceTy;
struct GenericKernelTy;

namespace inputgen {

llvm::Error beforeKernelLaunch(GenericDeviceTy &Device,
                               const GenericKernelTy &Kernel);
llvm::Error afterKernelLaunch(GenericDeviceTy &Device,
                              const GenericKernelTy &Kernel,
                              AsyncInfoWrapperTy &AsyncInfoWrapper,
                              bool AlreadySynchronized);

} // namespace inputgen

} // namespace plugin
} // namespace target
} // namespace omp
} // namespace llvm

#endif

#endif
