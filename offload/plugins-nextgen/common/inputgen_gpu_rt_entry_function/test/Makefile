CC ?= clang
CXX ?= clang++
OPT ?= opt
LLC ?= llc
LLVM_DIS ?= llvm-dis
LLVM_LINK ?= llvm-link
LD_LLD ?= ld.lld

RUNTIME_DIR := ..
LLVM_SOURCE_ROOT := $(abspath $(RUNTIME_DIR)/../../../..)
LLVM_INSTALL_DIR ?=
BUILD_DIR ?= build

SRC := driver.cpp
DEVICE_SRC := test.c
GPU_BIN := $(BUILD_DIR)/test_gpu.bin

CFLAGS ?= -O0
CXXFLAGS ?= -O0 -g -std=c++17
CPPFLAGS ?= -I$(RUNTIME_DIR)
LDFLAGS ?=

ROCM_GPU ?= $(strip $(shell amdgpu-arch 2>/dev/null | head -n 1))
GPU_ARCH ?= $(ROCM_GPU)
TRIPLE ?= amdgcn-amd-amdhsa
LIBOMPTARGET_LIBDIR ?=
HOST_CPPFLAGS := $(CPPFLAGS) -I$(LLVM_SOURCE_ROOT)/offload/include \
	-I$(LLVM_SOURCE_ROOT)/llvm/include \
	$(if $(LLVM_INSTALL_DIR),-I$(LLVM_INSTALL_DIR)/include,) \
	-DINPUTGEN_GPU_ARCH=\"$(GPU_ARCH)\" \
	-DLLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING=1
HOST_LDFLAGS := $(if $(LIBOMPTARGET_LIBDIR),-L$(LIBOMPTARGET_LIBDIR),) \
	-lomptarget
HOST_OPENMP_FLAGS := -fopenmp

RUNTIME_BC := $(RUNTIME_DIR)/build/inputgen_gpu_rt.bc
DEVICE_BC := $(BUILD_DIR)/test.$(GPU_ARCH).bc
LINKED_BC := $(BUILD_DIR)/test.$(GPU_ARCH).linked.bc
DEVICE_LL := $(BUILD_DIR)/test.$(GPU_ARCH).ll
DEVICE_OBJ := $(BUILD_DIR)/test.$(GPU_ARCH).o
DEVICE_IMAGE := $(BUILD_DIR)/test.$(GPU_ARCH).image

.PHONY: all gpu image clean check-gpu-env

all: gpu

gpu: $(GPU_BIN)

image: $(DEVICE_IMAGE)

check-gpu-env:
	@if [ -z "$(GPU_ARCH)" ]; then \
		echo "GPU_ARCH is not set and no AMD GPU architecture was detected."; \
		echo "Run 'make gpu GPU_ARCH=<gfx...|sm_...>'."; \
		exit 1; \
	fi

$(BUILD_DIR):
	mkdir -p $@

$(DEVICE_BC): $(DEVICE_SRC) | $(BUILD_DIR) check-gpu-env
	$(CC) --target=$(TRIPLE) -mcpu=$(GPU_ARCH) -nogpulib -nogpuinc -nostdlibinc -emit-llvm -c $< -o $@

$(RUNTIME_BC):
	$(MAKE) -C $(RUNTIME_DIR) device-bc GPU_ARCH=$(GPU_ARCH) CC=$(CC) LLVM_LINK=$(LLVM_LINK)

$(LINKED_BC): $(DEVICE_BC) $(RUNTIME_BC)
	$(OPT) -passes=inputgen-gpu -inputgen-gpu-runtime-bitcode=$(RUNTIME_BC) -inputgen-gpu-entry-function=vvv_foo $< -o $@

$(DEVICE_LL): $(LINKED_BC)
	$(LLVM_DIS) $< -o $@

$(DEVICE_OBJ): $(LINKED_BC)
	$(LLC) -mtriple=$(TRIPLE) -mcpu=$(GPU_ARCH) -filetype=obj $< -o $@

$(DEVICE_IMAGE): $(DEVICE_OBJ)
	$(LD_LLD) -flavor gnu -shared $< -o $@

$(GPU_BIN): $(SRC) $(DEVICE_IMAGE) | $(BUILD_DIR) check-gpu-env
	$(CXX) $(HOST_CPPFLAGS) $(CXXFLAGS) $(HOST_OPENMP_FLAGS) $(SRC) -o $@ $(HOST_LDFLAGS) $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR) input.txt
