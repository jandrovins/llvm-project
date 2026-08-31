set(INPUTGEN_GPU_LIT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(INPUTGEN_GPU_RUNTIME_BC "")
set(INPUTGEN_GPU_RUNTIME_AVAILABLE FALSE)
set(INPUTGEN_GPU_HOST_TEST TRUE)

if(COMPILER_RT_GPU_BUILD)
  foreach(arch ${COMPILER_RT_SUPPORTED_ARCH})
    if("${arch}" MATCHES "^(amdgpu|amdgcn|nvptx64)$")
      get_compiler_rt_output_dir(${arch} INPUTGEN_GPU_OUTPUT_DIR)
      set_output_name(INPUTGEN_GPU_OUTPUT_NAME clang_rt.inputgen_gpu ${arch})
      set(INPUTGEN_GPU_RUNTIME_BC
          ${INPUTGEN_GPU_OUTPUT_DIR}/lib${INPUTGEN_GPU_OUTPUT_NAME}.bc)
      set(INPUTGEN_GPU_RUNTIME_AVAILABLE TRUE)
      break()
    endif()
  endforeach()
  set(INPUTGEN_GPU_HOST_TEST FALSE)
endif()

pythonize_bool(INPUTGEN_GPU_RUNTIME_AVAILABLE)
pythonize_bool(INPUTGEN_GPU_HOST_TEST)
configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py)

set(INPUTGEN_GPU_TEST_DEPS FileCheck)
if(TARGET inputgen-gpu)
  list(APPEND INPUTGEN_GPU_TEST_DEPS inputgen-gpu)
endif()
add_lit_testsuite(check-inputgen_gpu "Running the InputGen GPU runtime tests"
  ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS ${INPUTGEN_GPU_TEST_DEPS})
