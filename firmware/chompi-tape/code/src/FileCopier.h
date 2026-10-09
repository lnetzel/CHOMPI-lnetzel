#pragma once
#include "daisy.h"
#include "fatfs.h"
#include "FileStreamingManager.h"

namespace daisy {

class FileCopier
{
    public:
        FileCopier() {}
        ~FileCopier() {}

        void Init(float samplerate, Engine* fx,
                    RamBufferMemory* chompi_buff, RamBufferMemory* looper_buff)
        {
            fx_ = fx;
            copying_ = false;

            // WAV file header template
            file_header.ChunkId       = kWavFileChunkId;     /** "RIFF" */
            file_header.FileFormat    = kWavFileWaveId;      /** "WAVE" */
            file_header.SubChunk1ID   = kWavFileSubChunk1Id; /** "fmt " */
            file_header.SubChunk1Size = 16;                  // for PCM
            file_header.AudioFormat   = WAVE_FORMAT_PCM;
            file_header.NbrChannels   = 2;
            file_header.SampleRate    = static_cast<int>(samplerate);
            file_header.ByteRate      = samplerate * 2 * 16 / 8; // sr * chan * bitspersample / 8
            file_header.BlockAlign    = 2 * 16 / 8; //channels * bitspersample / 8;
            file_header.BitPerSample  = 16;
            file_header.SubChunk2ID   = kWavFileSubChunk2Id; /** "data" */

            chompi_ram.Init(chompi_buff);
            looper_ram.Init(looper_buff);
        }

        bool CopyProcess()
        {
            if(!copying_ && !req_fifo.IsEmpty())
            {
                copying_ = true;
                req = req_fifo.PopFront();

                looper_undo_ready_ = false;
                looper_undo_invalidate_sent_ = false;

                is_chompi_ram = req.is_chompi;
                is_looper_ram = req.is_looper;

                chompi_ram.SetReadHead(0);
                chompi_ram.SetWriteHead(0);

                looper_ram.SetReadHead(0);
                if(req.append && is_looper_ram == CopyRequest::RamDir::TO)
                    looper_ram.SetWriteHead(looper_ram.GetSize());
                else
                    looper_ram.SetWriteHead(0);

                append_xfade_done = !req.append || looper_ram.GetSize() == 0;

                CopyStart(req.src, req.src_bank, req.src_mode, req.dest, req.dest_bank, req.dest_mode);
            }

            if(!copying_)
                return false;


            if(!CopySizeFull() && !IsEOF())
            {
                UINT read_num = 0;

                uint32_t read_size = buff_size;
                if(copyread + read_size >= copysize && copysize != 0)
                    read_size = copysize - copyread;

                // read
                if(is_chompi_ram == CopyRequest::RamDir::FROM)
                {
                    chompi_ram.BlockRead((int16_t*)buff, read_size / 2);
                    read_num = read_size;
                }
                else if(is_looper_ram == CopyRequest::RamDir::FROM)
                {
                    looper_ram.BlockRead((int16_t*)buff, read_size / 2);
                    read_num = read_size;
                }
                else
                {
                    f_read(&fptr_read, buff, read_size, &read_num);
                }


                // write
                if(is_chompi_ram == CopyRequest::RamDir::TO)
                {
                    chompi_ram.BlockWrite((int16_t*)buff, read_num / 2);
                }
                else if(is_looper_ram == CopyRequest::RamDir::TO)
                {
                    // Never mutate the loop during a confirmed undo; before
                    // the first mutation, request history invalidation and
                    // await the audio-side block-boundary acknowledgment.
                    if(!looper_undo_ready_)
                    {
                        if(fx_->UndoBusy())
                            return true;
                        if(!looper_undo_invalidate_sent_)
                        {
                            fx_->InvalidateLooperUndo();
                            looper_undo_invalidate_sent_ = true;
                            return true;
                        }
                        if(fx_->LooperUndoInvalidatePending())
                            return true;
                        looper_undo_ready_ = true;
                    }

                    if(!append_xfade_done)
                    {
                        ApplyAppendCrossfade((int16_t*)buff, read_num / 2);
                        append_xfade_done = true;
                    }
                    looper_ram.BlockWrite((int16_t*)buff, read_num / 2);
                }
                else
                {
                    if(misalign != 0)
                    {
                        for(size_t i = 0; i < misalign; i++)
                        {
                            buff[i] = 0;
                        }
                        misalign = 0;
                    }
                    f_write(&fptr_write, buff, read_size, nullptr);
                    f_sync(&fptr_write);
                }

                copyread += read_num;

                // double speed write
                if(is_chompi_ram != CopyRequest::RamDir::TO && is_looper_ram != CopyRequest::RamDir::TO)
                {
                    size_t write = 0;
                    size_t read = 0;
                    while(read < read_num)
                    {
                        buff[write] = buff[read];
                        buff[write + 1] = buff[read + 1];
                        buff[write + 2] = buff[read + 2];
                        buff[write + 3] = buff[read + 3];

                        write += 4;
                        read += 8;
                    }

                    f_write(&fptr_write_double, buff, read_num / 2, nullptr);
                    f_sync(&fptr_write_double);
                }
            }

            else
            {
                CopyDone();
            }

            return true;
        }

        inline bool IsCopying()  { return copying_ || !req_fifo.IsEmpty(); }

        bool FileExists(size_t idx, VoiceMode mode, size_t bank, bool dbl = false)
        {
            char fname[32];
            Engine::GetFileNameForSlot(idx + 1, bank, mode, fname, dbl);
            return f_stat(fname, nullptr) == FR_OK;
        }

        bool NeedsOverwrite(size_t idx, VoiceMode mode, size_t bank)
        {
            char fname[32];
            Engine::GetFileNameForSlot(idx + 1, bank, mode, fname, false);

            // no file, no overwrite
            if(f_stat(fname, nullptr) != FR_OK)
                return false;

            f_open(&fptr_read, fname, (FA_OPEN_ALWAYS | FA_WRITE | FA_READ));

            // large header || has footer?
            uint32_t size = 0;
            if(JumpToData(fname, &size))
            {
                CloseFile(&fptr_read);
                return true;
            }

            CloseFile(&fptr_read);

            // missing 2x file?
            Engine::GetFileNameForSlot(idx + 1, bank, mode, fname, true);
            if(f_stat(fname, nullptr) != FR_OK)
                return true;

            f_open(&fptr_read, fname, (FA_OPEN_ALWAYS | FA_WRITE | FA_READ));

            // 2x file large header || has footer?
            uint32_t dbl_size = 0;
            if(JumpToData(fname, &dbl_size))
            {
                CloseFile(&fptr_read);
                return true;
            }

            CloseFile(&fptr_read);

            // wrong 2x size
            return ((size - sizeof(WAV_FormatTypeDef)) / 2) != (dbl_size - sizeof(WAV_FormatTypeDef));
        }

        struct CopyRequest {

            enum RamDir : uint8_t {
                NONE,
                FROM,
                TO
            };

            CopyRequest(size_t s, size_t b, VoiceMode m, size_t d, size_t db, VoiceMode dm, bool st, RamDir ic, RamDir il, bool ap = false)
                : src(s),
                src_bank (b),
                src_mode(m),
                dest(d),
                dest_bank(db),
                dest_mode(dm),
                is_chompi(ic),
                is_looper(il),
                set(st),
                append(ap)
            {
            }

            // default (trivially constructible -> FIFO zero-inits into .bss)
            CopyRequest() = default;

            ~CopyRequest() {}

            size_t src = 0;
            size_t src_bank = 0;
            VoiceMode src_mode = VoiceMode::JAMMI;
            size_t dest = 0;
            size_t dest_bank = 0;
            VoiceMode dest_mode = VoiceMode::JAMMI;
            RamDir is_chompi = RamDir::NONE;
            RamDir is_looper = RamDir::NONE;
            bool set = false;
            bool append = false;

            /** Optional normalized source range [0, 1], taken from the source
             *  preset's saved start/end. trim == false (the default) copies the
             *  full source, preserving the behavior of all existing callers. */
            bool trim = false;
            float trim_start = 0.f;
            float trim_end = 1.f;
        };

        FIFO<CopyRequest, 16> req_fifo;

    private:
        void CopyStart(size_t src, size_t src_bank, VoiceMode src_mode, size_t dest, size_t dest_bank, VoiceMode dest_mode)
        {

            if(dest < 16)
                fx_->SetFileExists(dest - 1, dest_bank, dest_mode, true);

            char fname[32];

            copyread = 0;
            copysize = 0;

            if(is_chompi_ram != CopyRequest::RamDir::FROM && is_looper_ram != CopyRequest::RamDir::FROM)
            {
                // close then open read FIL
                CloseFile(&fptr_read);

                Engine::GetFileNameForSlot(src, src_bank, src_mode, fname, false);
                f_open(&fptr_read, fname, (FA_OPEN_ALWAYS | FA_WRITE | FA_READ));

                // jump read FIL past the header, set size variables
                JumpToData(fname, &copysize);

                // Restrict the copy to the source preset's saved start/end
                // region (SD-card preset -> looper copies). Offsets are relative
                // to the parsed data chunk and stereo-frame aligned, matching
                // SampleReader's playback convention for standard WAV files.
                // copysize == 0 means no data chunk was found; keep the legacy
                // copy-until-EOF policy for such nonstandard files.
                if(req.trim && copysize != 0)
                {
                    const uint32_t data_start = f_tell(&fptr_read);

                    uint32_t start_byte = req.trim_start * copysize;
                    start_byte -= start_byte % 4; // stereo frame = 2ch * 16bit
                    uint32_t end_byte = req.trim_end * copysize;
                    end_byte -= end_byte % 4;

                    if(end_byte > start_byte
                        && f_lseek(&fptr_read, data_start + start_byte) == FR_OK)
                    {
                        copysize = end_byte - start_byte;
                    }
                    else
                    {
                        // empty selection or failed seek: reject the copy before
                        // anything is written to the destination or its metadata
                        CloseFile(&fptr_read);
                        copying_ = false;
                        return;
                    }
                }
            }
            else if(is_chompi_ram == CopyRequest::RamDir::FROM)
            {
                copysize = chompi_ram.GetSize() * 2;
                src_bank = 0;
                src_mode = VoiceMode::CUBBI;
            }
            else if(is_looper_ram == CopyRequest::RamDir::FROM)
            {
                copysize = looper_ram.GetSize() * 2;
                src_bank = 0;
                src_mode = VoiceMode::CUBBI;
            }

            if(is_chompi_ram != CopyRequest::RamDir::TO && is_looper_ram != CopyRequest::RamDir::TO)
            {
                // close then open write FILs
                CloseFile(&fptr_write);
                CloseFile(&fptr_write_double);


                Engine::GetFileNameForSlot(dest, dest_bank, dest_mode, fname, false);
                f_open(&fptr_write, fname, (FA_OPEN_ALWAYS | FA_WRITE | FA_READ));
                WriteHeader(&fptr_write);

                Engine::GetFileNameForSlot(dest, dest_bank, dest_mode, fname, true);
                f_open(&fptr_write_double, fname, (FA_OPEN_ALWAYS | FA_WRITE | FA_READ));
                WriteHeader(&fptr_write_double);
            }

            copying_ = true;
        }

        void WriteHeader(FIL* fil)
        {
            // write header
            UINT bw = 0;
            file_header.SubCHunk2Size = f_size(fil) - sizeof(file_header);
            file_header.FileSize = f_size(fil);

            f_lseek(fil, 0);
            f_write(fil, &file_header, sizeof(file_header), &bw);
            f_sync(fil);
        }

        void CloseFile(FIL* fil)
        {
            if(f_size(fil) != 0)
            {
                f_close(fil);
                fil->obj.objsize = 0;
            }
        }

        void CopyDone()
        {
            if(is_chompi_ram != CopyRequest::RamDir::TO && is_looper_ram != CopyRequest::RamDir::TO)
            {
                f_truncate(&fptr_write);
                f_truncate(&fptr_write_double);

                WriteHeader(&fptr_write);
                WriteHeader(&fptr_write_double);

                CloseFile(&fptr_write);
                CloseFile(&fptr_write_double);
            }

            if(is_chompi_ram != CopyRequest::RamDir::FROM && is_looper_ram != CopyRequest::RamDir::FROM)
                CloseFile(&fptr_read);

            fptr_read.obj.objsize = 0;
            fptr_write.obj.objsize = 0;
            fptr_write_double.obj.objsize = 0;

            copying_ = false;

            if(req.set)
            {
                fx_->SetBank(req.dest_bank);
                fx_->SetVoiceMode(req.dest_mode);
                fx_->SetVoiceSlot(req.dest, false);
            }

            fx_->SetAllCopyOccurred();
        }

        inline bool CopySizeFull() { return (copyread >= copysize && copysize != 0); }

        bool IsData(uint8_t* data)
        {
            return (data[0] == 'd') && (data[1] == 'a') && (data[2] == 't') && (data[3] == 'a');
        }

        bool JumpToData(char* fname, uint32_t* size)
        {
            if(size != nullptr)
                *size = f_size(&fptr_read);

            uint8_t data[4];
            copysize = 0;
            misalign = 0;

            uint32_t ctr = 0;
            UINT br;
            while(ctr < 2048 && !IsEOF())
            {
                f_lseek(&fptr_read, ctr);
                f_read(&fptr_read, data, 4, &br);
                ctr++;

                if(br != 4)
                {
                    break;
                }
                else if(IsData(data))
                {
                    f_read(&fptr_read, data, 4, &br); // read the data size

                    for(size_t i = 0; i < 4; i++)
                    {
                        copysize += uint32_t(data[i]) << (i * 8);
                    }

                    bool header = f_tell(&fptr_read) != sizeof(WAV_FormatTypeDef);
                    bool footer = f_tell(&fptr_read) + copysize != f_size(&fptr_read);

                    // fix 4 byte grid misalignment due to odd sized headers
                    misalign = f_tell(&fptr_read) % 4;
                    if(misalign == 2)
                        f_lseek(&fptr_read, f_tell(&fptr_read) - misalign);

                    return header || footer;
                }
            }

            // couldn't find "data", jump back to start
            f_lseek(&fptr_read, 0);
            return false;
        }

        bool IsEOF() 
        {
            if(is_chompi_ram == CopyRequest::RamDir::FROM)
                return chompi_ram.ReadEOF();
            else if(is_looper_ram == CopyRequest::RamDir::FROM)
                return looper_ram.ReadEOF();

            return f_eof(&fptr_read);
        }

        /** De-click at the append splice, mirroring the looper's loop_env
         *  wrap fade: fade the existing tail out to 0 and the appended head
         *  in from 0 over ~5ms, so the boundary passes through silence.
         *  No samples are added or removed. `data` is the first appended
         *  block, modified in place; the tail is tapered in place.
         */
        __attribute__((noinline))
        void ApplyAppendCrossfade(int16_t* data, size_t num_samples)
        {
            const size_t splice = looper_ram.GetWriteHead(); // = old loop end
            size_t n = num_samples / 2;
            if(n > kAppendXfade)
                n = kAppendXfade;
            if(splice < 2 * n)
                n = splice / 2;
            if(n == 0)
                return;

            const size_t tail_base = splice - (2 * n);
            const int32_t tstep = 4096 / (int32_t)n;

            int32_t tout = 4096 - tstep;          // tail fade-out 4096->0
            int32_t tin = 0;                       // source fade-in 0->4096
            for(size_t i = 0; i < n; i++, tout -= tstep, tin += tstep)
            {
                const size_t d = 2 * i;
                const size_t b = tail_base + d;

                looper_ram.Poke(b,     (int16_t)((looper_ram.Peek(b) * tout) >> 12));
                looper_ram.Poke(b + 1, (int16_t)((looper_ram.Peek(b + 1) * tout) >> 12));

                data[d]     = (int16_t)((data[d] * tin) >> 12);
                data[d + 1] = (int16_t)((data[d + 1] * tin) >> 12);
            }
        }

        enum { kAppendXfade = 240 }; // ~5ms @ 48kHz stereo
        bool append_xfade_done;

        // undo handshaking for loop-destined copies
        bool looper_undo_ready_ = false;
        bool looper_undo_invalidate_sent_ = false;

        FIL fptr_read;
        FIL fptr_write;
        FIL fptr_write_double;
        bool copying_;
        uint32_t copysize = 0;
        uint32_t copyread = 0;
        WAV_FormatTypeDef file_header;

        // RAM IO
        CopyRequest::RamDir is_chompi_ram;
        CopyRequest::RamDir is_looper_ram;        
        RamBuffer chompi_ram;
        RamBuffer looper_ram;        

        CopyRequest req;

        static const size_t buff_size = 4096;
        uint8_t buff[buff_size];
        uint8_t misalign;

        Engine* fx_;
};
} // namespace daisy