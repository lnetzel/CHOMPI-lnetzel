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

// Mounts the card briefly to set the CHOMPI-SD volume label, then unmounts
// again so the computer can take over the card.
bool SetVolumeLabel(bool sd_init_ok, bool fatfs_link_ok)
{
    if(!sd_init_ok || !fatfs_link_ok)
        return false;

    if(f_mount(&fatfs.GetSDFileSystem(), fatfs.GetSDPath(), 1) != FR_OK)
        return false;

    const FRESULT chdrive_result = f_chdrive(fatfs.GetSDPath());
    const FRESULT label_result   = f_setlabel("CHOMPI-SD");
    f_mount(nullptr, fatfs.GetSDPath(), 0);
    return chdrive_result == FR_OK && label_result == FR_OK;
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
    bool sector_read_ok = false;
    if(card_ready)
    {
        BSP_SD_CardInfo card_info;
        BSP_SD_GetCardInfo(&card_info);
        block_count = card_info.LogBlockNbr;
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

    const bool label_ok = SetVolumeLabel(sd_init_ok, fatfs_link_ok);

    if(!card_ready || !label_ok)
        SetStatusLeds(255, 0, 0);
    else
        SetStatusLeds(0, 80, 255);

    UsbMscSetMedia(block_count, card_ready && block_count > 0);
    const UsbMscInitResult usb_result = UsbMscInit();
    const bool usb_ready = usb_result == UsbMscInitResult::Ready;
    if(card_ready && label_ok && block_count > 0 && usb_ready)
        SetStatusLeds(0, 255, 0);
    else if(card_ready && label_ok && block_count > 0)
        SetStatusLeds(0, 0, 255);

    while(1)
    {
        UsbMscProcess();
        System::Delay(1);
    }
}
