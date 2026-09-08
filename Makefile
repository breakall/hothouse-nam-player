MODEL ?=
IR ?=
USE_IR ?= 0

A2_RUNTIME = external/DaisySeedProjects/Software/GuitarPedal/Effect-Modules/Nam/nam_a2_runtime.h

.PHONY: a1 a2 clean-a1 clean-a2 setup-a2 require-model

require-model:
	@test -n "$(MODEL)" || { echo "MODEL is required" >&2; exit 2; }
	@test -f "$(MODEL)" || { echo "Model not found: $(MODEL)" >&2; exit 2; }

a1: require-model
	python3 tools/embed_model.py "$(MODEL)" -o firmware/embedded_model.h
	$(MAKE) -C firmware BUILD_DIR=build/a1 clean
	$(MAKE) -C firmware BUILD_DIR=build/a1 USE_IR=$(USE_IR)

setup-a2:
	./tools/setup_a2_dependencies.sh

a2: require-model
	@test -f "$(A2_RUNTIME)" || { echo "A2 runtime missing; run: make setup-a2" >&2; exit 2; }
	python3 tools/embed_a2_model.py "$(MODEL)" -o firmware/embedded_a2_model.h
	$(MAKE) -C firmware -f Makefile.a2 BUILD_DIR=build/a2 clean
	$(MAKE) -C firmware -f Makefile.a2 BUILD_DIR=build/a2

clean-a1:
	$(MAKE) -C firmware BUILD_DIR=build/a1 clean

clean-a2:
	$(MAKE) -C firmware -f Makefile.a2 BUILD_DIR=build/a2 clean
