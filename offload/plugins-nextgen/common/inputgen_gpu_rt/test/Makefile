CC ?= clang
OPT ?= opt

RUNTIME_DIR := ..

SRC := test.c
GPU_BIN := test_gpu

CFLAGS ?= -O0 -g
CPPFLAGS ?= -I$(RUNTIME_DIR)
LDFLAGS ?=

ROCM_GPU ?= $(strip $(shell amdgpu-arch 2>/dev/null | head -n 1))
GPU_ARCH ?= $(ROCM_GPU)
OMPDEVICE_LIBDIR ?=
GPU_OPENMP_FLAGS := -fopenmp -fopenmp-target-jit --offload-arch=$(GPU_ARCH)
GPU_RUNTIME_LDFLAGS := $(if $(OMPDEVICE_LIBDIR),-L$(OMPDEVICE_LIBDIR),)

.PHONY: all gpu runtime clean check-gpu-env record-replay

all: gpu

gpu: $(GPU_BIN)

runtime:
	$(MAKE) -C $(RUNTIME_DIR) device-bc GPU_ARCH=$(GPU_ARCH)

check-gpu-env:
	@if [ -z "$(GPU_ARCH)" ]; then \
		echo "GPU_ARCH is not set and no AMD GPU architecture was detected."; \
		echo "Run 'make gpu GPU_ARCH=<gfx...|sm_...>'."; \
		exit 1; \
	fi

$(GPU_BIN): $(SRC) | check-gpu-env
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GPU_OPENMP_FLAGS) $< -o $@ $(GPU_RUNTIME_LDFLAGS) $(LDFLAGS)

record-replay: $(GPU_BIN) runtime
	./scripts/record_and_replay_openmp_kernel.sh ./$(GPU_BIN)

clean:
	rm -f $(GPU_BIN)
	rm -rf records build-instrumented
