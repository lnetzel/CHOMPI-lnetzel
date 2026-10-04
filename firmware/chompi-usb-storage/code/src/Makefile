# CHOMPI SD-card USB mass-storage firmware
TARGET = chompi_usb_storage_v1.5

DEBUG = 0
OPT = -O2

APP_TYPE = BOOT_SRAM
LDSCRIPT = ../../../chompi-wave/code/src/chompi_sram.lds

LIBDAISY_DIR = ../../../chompi-wave/code/libs/libDaisy
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core

CPP_SOURCES = main.cpp usb_msc.cpp
C_INCLUDES += -Iconfig
C_INCLUDES += -I../../../chompi-tape/code/src
USE_FATFS = 1

include $(SYSTEM_FILES_DIR)/Makefile

.PHONY: check-toolchain
check-toolchain:
	@command -v arm-none-eabi-gcc >/dev/null || { echo "arm-none-eabi-gcc is missing from PATH"; exit 1; }
	@command -v arm-none-eabi-g++ >/dev/null || { echo "arm-none-eabi-g++ is missing from PATH"; exit 1; }
	@command -v make >/dev/null || { echo "make is missing from PATH"; exit 1; }
	@arm-none-eabi-gcc --version | head -n 1
	@arm-none-eabi-gcc --version | grep -q '10\.3\.1' || { echo "This firmware requires Arm GCC 10.3.1"; exit 1; }
