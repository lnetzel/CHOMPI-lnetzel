# CHOMPI Organ Emulator
# See firmware/chompi-organ/chompi-organ- specification.md

TARGET = CHOMPI_ORGAN_1.0

DEBUG=0
OPT=-O3

APP_TYPE=BOOT_SRAM
# `make program-boot` installs CHOMPI's own bootloader, shared with chompi-wave
BOOT_BIN = ../../../chompi-wave/bin/CHOMPI_Bootloader_V6_2_0.bin
LDSCRIPT = chompi_sram.lds

# Sources
CPP_SOURCES = chompi_main.cpp \
	encoder.cpp

# Libs: reuse chompi-wave's libDaisy/DaisySP (same toolchain, prebuilt)
LIBDAISY_DIR = ../../../chompi-wave/code/libs/libDaisy
DAISYSP_DIR = ../../../chompi-wave/code/libs/DaisySP

# Core location, and generic Makefile
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
