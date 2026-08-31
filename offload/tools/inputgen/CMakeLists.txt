message(STATUS "Building the llvm-inputgen-gpu tool")

add_offload_tool(llvm-inputgen-gpu llvm-inputgen-gpu.cpp)

llvm_update_compile_flags(llvm-inputgen-gpu)

if(LLVM_EXTERNAL_COMPILER_RT_SOURCE_DIR)
  set(INPUTGEN_GPU_RUNTIME_SOURCE_DIR
      ${LLVM_EXTERNAL_COMPILER_RT_SOURCE_DIR}/lib/inputgen_gpu)
else()
  set(INPUTGEN_GPU_RUNTIME_SOURCE_DIR
      ${CMAKE_CURRENT_SOURCE_DIR}/../../../compiler-rt/lib/inputgen_gpu)
endif()

target_include_directories(llvm-inputgen-gpu PRIVATE
  ${LIBOMPTARGET_INCLUDE_DIR}
  ${INPUTGEN_GPU_RUNTIME_SOURCE_DIR}
)
target_link_libraries(llvm-inputgen-gpu PRIVATE
  LLVMSupport
  omp
  omptarget
)
