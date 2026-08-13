if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20.0)
  project(InputGenGPU C)
endif()

set(INPUTGEN_GPU_RT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(INPUTGEN_GPU_RT_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})
set(INPUTGEN_GPU_RT_INTERFACE_INCLUDE_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugins-nextgen/common/include)
if(INPUTGEN_GPU_RT_LLVM_INCLUDE_DIR)
  # Use an explicitly configured LLVM include directory.
elseif(LLVM_MAIN_INCLUDE_DIR)
  set(INPUTGEN_GPU_RT_LLVM_INCLUDE_DIR ${LLVM_MAIN_INCLUDE_DIR})
else()
  get_filename_component(INPUTGEN_GPU_RT_LLVM_INCLUDE_DIR
                         ${CMAKE_CURRENT_SOURCE_DIR}/../../llvm/include
                         ABSOLUTE)
endif()

set(INPUTGEN_GPU_ENTRY_RUNTIME
    ${INPUTGEN_GPU_RT_BINARY_DIR}/inputgen_gpu_entry_runtime.bc)
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(INPUTGEN_GPU_ENTRY_RUNTIME ${INPUTGEN_GPU_ENTRY_RUNTIME} PARENT_SCOPE)
endif()

if(INPUTGEN_GPU_RT_ARCH)
  # Use an explicitly configured arch.
elseif(DEFINED ENV{GPU_ARCH} AND NOT "$ENV{GPU_ARCH}" STREQUAL "")
  set(INPUTGEN_GPU_RT_ARCH $ENV{GPU_ARCH})
elseif(LIBOMPTARGET_AMDGPU_DETECTED_ARCH_LIST)
  list(GET LIBOMPTARGET_AMDGPU_DETECTED_ARCH_LIST 0 INPUTGEN_GPU_RT_ARCH)
elseif(LIBOMPTARGET_FORCE_AMDGPU_TESTS)
  set(INPUTGEN_GPU_RT_ARCH native)
endif()

if(NOT INPUTGEN_GPU_RT_ARCH)
  message(STATUS "Not building InputGen GPU runtime bitcode: no AMDGPU arch detected")
  return()
endif()

if(INPUTGEN_GPU_RT_CLANG)
  # Use an explicitly configured clang.
elseif(OPENMP_TEST_C_COMPILER)
  set(INPUTGEN_GPU_RT_CLANG ${OPENMP_TEST_C_COMPILER})
else()
  set(INPUTGEN_GPU_RT_CLANG ${CMAKE_C_COMPILER})
endif()
if(NOT INPUTGEN_GPU_RT_CLANG)
  find_program(INPUTGEN_GPU_RT_CLANG clang REQUIRED)
endif()

if(NOT INPUTGEN_GPU_RT_LLVM_LINK)
  if(LLVM_TOOLS_BINARY_DIR)
    set(INPUTGEN_GPU_RT_LLVM_LINK ${LLVM_TOOLS_BINARY_DIR}/llvm-link)
  else()
    find_program(INPUTGEN_GPU_RT_LLVM_LINK llvm-link REQUIRED)
  endif()
endif()

set(INPUTGEN_GPU_RT_COMMON_FLAGS
  -DINPUTGEN_GPU_RT_DEVICE=1
  -I${INPUTGEN_GPU_RT_SOURCE_DIR}
  -I${INPUTGEN_GPU_RT_INTERFACE_INCLUDE_DIR}
  -I${INPUTGEN_GPU_RT_LLVM_INCLUDE_DIR}
  -O3
  -g
  -Wall
  -Wextra
  -std=c11
  --target=amdgcn-amd-amdhsa
  -march=${INPUTGEN_GPU_RT_ARCH}
  -nogpulib
  -nostdlibinc
  -fconvergent-functions
  -fvisibility=protected
  -flto
  -c
  -emit-llvm)

set(INPUTGEN_GPU_RT_COMMON_DEPS
  ${INPUTGEN_GPU_RT_SOURCE_DIR}/inputgen_gpu_instrumentor_abi.h
  ${INPUTGEN_GPU_RT_SOURCE_DIR}/inputgen_gpu_entry_internal.h
  ${INPUTGEN_GPU_RT_INTERFACE_INCLUDE_DIR}/InputGenInterface.hpp
  ${INPUTGEN_GPU_RT_LLVM_INCLUDE_DIR}/llvm/Frontend/Offloading/InputGenGPUABI.def)

function(add_inputgen_gpu_runtime_bc output source)
  add_custom_command(
    OUTPUT ${output}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${INPUTGEN_GPU_RT_BINARY_DIR}
    COMMAND ${INPUTGEN_GPU_RT_CLANG} ${INPUTGEN_GPU_RT_COMMON_FLAGS}
            ${INPUTGEN_GPU_RT_SOURCE_DIR}/${source} -o ${output}
    DEPENDS ${INPUTGEN_GPU_RT_SOURCE_DIR}/${source}
            ${INPUTGEN_GPU_RT_COMMON_DEPS}
    COMMENT "Building ${source} InputGen GPU runtime bitcode"
    VERBATIM)
endfunction()

add_inputgen_gpu_runtime_bc(
  ${INPUTGEN_GPU_RT_BINARY_DIR}/inputgen_gpu_entry_callbacks.bc
  inputgen_gpu_entry_callbacks.c)
add_inputgen_gpu_runtime_bc(
  ${INPUTGEN_GPU_RT_BINARY_DIR}/inputgen_gpu_entry_state.bc
  inputgen_gpu_entry_state.c)

add_custom_command(
  OUTPUT ${INPUTGEN_GPU_RT_BINARY_DIR}/inputgen_gpu_entry_runtime.bc
  COMMAND ${INPUTGEN_GPU_RT_LLVM_LINK}
          ${INPUTGEN_GPU_RT_BINARY_DIR}/inputgen_gpu_entry_callbacks.bc
          ${INPUTGEN_GPU_RT_BINARY_DIR}/inputgen_gpu_entry_state.bc
          -o ${INPUTGEN_GPU_RT_BINARY_DIR}/inputgen_gpu_entry_runtime.bc
  DEPENDS ${INPUTGEN_GPU_RT_BINARY_DIR}/inputgen_gpu_entry_callbacks.bc
          ${INPUTGEN_GPU_RT_BINARY_DIR}/inputgen_gpu_entry_state.bc
  COMMENT "Linking InputGen GPU entry runtime bitcode"
  VERBATIM)

add_custom_target(inputgen-gpu-runtime-bc
  DEPENDS
    ${INPUTGEN_GPU_RT_BINARY_DIR}/inputgen_gpu_entry_runtime.bc)

if(TARGET clang)
  add_dependencies(inputgen-gpu-runtime-bc clang)
endif()
if(TARGET llvm-link)
  add_dependencies(inputgen-gpu-runtime-bc llvm-link)
endif()
