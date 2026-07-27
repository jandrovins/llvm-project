CC ?= clang
ifeq ($(origin AR),default)
AR := llvm-ar
endif
LLVM_LINK ?= llvm-link

BUILD_DIR ?= build
PREFIX ?= /usr/local
LLVM_INPUTGEN_INCLUDE ?= ../include

DEVICE_SRCS := inputgen_gpu_rt_device.c inputgen_gpu_rt_buffer.c
HDRS := inputgen_gpu_rt.h inputgen_gpu_rt_internal.h $(LLVM_INPUTGEN_INCLUDE)/InputGenInterface.hpp

DEVICE_BCS := $(patsubst %.c,$(BUILD_DIR)/%.bc,$(DEVICE_SRCS))
DEVICE_BITCODE := $(BUILD_DIR)/inputgen_gpu_rt.bc

CFLAGS ?= -O3 -g
CPPFLAGS ?=
WARNFLAGS ?= -Wall -Wextra

ROCM_GPU ?= $(strip $(shell amdgpu-arch 2>/dev/null | head -n 1))
GPU_ARCH ?= $(if $(ROCM_GPU),$(ROCM_GPU),native)
DEVICE_TRIPLE ?= amdgcn-amd-amdhsa
DEVICE_ARCH_FLAGS ?= -march=$(GPU_ARCH)

DEVICE_CPPFLAGS := $(CPPFLAGS) -DINPUTGEN_GPU_RT_DEVICE=1 -I. -I$(LLVM_INPUTGEN_INCLUDE)
DEVICE_COMPILE_FLAGS := $(CFLAGS) $(WARNFLAGS) -std=c11 \
	--target=$(DEVICE_TRIPLE) $(DEVICE_ARCH_FLAGS) \
	-nogpulib -nostdlibinc -fconvergent-functions -fvisibility=protected -flto
DEVICE_BITCODE_FLAGS := $(DEVICE_COMPILE_FLAGS) -c -emit-llvm

.PHONY: all clean check-gpu-env install device-bc

all: device-bc

device-bc: $(DEVICE_BCS) $(DEVICE_BITCODE)

$(BUILD_DIR):
	mkdir -p $@

check-gpu-env:
	@if [ -z "$(GPU_ARCH)" ]; then \
		echo "GPU_ARCH is not set and no AMD GPU architecture was detected."; \
		echo "Run 'make GPU_ARCH=<gfx...|sm_...>'."; \
		exit 1; \
	fi

$(DEVICE_BCS): $(BUILD_DIR)/%.bc: %.c $(HDRS) | $(BUILD_DIR) check-gpu-env
	CCACHE_DISABLE=1 $(CC) $(DEVICE_CPPFLAGS) $(DEVICE_BITCODE_FLAGS) $< -o $@

$(DEVICE_BITCODE): $(DEVICE_BCS) | $(BUILD_DIR)
	$(LLVM_LINK) $^ -o $@

install: $(DEVICE_BITCODE)
	install -d $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include
	install -m 0644 $(DEVICE_BITCODE) $(DESTDIR)$(PREFIX)/lib/
	install -m 0644 inputgen_gpu_rt.h $(DESTDIR)$(PREFIX)/include/

clean:
	rm -rf $(BUILD_DIR)
