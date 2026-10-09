#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum class UsbMscInitResult : uint8_t
{
	Ready,
	CoreInitFailed,
	ClassRegistrationFailed,
	DeviceStartFailed,
};

UsbMscInitResult UsbMscInit();
void UsbMscSetMedia(uint32_t block_count, bool ready);
void UsbMscProcess();
uint32_t UsbMscTraceCount();
bool UsbMscFormatTrace(uint32_t seq, char* line, size_t size);
bool UsbMscBusIdle();
bool UsbMscEjected();
void UsbMscStop();
