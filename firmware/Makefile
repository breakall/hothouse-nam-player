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
LDFLAGS += -T"$(abspath nam_a2_sections.lds)"
IR ?=
USE_IR ?= 0
NAM_ENGINE ?= legacy
ifeq ($(filter $(NAM_ENGINE),legacy tone3000),)
$(error NAM_ENGINE must be legacy or tone3000)
endif
ifeq ($(NAM_ENGINE),tone3000)
TONE3000_DIR = ../external/tone3000-embedded/engines/cortex-m7/a2lite
C_SOURCES += $(TONE3000_DIR)/nam_a2lite.c
LDFLAGS += -T"$(abspath nam_tone3000_sections.lds)"
endif

CPP_SOURCES = \
  hothouse_nam_host.cpp \
  nam_engine.cpp \
  hothouse.cpp

C_INCLUDES = \
  -I. \
  -I../external/DaisySeedProjects/Software/GuitarPedal/Effect-Modules/Nam
ifeq ($(NAM_ENGINE),tone3000)
C_INCLUDES += -I$(TONE3000_DIR)
endif

C_DEFS = \
  -DHOTHOUSE_DIAGNOSTIC=0 \
  -DHOTHOUSE_SIZE_BUILD=0 \
  -DHOTHOUSE_USE_IR=$(USE_IR) \
  -D__ARM_ARCH_7EM__
C_DEFS += -DHOTHOUSE_ENGINE_TONE3000=$(if $(filter tone3000,$(NAM_ENGINE)),1,0) \
  -DNAM_A2LITE_MAX_BUFFER_SIZE=48

LIBDAISY_DIR = ../HothouseExamples/libDaisy
DAISYSP_DIR = ../HothouseExamples/DaisySP

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

include ../HothouseExamples/src/Makefile

CPPFLAGS += -ffast-math -funroll-loops -ftree-vectorize -fmove-loop-invariants
ifeq ($(NAM_ENGINE),tone3000)
CFLAGS += -ffast-math
endif
