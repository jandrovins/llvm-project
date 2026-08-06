CC ?= clang
OPT ?= opt
LLC ?= llc
LLVM_DIS ?= llvm-dis
LLVM_LINK ?= llvm-link
LD_LLD ?= ld.lld

RUNTIME_DIR := ..
BUILD_DIR ?= build

DEVICE_SRC := test.c
ENTRY_FUNCTION ?= vvv_foo
DEVICE_ID ?= 0

CFLAGS ?= -O0

ROCM_GPU ?= $(strip $(shell amdgpu-arch 2>/dev/null | head -n 1))
GPU_ARCH ?= $(ROCM_GPU)
TRIPLE ?= amdgcn-amd-amdhsa

RUNTIME_BC := $(RUNTIME_DIR)/build/inputgen_gpu_rt.bc
DEVICE_BC := $(BUILD_DIR)/test.$(GPU_ARCH).bc
LINKED_BC := $(BUILD_DIR)/test.$(GPU_ARCH).linked.bc
DEVICE_LL := $(BUILD_DIR)/test.$(GPU_ARCH).ll
DEVICE_OBJ := $(BUILD_DIR)/test.$(GPU_ARCH).o
DEVICE_IMAGE := $(BUILD_DIR)/test.$(GPU_ARCH).image
RECORD_JSON := $(BUILD_DIR)/$(ENTRY_FUNCTION).json

.PHONY: all gpu image clean check-gpu-env

all: image

gpu: image

image: $(DEVICE_IMAGE) $(RECORD_JSON)

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
	$(OPT) -passes=inputgen-gpu -inputgen-gpu-runtime-bitcode=$(RUNTIME_BC) -inputgen-gpu-entry-function=$(ENTRY_FUNCTION) $< -o $@

$(DEVICE_LL): $(LINKED_BC)
	$(LLVM_DIS) $< -o $@

$(DEVICE_OBJ): $(LINKED_BC)
	$(LLC) -mtriple=$(TRIPLE) -mcpu=$(GPU_ARCH) -filetype=obj $< -o $@

$(DEVICE_IMAGE): $(DEVICE_OBJ)
	$(LD_LLD) -flavor gnu -shared $< -o $@

$(RECORD_JSON): | $(BUILD_DIR)
	printf '{\n  "Name": "$(ENTRY_FUNCTION)",\n  "DeviceId": $(DEVICE_ID)\n}\n' > $@

clean:
	rm -rf $(BUILD_DIR) input.txt
