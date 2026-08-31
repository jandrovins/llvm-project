# Build the device-only InputGen runtime as a normal compiler-rt component.
# The instrumentation pass consumes the linked bitcode module, while the
# archive keeps the sources available to regular compiler-rt build machinery.

if(NOT COMPILER_RT_GPU_BUILD)
  return()
endif()

add_compiler_rt_component(inputgen-gpu)

set(INPUTGEN_GPU_SOURCES
  inputgen_gpu_runtime_state.c
  inputgen_gpu_runtime_callbacks.c)
set(INPUTGEN_GPU_HEADERS
  inputgen_gpu_factory.h
  inputgen_gpu_instrumentor_abi.h
  inputgen_gpu_runtime_internal.h)
set(INPUTGEN_GPU_CFLAGS
  -ffreestanding
  -nogpulib
  -nostdlibinc
  -fconvergent-functions
  -fvisibility=protected
  -flto)

find_program(INPUTGEN_GPU_LLVM_LINK llvm-link
  PATHS ${LLVM_TOOLS_BINARY_DIR}
  NO_DEFAULT_PATH)
if(NOT INPUTGEN_GPU_LLVM_LINK)
  message(FATAL_ERROR
    "llvm-link not found (LLVM_TOOLS_BINARY_DIR='${LLVM_TOOLS_BINARY_DIR}')")
endif()

foreach(arch ${COMPILER_RT_SUPPORTED_ARCH})
  if(NOT "${arch}" MATCHES "^(amdgpu|amdgcn|nvptx64)$")
    continue()
  endif()

  add_compiler_rt_runtime(clang_rt.inputgen_gpu
    STATIC
    ARCHS ${arch}
    SOURCES ${INPUTGEN_GPU_SOURCES}
    CFLAGS ${INPUTGEN_GPU_CFLAGS}
    ADDITIONAL_HEADERS ${INPUTGEN_GPU_HEADERS}
    C_STANDARD 11
    PARENT_TARGET inputgen-gpu)

  get_compiler_rt_output_dir(${arch} INPUTGEN_GPU_OUTPUT_DIR)
  get_compiler_rt_install_dir(${arch} INPUTGEN_GPU_INSTALL_DIR)
  set_output_name(INPUTGEN_GPU_OUTPUT_NAME clang_rt.inputgen_gpu ${arch})
  set(INPUTGEN_GPU_BC_OUTPUT
      "${INPUTGEN_GPU_OUTPUT_DIR}/lib${INPUTGEN_GPU_OUTPUT_NAME}.bc")

  # Instrumentor parses a single IR module, so merge the archive's bitcode
  # members before publishing the pass-consumable runtime artifact.
  add_custom_command(
    OUTPUT "${INPUTGEN_GPU_BC_OUTPUT}"
    COMMAND "${INPUTGEN_GPU_LLVM_LINK}"
            "$<TARGET_FILE:clang_rt.inputgen_gpu-${arch}>"
            -o "${INPUTGEN_GPU_BC_OUTPUT}"
    DEPENDS clang_rt.inputgen_gpu-${arch}
    COMMENT "Linking lib${INPUTGEN_GPU_OUTPUT_NAME}.bc"
    VERBATIM)
  add_custom_target(clang_rt.inputgen_gpu-${arch}-bc ALL
    DEPENDS "${INPUTGEN_GPU_BC_OUTPUT}")
  add_dependencies(inputgen-gpu clang_rt.inputgen_gpu-${arch}-bc)
  install(FILES "${INPUTGEN_GPU_BC_OUTPUT}"
    DESTINATION "${INPUTGEN_GPU_INSTALL_DIR}"
    COMPONENT inputgen-gpu)
endforeach()
