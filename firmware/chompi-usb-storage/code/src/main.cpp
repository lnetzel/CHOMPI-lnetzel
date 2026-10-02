#include <algorithm>
#include <cstdio>
#include <cstring>

#include "daisy_seed.h"
#include "per/sdmmc.h"
#include "sys/fatfs.h"
#include "util/bsp_sd_diskio.h"
#include "ff.h"
#include "diskio.h"
#include "temp_led_stuff.h"
#include "usb_msc.h"

using namespace daisy;

namespace
{
DaisySeed board;
GPIO usb_switch;
SdmmcHandler sdmmc;
FatFSInterface fatfs;
alignas(32) uint8_t card_probe[512];
alignas(32) FIL log_file;
char boot_log[512];
size_t boot_log_size = 0;

void SetStatusLeds(uint8_t red, uint8_t green, uint8_t blue)
{
    for(size_t index = 0; index < chompi::kNumPthLeds - 2 * chompi::kPorchSize;
        ++index)
        chompi::SetPthLed(index, red, green, blue);
    for(size_t index = 0; index < chompi::kNumSmtLeds - 2 * chompi::kPorchSize;
        ++index)
        chompi::SetSmtLed(index, red, green, blue);
    chompi::fill_led_data();
}

void AppendLogLine(const char* line)
{
    const size_t line_size = std::strlen(line);
    if(boot_log_size + line_size + 2 > sizeof(boot_log))
        return;
    std::memcpy(boot_log + boot_log_size, line, line_size);
    boot_log_size += line_size;
    boot_log[boot_log_size++] = '\r';
    boot_log[boot_log_size++] = '\n';
}

void AppendLogResult(const char* name, FRESULT result)
{
    char line[64];
    std::snprintf(line, sizeof(line), "%s=%u", name,
                  static_cast<unsigned>(result));
    AppendLogLine(line);
}

void AppendLogValue(const char* name, uint32_t value)
{
    char line[64];
    std::snprintf(line, sizeof(line), "%s=%lu", name,
                  static_cast<unsigned long>(value));
    AppendLogLine(line);
}

bool PersistBootLog(bool sd_init_ok, bool fatfs_link_ok)
{
    const FRESULT mount_result = sd_init_ok && fatfs_link_ok
                                     ? f_mount(&fatfs.GetSDFileSystem(),
                                               fatfs.GetSDPath(), 1)
                                     : FR_NOT_READY;
    AppendLogResult("FAT_MOUNT", mount_result);
    if(mount_result != FR_OK)
        return false;

    const FRESULT chdrive_result = f_chdrive(fatfs.GetSDPath());
    AppendLogResult("FAT_CHDRIVE", chdrive_result);
    const FRESULT label_result = f_setlabel("CHOMPI-SD");
    AppendLogResult("FAT_SETLABEL", label_result);

    char log_path[32];
    std::snprintf(log_path, sizeof(log_path), "%s/CHOMPI_USB.LOG",
                  fatfs.GetSDPath());
    const FRESULT open_result = f_open(
        &log_file, log_path, FA_OPEN_APPEND | FA_WRITE);
    AppendLogResult("LOG_OPEN", open_result);
    if(open_result != FR_OK)
    {
        f_mount(nullptr, fatfs.GetSDPath(), 0);
        return false;
    }

    SetStatusLeds(255, 0, 255);
    UINT written = 0;
    const FRESULT write_result
        = f_write(&log_file, boot_log, boot_log_size, &written);
    if(write_result != FR_OK || written != boot_log_size)
    {
        f_mount(nullptr, fatfs.GetSDPath(), 0);
        return false;
    }

    SetStatusLeds(255, 255, 0);
    const FRESULT sync_result = f_sync(&log_file);
    if(sync_result != FR_OK)
    {
        f_mount(nullptr, fatfs.GetSDPath(), 0);
        return false;
    }

    const FRESULT close_result = f_close(&log_file);
    f_mount(nullptr, fatfs.GetSDPath(), 0);
    return chdrive_result == FR_OK && label_result == FR_OK
           && close_result == FR_OK;
}

void AppendFailureToLog(const char* message)
{
    if(f_mount(&fatfs.GetSDFileSystem(), fatfs.GetSDPath(), 1) != FR_OK)
        return;

    char log_path[32];
    std::snprintf(log_path, sizeof(log_path), "%s/CHOMPI_USB.LOG",
                  fatfs.GetSDPath());
    if(f_chdrive(fatfs.GetSDPath()) == FR_OK
       && f_open(&log_file, log_path, FA_OPEN_APPEND | FA_WRITE) == FR_OK)
    {
        UINT written = 0;
        f_write(&log_file, message, std::strlen(message), &written);
        f_write(&log_file, "\r\n", 2, &written);
        f_sync(&log_file);
        f_close(&log_file);
    }
    f_mount(nullptr, fatfs.GetSDPath(), 0);
}
}

int main()
{
    board.Init(true);
    chompi::SetLedBrightness(10);
    chompi::LedSetup();
    SetStatusLeds(255, 80, 0);

    usb_switch.Init(daisy::seed::D32, GPIO::Mode::OUTPUT, GPIO::Pull::NOPULL);
    usb_switch.Write(true);
    System::Delay(10);

    SdmmcHandler::Config sd_config;
    sd_config.Defaults();
    sd_config.speed = SdmmcHandler::Speed::FAST;
    sd_config.width = SdmmcHandler::BusWidth::BITS_4;
    sdmmc.Init(sd_config);

    const bool fatfs_link_ok
        = fatfs.Init(FatFSInterface::Config::MEDIA_SD) == FatFSInterface::OK;
    const bool sd_init_ok = BSP_SD_Init() == MSD_OK;
    bool card_ready = sd_init_ok;
    uint32_t block_count = 0;
    uint32_t block_size = 0;
    bool card_info_valid = false;
    bool sector_read_ok = false;
    if(card_ready)
    {
        BSP_SD_CardInfo card_info;
        BSP_SD_GetCardInfo(&card_info);
        block_count = card_info.LogBlockNbr;
        block_size = card_info.LogBlockSize;
        card_info_valid = true;
        if(card_info.LogBlockSize == 512 && card_info.LogBlockNbr > 0)
        {
            sector_read_ok = BSP_SD_ReadBlocks(
                                 reinterpret_cast<uint32_t*>(card_probe), 0, 1,
                                 SD_DATATIMEOUT)
                             == MSD_OK;
            card_ready = sector_read_ok;
        }
        else
            card_ready = false;
    }

    AppendLogLine("=== CHOMPI USB STORAGE BOOT ===");
    AppendLogLine("SDMMC_CONFIG=4BIT_FAST");
    AppendLogLine(fatfs_link_ok ? "FAT_DRIVER_LINK=OK" : "FAT_DRIVER_LINK=FAIL");
    AppendLogLine(sd_init_ok ? "SD_INIT=OK" : "SD_INIT=FAIL");
    if(card_info_valid)
    {
        AppendLogValue("CARD_BLOCKS", block_count);
        AppendLogValue("CARD_BLOCK_SIZE", block_size);
    }
    AppendLogLine(sector_read_ok ? "SECTOR0_READ=OK" : "SECTOR0_READ=FAIL");
    AppendLogLine("USB_MSC_INIT=STARTING");
    const bool log_ready = PersistBootLog(sd_init_ok, fatfs_link_ok);

    if(!card_ready || !log_ready)
        SetStatusLeds(255, 0, 0);
    else
        SetStatusLeds(0, 80, 255);

    UsbMscSetMedia(block_count, card_ready && block_count > 0);
    const UsbMscInitResult usb_result = UsbMscInit();
    const bool usb_ready = usb_result == UsbMscInitResult::Ready;
    if(card_ready && log_ready && block_count > 0 && usb_ready)
        SetStatusLeds(0, 255, 0);
    else if(card_ready && log_ready && block_count > 0)
        SetStatusLeds(0, 0, 255);
    if(!usb_ready && log_ready)
    {
        switch(usb_result)
        {
            case UsbMscInitResult::CoreInitFailed:
                AppendFailureToLog("USB_MSC_INIT=CORE_INIT_FAILED");
                break;
            case UsbMscInitResult::ClassRegistrationFailed:
                AppendFailureToLog("USB_MSC_INIT=CLASS_REGISTRATION_FAILED");
                break;
            case UsbMscInitResult::DeviceStartFailed:
                AppendFailureToLog("USB_MSC_INIT=DEVICE_START_FAILED");
                break;
            case UsbMscInitResult::Ready: break;
        }
    }

    while(1)
    {
        UsbMscProcess();
        System::Delay(1);
    }
}
