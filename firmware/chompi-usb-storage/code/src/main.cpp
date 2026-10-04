#include <algorithm>
#include <cstdio>
#include <cstring>

#include "daisy_seed.h"
#include "dev/sr_4021.h"
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
ShiftRegister4021<5, 1> button_sr;
alignas(32) uint8_t card_probe[512];

/** Bit positions of the big transport keys on the CD4021 chain, from the
 *  SwId map in chompi-tape's hardware.h. */
constexpr int kChompiKeySr  = 5;  /**< KEY_26 */
constexpr int kOverdubKeySr = 34; /**< KEY_28 */

/** PTH LED under each key, from chompi-tape's led_map. */
constexpr size_t kChompiKeyLed  = 0;
constexpr size_t kOverdubKeyLed = 8;

constexpr uint32_t kBlinkPeriodMs = 300; /**< overdub "are you sure?" blink */
constexpr uint32_t kFadeMs        = 400; /**< green -> white fade on shutdown */

/** Restart state machine: the overdub key arms it, the chompi key confirms. */
enum class ResetState : uint8_t
{
    Idle,       /**< normal operation, overdub key solid red */
    Confirming, /**< overdub blinking, chompi red = "yes" */
    ShuttingDown,
};

/** Colour all LEDs were last set to, so the chompi key can return to it
 *  after a cancelled restart. */
uint8_t status_r = 0;
uint8_t status_g = 0;
uint8_t status_b = 0;

void SetStatusLeds(uint8_t red, uint8_t green, uint8_t blue)
{
    status_r = red;
    status_g = green;
    status_b = blue;
    for(size_t index = 0; index < chompi::kNumPthLeds - 2 * chompi::kPorchSize;
        ++index)
        chompi::SetPthLed(index, red, green, blue);
    for(size_t index = 0; index < chompi::kNumSmtLeds - 2 * chompi::kPorchSize;
        ++index)
        chompi::SetSmtLed(index, red, green, blue);
    chompi::fill_led_data();
}

/** Paints the two big key LEDs for the current reset state: the overdub key
 *  is solid red when idle and blinks red while asking "are you sure?";
 *  the chompi key turns red to offer "yes". */
void PaintKeyLeds(ResetState state, bool blink_on)
{
    if(state == ResetState::Confirming)
    {
        chompi::SetPthLed(kOverdubKeyLed, blink_on ? 255 : 0, 0, 0);
        chompi::SetPthLed(kChompiKeyLed, 255, 0, 0);
    }
    else
    {
        chompi::SetPthLed(kOverdubKeyLed, 255, 0, 0);
        chompi::SetPthLed(kChompiKeyLed, status_r, status_g, status_b);
    }
    chompi::fill_led_data();
}

/** Confirmed restart: drops the USB link so the host unmounts the drive,
 *  fades the green LEDs to white, then resets the MCU back into the
 *  bootloader. Never returns. */
void ShutdownAndReset(bool usb_started)
{
    /* Host-side unmount: stop the USB device and open the USB switch, so the
     * computer sees the drive disappear as if the cable was unplugged. */
    if(usb_started)
        UsbMscStop();
    usb_switch.Write(false);

    /* Everything green fades to white; the two red key LEDs just go out. */
    const uint32_t start = System::GetNow();
    for(uint32_t elapsed = 0; elapsed < kFadeMs;
        elapsed          = System::GetNow() - start)
    {
        const uint8_t rb = static_cast<uint8_t>(elapsed * 255 / kFadeMs);
        for(size_t index = 0;
            index < chompi::kNumPthLeds - 2 * chompi::kPorchSize;
            ++index)
        {
            if(index == kChompiKeyLed || index == kOverdubKeyLed)
                chompi::SetPthLed(index, 0, 0, 0);
            else
                chompi::SetPthLed(index, rb, 255, rb);
        }
        for(size_t index = 0;
            index < chompi::kNumSmtLeds - 2 * chompi::kPorchSize;
            ++index)
            chompi::SetSmtLed(index, rb, 255, rb);
        chompi::fill_led_data();
        System::Delay(5);
    }
    SetStatusLeds(255, 255, 255);
    System::Delay(120);
    SetStatusLeds(0, 0, 0);
    System::Delay(80);

    NVIC_SystemReset();
    while(1)
    {}
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

    /** Transport keys on the CD4021 chain (same pins as the instrument
     *  firmwares): the overdub key doubles as the restart trigger. */
    ShiftRegister4021<5, 1>::Config button_sr_cfg;
    button_sr_cfg.clk      = seed::D8;
    button_sr_cfg.latch    = seed::D7;
    button_sr_cfg.data[0]  = seed::D9;
    button_sr_cfg.dbc_size = 7;
    button_sr.Init(button_sr_cfg);

    /** Flush junk out of the shift registers and let the debouncer settle,
     *  like the tape firmware does at boot. */
    for(int i = 0; i < 40; ++i)
    {
        button_sr.Update();
        System::Delay(1);
    }

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

    /** Whatever the status colour, the overdub key is the red restart
     *  trigger. */
    PaintKeyLeds(ResetState::Idle, false);

    // No delay here: UsbMscProcess() moves one 512-byte sector per call, so
    // sleeping 1 ms each pass capped transfers at ~1000 sectors/s. macOS
    // reads the whole FAT before mounting, and on a card formatted with
    // small clusters that took longer than its 20 s timeout.
    ResetState reset_state = ResetState::Idle;
    uint32_t   last_scan   = 0;
    uint32_t   last_blink  = 0;
    bool       blink_on    = false;
    while(1)
    {
        UsbMscProcess();

        /* Scan the keys at ~1 kHz so USB transfers keep full speed. */
        const uint32_t now = System::GetNow();
        if(now == last_scan)
            continue;
        last_scan = now;
        button_sr.Update();

        /* RisingEdge() latches an internal "risen" flag that only
         * FallingEdge() clears, so both edges must be polled on every scan
         * or a key press can never be detected twice. */
        const bool overdub_hit = button_sr.RisingEdge(kOverdubKeySr);
        button_sr.FallingEdge(kOverdubKeySr);
        const bool chompi_hit = button_sr.RisingEdge(kChompiKeySr);
        button_sr.FallingEdge(kChompiKeySr);

        switch(reset_state)
        {
            case ResetState::Idle:
                if(overdub_hit)
                {
                    reset_state = ResetState::Confirming;
                    blink_on    = true;
                    last_blink  = now;
                    PaintKeyLeds(reset_state, blink_on);
                }
                break;

            case ResetState::Confirming:
                if(overdub_hit)
                {
                    /* "No": back to solid red, nothing changes. */
                    reset_state = ResetState::Idle;
                    PaintKeyLeds(reset_state, false);
                }
                else if(chompi_hit)
                {
                    /* "Yes": unmount from the host and restart the device. */
                    reset_state = ResetState::ShuttingDown;
                    ShutdownAndReset(usb_ready);
                }
                else if(now - last_blink >= kBlinkPeriodMs)
                {
                    last_blink = now;
                    blink_on   = !blink_on;
                    PaintKeyLeds(reset_state, blink_on);
                }
                break;

            case ResetState::ShuttingDown:
                break; // never reached: ShutdownAndReset() does not return
        }
    }
}
