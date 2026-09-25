# Project Name
TARGET = hothouse_nam

APP_TYPE = BOOT_SRAM
USE_DAISYSP_LGPL = 1
OPT = -O3
CPP_STANDARD = -std=gnu++17
ifneq ($(wildcard ../external/arm-toolchain-pkg/Payload/bin/arm-none-eabi-gcc),)
GCC_PATH ?= ../external/arm-toolchain-pkg/Payload/bin
else
ARM_GNU_TOOLCHAIN_BIN := $(patsubst %/,%,$(lastword $(sort $(dir $(wildcard /Applications/ArmGNUToolchain/*/arm-none-eabi/bin/arm-none-eabi-gcc)))))
ifneq ($(ARM_GNU_TOOLCHAIN_BIN),)
GCC_PATH ?= $(ARM_GNU_TOOLCHAIN_BIN)
endif
endif
LDFLAGS = -u _printf_float
MODEL ?=
IR ?=
USE_IR ?= 0
EMBED_SCRIPT = ../tools/embed_model.py

CPP_SOURCES = \
  hothouse_nam_host.cpp \
  nam_engine_a1.cpp \
  ../hothouse.cpp \
  ../nam-pedal/nam-binary-loader/namb/get_dsp_namb.cpp \
  ../nam-pedal/NeuralAmpModelerCore/NAM/activations.cpp \
  ../nam-pedal/NeuralAmpModelerCore/NAM/conv1d.cpp \
  ../nam-pedal/NeuralAmpModelerCore/NAM/dsp.cpp \
  ../nam-pedal/NeuralAmpModelerCore/NAM/ring_buffer.cpp \
  ../nam-pedal/NeuralAmpModelerCore/NAM/util.cpp \
  ../nam-pedal/NeuralAmpModelerCore/NAM/wavenet.cpp

C_INCLUDES = \
  -I. \
  -I../nam-pedal/NeuralAmpModelerCore \
  -I../nam-pedal/NeuralAmpModelerCore/Dependencies/eigen \
  -I../nam-pedal/NeuralAmpModelerCore/Dependencies/nlohmann \
  -I../nam-pedal/nam-binary-loader

C_DEFS = \
  -DNAM_SAMPLE_FLOAT \
  -DNAM_USE_INLINE_GEMM \
  -DNAM_DISABLE_JSON_REGISTRY \
  -DNAM_NAMB_WAVENET_ONLY \
  -DHOTHOUSE_DIAGNOSTIC=0 \
  -DHOTHOUSE_SIZE_BUILD=0 \
  -DHOTHOUSE_USE_IR=$(USE_IR) \
  -D__ARM_ARCH_7EM__

LIBDAISY_DIR = ../HothouseExamples/libDaisy
DAISYSP_DIR = ../HothouseExamples/DaisySP

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

include ../HothouseExamples/src/Makefile

CPPFLAGS += -fexceptions -ffast-math -funroll-loops -ftree-vectorize -fmove-loop-invariants

embed-model:
	python3 $(EMBED_SCRIPT) "$(MODEL)" -o embedded_model.h
