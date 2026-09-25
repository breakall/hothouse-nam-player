IR ?=
IR_A ?= $(IR)
IR_B ?=
USE_IR ?= 0
CAPTURE ?=
SLOT ?= A
HOST_CXX ?= c++
HOST_TEST_BUILD ?= firmware/build/host-tests

# The macOS ARM GNU Toolchain installer keeps versioned toolchains here.  Use
# the newest installed copy automatically; callers may still override
# GCC_PATH explicitly for another toolchain.
ARM_GNU_TOOLCHAIN_BIN ?= $(patsubst %/,%,$(lastword $(sort $(dir $(wildcard /Applications/ArmGNUToolchain/*/arm-none-eabi/bin/arm-none-eabi-gcc)))))
ifneq ($(ARM_GNU_TOOLCHAIN_BIN),)
GCC_PATH ?= $(ARM_GNU_TOOLCHAIN_BIN)
endif
export GCC_PATH

A2_RUNTIME = external/DaisySeedProjects/Software/GuitarPedal/Effect-Modules/Nam/nam_a2_runtime.h

.PHONY: firmware a1 a2 clean-firmware clean-a1 clean-a2 setup-a2 require-ir embed-ir-bank install-capture list-captures test

test: $(HOST_TEST_BUILD)/capture_loader_test $(HOST_TEST_BUILD)/capture_transition_test $(HOST_TEST_BUILD)/reverb_engines_test
	$(HOST_TEST_BUILD)/capture_loader_test
	$(HOST_TEST_BUILD)/capture_transition_test
	$(HOST_TEST_BUILD)/reverb_engines_test
	python3 -m unittest discover -s tools/tests -p 'test_*.py'

$(HOST_TEST_BUILD):
	mkdir -p $@

$(HOST_TEST_BUILD)/capture_loader_test: firmware/tests/capture_loader_test.cpp | $(HOST_TEST_BUILD)
	$(HOST_CXX) -std=c++17 -O2 -Wall -Wextra -Werror $< -o $@

$(HOST_TEST_BUILD)/capture_transition_test: firmware/tests/capture_transition_test.cpp firmware/capture_transition.h | $(HOST_TEST_BUILD)
	$(HOST_CXX) -std=c++17 -O2 -Wall -Wextra -Werror $< -o $@

$(HOST_TEST_BUILD)/reverb_engines_test: firmware/tests/reverb_engines_test.cpp | $(HOST_TEST_BUILD)
	$(HOST_CXX) -std=c++17 -O2 -Wall -Wextra -Werror $< -o $@

require-ir:
	@test "$(USE_IR)" = "0" || test -n "$(IR_A)" || { echo "IR_A (or legacy IR) is required when USE_IR=1" >&2; exit 2; }
	@test "$(USE_IR)" = "0" || test -f "$(IR_A)" || { echo "IR not found: $(IR_A)" >&2; exit 2; }
	@test -z "$(IR_B)" || test -f "$(IR_B)" || { echo "IR not found: $(IR_B)" >&2; exit 2; }

embed-ir-bank: require-ir
	@test "$(USE_IR)" = "0" || python3 tools/embed_ir_bank.py "$(IR_A)" $(if $(IR_B),"$(IR_B)") -o firmware/embedded_ir_bank.h

firmware: require-ir embed-ir-bank
	@test -f "$(A2_RUNTIME)" || { echo "A2 runtime missing; run: make setup-a2" >&2; exit 2; }
	mkdir -p firmware/build
	$(MAKE) -C firmware BUILD_DIR=build/combined clean
	$(MAKE) -C firmware BUILD_DIR=build/combined USE_IR=$(USE_IR)

setup-a2:
	./tools/setup_a2_dependencies.sh

a1 a2: firmware
	@echo "A1 and A2 now use the same combined firmware image."

clean-firmware:
	$(MAKE) -C firmware BUILD_DIR=build/combined clean

clean-a1 clean-a2: clean-firmware

install-capture:
	@test -n "$(CAPTURE)" || { echo "CAPTURE is required" >&2; exit 2; }
	python3 tools/install_capture.py "$(CAPTURE)" --slot "$(SLOT)"

list-captures:
	python3 tools/install_capture.py --list
