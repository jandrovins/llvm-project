message(STATUS "Building the llvm-inputgen-gpu tool")

add_offload_tool(llvm-inputgen-gpu llvm-inputgen-gpu.cpp)

llvm_update_compile_flags(llvm-inputgen-gpu)

target_include_directories(llvm-inputgen-gpu PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../plugins-nextgen/common/include
  ${LIBOMPTARGET_INCLUDE_DIR}
)
target_link_libraries(llvm-inputgen-gpu PRIVATE
  LLVMSupport
  omp
  omptarget
)
