/** @file SettingsManager.h
 *  @brief Pipe levels persisted to SD card (FR-0a): loaded once at boot,
 *  auto-saved 2 s after the last change. Missing/unreadable card or file
 *  falls back to defaults and the instrument stays playable.
 *
 *  File: /organ_settings.json. The JSON object leaves room for future
 *  preset/registration entries.
 */
#pragma once
#include <string.h>
#include <stdio.h>
#include "fatfs.h"
#include "OrganEngine.h"

namespace chompi
{

class SettingsManager
{
  public:
    static constexpr const char *kFileName = "organ_settings.json";

    void Init(OrganEngine *engine)
    {
        engine_    = engine;
        dirty_     = false;
        available_ = false;
        last_change_ms_ = 0;
    }

    /** Try to load levels from the SD card. Call after f_mount. */
    void Load()
    {
        FIL  fil;
        UINT br = 0;
        char buf[128];

        if(f_open(&fil, kFileName, FA_READ) != FR_OK)
            return;

        memset(buf, 0, sizeof(buf));
        const FRESULT res = f_read(&fil, buf, sizeof(buf) - 1, &br);
        f_close(&fil);

        if(res != FR_OK || br == 0)
            return;

        /* expect: ... "pipe_levels": [a, b, c, d] ... */
        const char *key = strstr(buf, "pipe_levels");
        if(key == nullptr)
            return;
        const char *arr = strchr(key, '[');
        if(arr == nullptr)
            return;

        int steps[OrganEngine::kNumPipes] = {0, 0, 0, 0};
        if(sscanf(arr, "[%d, %d, %d, %d]", &steps[0], &steps[1], &steps[2], &steps[3])
           != OrganEngine::kNumPipes)
            return;

        for(int p = 0; p < OrganEngine::kNumPipes; p++)
            engine_->SetPipeStep(p, steps[p]);

        available_ = true;
    }

    /** Mark levels changed; the file is written 2 s after the last change. */
    void NotifyChanged()
    {
        dirty_         = true;
        last_change_ms_ = daisy::System::GetNow();
    }

    /** Call from the main loop. */
    void Poll()
    {
        if(!dirty_)
            return;
        if(daisy::System::GetNow() - last_change_ms_ < 2000)
            return;

        dirty_ = false;
        Write();
    }

  private:
    void Write()
    {
        char buf[128];
        const int len = snprintf(buf,
                                 sizeof(buf),
                                 "{\n\t\"chompi_organ\": {\n\t\t\"version\": 1,"
                                 "\n\t\t\"pipe_levels\": [%d, %d, %d, %d]\n\t}\n}\n",
                                 engine_->pipe_steps[0],
                                 engine_->pipe_steps[1],
                                 engine_->pipe_steps[2],
                                 engine_->pipe_steps[3]);

        FIL  fil;
        UINT bw = 0;
        if(f_open(&fil, kFileName, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
            return;
        f_write(&fil, buf, len, &bw);
        f_close(&fil);
        available_ = true;
    }

    OrganEngine *engine_;
    bool         dirty_;
    bool         available_;
    uint32_t     last_change_ms_;
};

} // namespace chompi
