/** @file MidiHandler.h
 *  @brief Minimal MIDI in/out for the organ: fixed channel 1, notes both ways,
 *  CC 70-73 for the four pipe levels, CC 74-77 for the four pipe waveform
 *  morphs. UART (5-pin DIN) uses DMA transmit;
 *  adapted from chompi-wave's MidiManager.h (see FR-5b).
 */
#pragma once
#include "daisy_seed.h"
#include "OrganEngine.h"

using namespace daisy;

struct OrganMidiFifoEntry
{
    uint8_t data[3];
    size_t  length;
};

class OrganMidi
{
  public:
    static constexpr uint8_t kChannel     = 0; /* MIDI channel 1 */
    static constexpr uint8_t kPipeCcBase  = 70;
    static constexpr uint8_t kWaveCcBase  = 74;
    static constexpr uint8_t kKeyVelocity = 100;

    void Init(chompi::OrganEngine *engine)
    {
        engine_ = engine;

        fifo_head = fifo_tail = 0;
        uart_tx_busy          = false;

        MidiUartHandler::Config uart_midi_cfg;
        uart_midi.Init(uart_midi_cfg);
        uart_midi.StartReceive();

        MidiUartTransport transport = uart_midi.GetMutableTransport();
        uart_handler_               = transport.GetUartHandle();

        MidiUsbHandler::Config usb_midi_cfg;
        usb_midi_cfg.transport_config.periph = MidiUsbTransport::Config::EXTERNAL;
        usb_midi.Init(usb_midi_cfg);
        usb_midi.Listen();
    }

    /** Call from the audio ISR: incoming notes/CCs become engine requests. */
    void ProcessMidiIn()
    {
        MidiEvent event;
        while(GetMidi(event))
        {
            if(event.channel != kChannel)
                continue;

            switch(event.type)
            {
                case NoteOn:
                    if(event.data[1] > 0)
                        engine_->NoteOn(event.data[0],
                                        static_cast<float>(event.data[1]) / 127.f);
                    else
                        engine_->NoteOff(event.data[0]); /* vel-0 = note off */
                    break;
                case NoteOff:
                    engine_->NoteOff(event.data[0]);
                    break;
                case ControlChange:
                {
                    const uint8_t cc  = event.data[0];
                    const uint8_t val = event.data[1];
                    if(cc >= kPipeCcBase && cc < kPipeCcBase + chompi::OrganEngine::kNumPipes)
                    {
                        /* 0-127 -> 0-32, rounded */
                        const int step = (static_cast<int>(val)
                                              * chompi::OrganEngine::kMaxSteps
                                          + 63)
                                         / 127;
                        engine_->SetPipeStep(cc - kPipeCcBase, step);
                        if(cc_level_callback_)
                            cc_level_callback_(cc - kPipeCcBase, 0);
                    }
                    else if(cc >= kWaveCcBase
                            && cc < kWaveCcBase + chompi::OrganEngine::kNumPipes)
                    {
                        /* 0-127 -> 0-64, rounded */
                        const int step = (static_cast<int>(val)
                                              * chompi::OrganEngine::kMaxWaveSteps
                                          + 63)
                                         / 127;
                        engine_->SetPipeWaveStep(cc - kWaveCcBase, step);
                        if(cc_level_callback_)
                            cc_level_callback_(cc - kWaveCcBase, 1);
                    }
                    break;
                }
                default: break;
            }
        }
    }

    /** Call from the main loop: drains the UART DMA out queue. */
    void ProcessMidiOut()
    {
        if(fifo_head != fifo_tail)
        {
            if(!uart_tx_busy)
            {
                ScopedIrqBlocker block;
                DequeueDmaMessage();
            }
        }
    }

    void SendNoteOn(uint8_t note)
    {
        uint8_t buf[3] = {static_cast<uint8_t>(0x90 | kChannel), note, kKeyVelocity};
        QueueDmaMessage(buf, 3);
        if(usb_midi_active_)
            usb_midi.SendNoteOn(kChannel, note, kKeyVelocity);
    }

    void SendNoteOff(uint8_t note)
    {
        uint8_t buf[3] = {static_cast<uint8_t>(0x80 | kChannel), note, 0};
        QueueDmaMessage(buf, 3);
        if(usb_midi_active_)
            usb_midi.SendNoteOff(kChannel, note, 0);
    }

    void SendPipeCC(int pipe, int step)
    {
        SendStepCC(kPipeCcBase + pipe, step, chompi::OrganEngine::kMaxSteps);
    }

    void SendWaveCC(int pipe, int step)
    {
        SendStepCC(kWaveCcBase + pipe, step, chompi::OrganEngine::kMaxWaveSteps);
    }

    /** Optional hook so the UI can show the level bar on CC changes.
 *      wave_page: 0 = volume bar, 1 = waveform bar. */
    void SetCcLevelCallback(void (*cb)(int pipe, int wave_page))
    {
        cc_level_callback_ = cb;
    }

  private:
    void SendStepCC(int cc, int step, int max_steps)
    {
        const uint8_t val = static_cast<uint8_t>(
            (step * 127 + max_steps / 2) / max_steps);
        uint8_t buf[3] = {static_cast<uint8_t>(0xB0 | kChannel),
                          static_cast<uint8_t>(cc),
                          val};
        QueueDmaMessage(buf, 3);
        if(usb_midi_active_)
            usb_midi.SendCC(kChannel, cc, val);
    }

    bool GetMidi(MidiEvent &event)
    {
        uart_midi.Listen();

        if(uart_midi.HasEvents())
        {
            event = uart_midi.PopEvent();
            return true;
        }
        else if(usb_midi.HasEvents())
        {
            event = usb_midi.PopEvent();
            return true;
        }

        return false;
    }

    /* DMA out queue, same pattern as chompi-wave's MidiManager */
    void StartDmaTransfer()
    {
        uint8_t *p   = fifo[fifo_tail].data;
        size_t   len = fifo[fifo_tail].length;

        uintptr_t start = (uintptr_t)p & ~(32 - 1);
        uintptr_t end   = ((uintptr_t)p + len + 32 - 1) & ~(32 - 1);
        SCB_CleanDCache_by_Addr((uint32_t *)start, end - start);

        uart_handler_.DmaTransmit(p, len, nullptr, TxEndCallback, this);
    }

    void QueueDmaMessage(uint8_t *msg, size_t length)
    {
        fifo[fifo_head].length = length;
        memcpy(fifo[fifo_head].data, msg, length);
        fifo_head = (fifo_head + 1) % 64;

        if(!uart_tx_busy)
        {
            __disable_irq();
            DequeueDmaMessage();
            __enable_irq();
        }
    }

    void DequeueDmaMessage()
    {
        if(fifo_head == fifo_tail)
        {
            fifo_head = (fifo_head + 1) % 64;
        }
        if(!uart_tx_busy)
        {
            uart_tx_busy = true;
            StartDmaTransfer();
        }
    }

    static void TxEndCallback(void *context, daisy::UartHandler::Result)
    {
        OrganMidi *self = static_cast<OrganMidi *>(context);

        self->fifo_tail++;
        if(self->fifo_tail > 63)
        {
            self->fifo_tail = 0;
        }
        self->uart_tx_busy = false;
        if(self->fifo_head != self->fifo_tail)
        {
            self->DequeueDmaMessage();
        }
    }

    UartHandler uart_handler_;
    OrganMidiFifoEntry fifo[64];
    size_t fifo_head, fifo_tail;
    bool   uart_tx_busy;

    chompi::OrganEngine *engine_;
    MidiUartHandler      uart_midi;
    MidiUsbHandler       usb_midi;
    bool                 usb_midi_active_ = true;
    void (*cc_level_callback_)(int pipe, int wave_page) = nullptr;
};
