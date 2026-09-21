MODEL ?=
IR ?=
IR_A ?= $(IR)
IR_B ?=
USE_IR ?= 0
A2_DIAGNOSTIC ?= 0
CAPTURE ?=

A2_RUNTIME = external/DaisySeedProjects/Software/GuitarPedal/Effect-Modules/Nam/nam_a2_runtime.h

.PHONY: a1 a2 clean-a1 clean-a2 setup-a2 require-model require-ir embed-ir-bank install-capture

require-model:
	@test -n "$(MODEL)" || { echo "MODEL is required" >&2; exit 2; }
	@test -f "$(MODEL)" || { echo "Model not found: $(MODEL)" >&2; exit 2; }

require-ir:
	@test "$(USE_IR)" = "0" || test -n "$(IR_A)" || { echo "IR_A (or legacy IR) is required when USE_IR=1" >&2; exit 2; }
	@test "$(USE_IR)" = "0" || test -f "$(IR_A)" || { echo "IR not found: $(IR_A)" >&2; exit 2; }
	@test -z "$(IR_B)" || test -f "$(IR_B)" || { echo "IR not found: $(IR_B)" >&2; exit 2; }

embed-ir-bank: require-ir
	@test "$(USE_IR)" = "0" || python3 tools/embed_ir_bank.py "$(IR_A)" $(if $(IR_B),"$(IR_B)") -o firmware/embedded_ir_bank.h

a1: require-model require-ir embed-ir-bank
	python3 tools/embed_model.py "$(MODEL)" -o firmware/embedded_model.h
	$(MAKE) -C firmware BUILD_DIR=build/a1 clean
	$(MAKE) -C firmware BUILD_DIR=build/a1 USE_IR=$(USE_IR)

setup-a2:
	./tools/setup_a2_dependencies.sh

a2: require-model require-ir embed-ir-bank
	@test -f "$(A2_RUNTIME)" || { echo "A2 runtime missing; run: make setup-a2" >&2; exit 2; }
	python3 tools/embed_a2_model.py "$(MODEL)" -o firmware/embedded_a2_model.h
	$(MAKE) -C firmware -f Makefile.a2 BUILD_DIR=build/a2 clean
	$(MAKE) -C firmware -f Makefile.a2 BUILD_DIR=build/a2 DIAGNOSTIC=$(A2_DIAGNOSTIC) USE_IR=$(USE_IR)

clean-a1:
	$(MAKE) -C firmware BUILD_DIR=build/a1 clean

clean-a2:
	$(MAKE) -C firmware -f Makefile.a2 BUILD_DIR=build/a2 clean

install-capture:
	@test -n "$(CAPTURE)" || { echo "CAPTURE is required" >&2; exit 2; }
	python3 tools/install_capture.py "$(CAPTURE)"
