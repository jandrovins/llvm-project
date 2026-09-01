# Components that must be visible to llvm/runtimes before compiler-rt's nested
# configure has generated Components.cmake.  The registration function is
# supplied by llvm/runtimes so this file remains declarative.
runtime_register_early_component(
  NAME inputgen-gpu
  PROJECT compiler-rt
  TARGET_PATTERN "^(default|amdgpu|amdgcn|nvptx)"
  DEPENDENTS offload check-offload)
