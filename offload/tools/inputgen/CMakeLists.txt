message(STATUS "Building the llvm-inputgen-gpu tool")

add_offload_tool(llvm-inputgen-gpu llvm-inputgen-gpu.cpp)

llvm_update_compile_flags(llvm-inputgen-gpu)

if(NOT TARGET inputgen_gpu_host)
  message(FATAL_ERROR
    "llvm-inputgen-gpu requires compiler-rt's inputgen_gpu_host target")
endif()

target_include_directories(llvm-inputgen-gpu PRIVATE
  ${LIBOMPTARGET_INCLUDE_DIR}
)
target_link_libraries(llvm-inputgen-gpu PRIVATE
  inputgen_gpu_host
  LLVMSupport
  omp
  omptarget
)
