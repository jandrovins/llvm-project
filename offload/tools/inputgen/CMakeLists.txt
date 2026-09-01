# The launcher shares the factory and input-file protocols with compiler-rt's
# host codec.  Offload is routinely built without compiler-rt (see
# openmp/docs/Building.md), so skip the tool rather than failing the configure.
# This must return before add_offload_tool so that dependents can test for the
# target instead of referring to one that was never created.
if(NOT TARGET inputgen_gpu_host)
  message(STATUS "Skipping llvm-inputgen-gpu: compiler-rt's inputgen_gpu_host "
                 "target is not available")
  return()
endif()

message(STATUS "Building the llvm-inputgen-gpu tool")

add_offload_tool(llvm-inputgen-gpu llvm-inputgen-gpu.cpp)

llvm_update_compile_flags(llvm-inputgen-gpu)

target_include_directories(llvm-inputgen-gpu PRIVATE
  ${LIBOMPTARGET_INCLUDE_DIR}
)
target_link_libraries(llvm-inputgen-gpu PRIVATE
  inputgen_gpu_host
  LLVMSupport
  omp
  omptarget
)
