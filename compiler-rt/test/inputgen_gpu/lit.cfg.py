import os

import lit.formats

config.name = "InputGenGPU"
config.test_format = lit.formats.ShTest()
config.suffixes = [".c"]
config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.path.join(config.inputgen_gpu_lit_binary_dir, "Output")
config.substitutions.append(("%inputgen-gpu-src", config.inputgen_gpu_src))
config.substitutions.append(("%inputgen-gpu-runtime-bc", config.inputgen_gpu_runtime_bc))
config.substitutions.append(("%inputgen-gpu-cc", config.clang))

if config.inputgen_gpu_runtime_available:
    config.available_features.add("inputgen-gpu-runtime")
if config.inputgen_gpu_host_test:
    config.available_features.add("inputgen-gpu-host-test")
