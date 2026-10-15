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

# SD-card presets: FatFS + coreJSON (shared from chompi-wave's libs)
USE_FATFS = 1
C_SOURCES = ../../../chompi-wave/code/libs/coreJSON/source/core_json.c
C_INCLUDES += -I../../../chompi-wave/code/libs/coreJSON/source/include

# Libs: reuse chompi-wave's libDaisy/DaisySP (same toolchain, prebuilt)
LIBDAISY_DIR = ../../../chompi-wave/code/libs/libDaisy
DAISYSP_DIR = ../../../chompi-wave/code/libs/DaisySP

# Core location, and generic Makefile
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

.PHONY: check-toolchain
check-toolchain:
	@command -v arm-none-eabi-gcc >/dev/null || { echo "arm-none-eabi-gcc is missing from PATH"; exit 1; }
	@command -v arm-none-eabi-g++ >/dev/null || { echo "arm-none-eabi-g++ is missing from PATH"; exit 1; }
	@command -v make >/dev/null || { echo "make is missing from PATH"; exit 1; }
	@arm-none-eabi-gcc --version | head -n 1
	@arm-none-eabi-gcc --version | grep -q '10\.3\.1' || { echo "This firmware requires Arm GCC 10.3.1"; exit 1; }
