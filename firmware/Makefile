# Project Name
TARGET = hothouse_nam

APP_TYPE = BOOT_QSPI
OPT = -O3
CPP_STANDARD = -std=gnu++17
GCC_PATH ?= ../external/arm-toolchain-pkg/Payload/bin
LDFLAGS = -u _printf_float
MODEL ?=
IR ?=
USE_IR ?= 0
EMBED_SCRIPT = ../tools/embed_model.py
IR_EMBED_SCRIPT = ../tools/embed_ir.py

CPP_SOURCES = \
  hothouse_nam.cpp \
  ../hothouse.cpp \
  ../nam-pedal/nam-binary-loader/namb/get_dsp_namb.cpp \
  ../nam-pedal/NeuralAmpModelerCore/NAM/activations.cpp \
  ../nam-pedal/NeuralAmpModelerCore/NAM/conv1d.cpp \
  ../nam-pedal/NeuralAmpModelerCore/NAM/convnet.cpp \
  ../nam-pedal/NeuralAmpModelerCore/NAM/dsp.cpp \
  ../nam-pedal/NeuralAmpModelerCore/NAM/lstm.cpp \
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

embed-ir:
	python3 $(IR_EMBED_SCRIPT) "$(IR)" -o embedded_ir.h
