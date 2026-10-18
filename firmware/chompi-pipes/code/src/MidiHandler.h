/** @file MidiHandler.h
 *  @brief Minimal MIDI in/out for the organ: fixed channel 1, notes both ways,
 *  CC 70-73 for the four pipe levels, CC 74-77 for the four pipe Character
 *  amounts (tonewheel color; 0 = clean sine, 127 ~ step 64 = fully aged).
 *  UART (5-pin DIN) uses DMA transmit;
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
    static constexpr uint8_t kChannel          = 0; /* MIDI channel 1 */
    static constexpr uint8_t kPipeCcBase       = 70;
    static constexpr uint8_t kCharacterCcBase  = 74;
    static constexpr uint8_t kKeyVelocity      = 100;

    void Init(chompi::OrganEngine *engine)
    {
        engine_ = engine;

        fifo_head = fifo_tail = 0;
        uart_tx_busy          = false;

        MidiUartHandler::Config uart_midi_cfg;
        uart_midi.Init(uart_midi_cfg);
        uart_midi.StartReceive();

        /* C8: GetMutableTransport() returns a reference; copying the
         * transport only worked by accident (pimpl). */
        auto &transport = uart_midi.GetMutableTransport();
        uart_handler_   = transport.GetUartHandle();

        MidiUsbHandler::Config usb_midi_cfg;
        usb_midi_cfg.transport_config.periph = MidiUsbTransport::Config::EXTERNAL;
        /* B7: never retry USB TX - each retry blocks the main loop for
         * 100 us while the host isn't reading; a failed send is dropped. */
        usb_midi_cfg.transport_config.tx_retry_count = 0;
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
                    else if(cc >= kCharacterCcBase
                            && cc < kCharacterCcBase + chompi::OrganEngine::kNumPipes)
                    {
                        /* 0-127 -> 0-64, rounded */
                        const int step = (static_cast<int>(val)
                                              * chompi::OrganEngine::kMaxCharacterSteps
                                          + 63)
                                         / 127;
                        engine_->SetPipeCharacterStep(cc - kCharacterCcBase, step);
                        if(cc_level_callback_)
                            cc_level_callback_(cc - kCharacterCcBase, 1);
                    }
                    break;
                }
                default: break;
            }
        }
    }

    /** Call from the main loop: starts the next queued UART DMA transfer.
     *  A1/A2: never touches the UART with IRQs disabled or from an ISR -
     *  libDaisy StartDmaTx spins on HAL state with a frozen tick there and
     *  would lock the whole chip. Only this function sets uart_tx_busy,
     *  only TxEndCallback clears it, so the ring needs no critical
     *  section (SPSC). */
    void ProcessMidiOut()
    {
        if(!uart_tx_busy && fifo_head != fifo_tail)
        {
            uart_tx_busy = true;
            StartDmaTransfer();
        }
    }

    /** Call from the main loop every ~50 ms: keeps the UART listen DMA
     *  alive. B1: after any RX error (framing/noise/overrun, e.g. a DIN
     *  plug wobble) HAL aborts the RX DMA and libDaisy never restarts it
     *  (the abort-complete IRQ it waits for is NVIC-disabled), so MIDI-in
     *  died permanently. CheckError() latches the error; a stop+start
     *  recovers (HAL_UART_Receive_DMA clears the error code). This must
     *  not run in the audio ISR: DmaListenStart re-inits HAL with
     *  tick-based timeouts that freeze inside an ISR. */
    void ServiceMidiIn()
    {
        if(uart_handler_.CheckError() != 0)
        {
            uart_handler_.DmaListenStop();
            uart_midi.StartReceive();
        }
        else
        {
            uart_midi.Listen(); /* restarts a silently stopped listener */
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

    void SendCharacterCC(int pipe, int step)
    {
        SendStepCC(kCharacterCcBase + pipe, step,
                   chompi::OrganEngine::kMaxCharacterSteps);
    }

    /** Optional hook so the UI can show the level bar on CC changes.
 *      page: 0 = volume bar, 1 = Character bar. */
    void SetCcLevelCallback(void (*cb)(int pipe, int page))
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
        /* B1: no Listen() here - the listener is serviced from the main
         * loop (ServiceMidiIn) so HAL re-init never runs in the audio ISR */
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

    /** Enqueue only - the transmit itself happens in ProcessMidiOut()
     *  (main loop). A1: this used to call into UartHandler with
     *  __disable_irq(), which can deadlock the chip inside StartDmaTx. */
    void QueueDmaMessage(uint8_t *msg, size_t length)
    {
        /* A2: plain SPSC ring (main loop produces, TX-done ISR consumes).
         * When full, drop the message instead of overwriting unsent
         * entries (the old code lapped head past tail and then looked
         * empty / resent stale data). */
        const size_t next = (fifo_head + 1) % kFifoSize;
        if(next == fifo_tail)
        {
            tx_drops_++;
            return;
        }
        fifo[fifo_head].length = length;
        memcpy(fifo[fifo_head].data, msg, length);
        fifo_head = next;
    }

    static void TxEndCallback(void *context, daisy::UartHandler::Result)
    {
        OrganMidi *self = static_cast<OrganMidi *>(context);

        /* A2: ISR context - release the slot and nothing else. The main
         * loop starts the next transfer; no HAL calls from in here (the
         * HAL tick is frozen in ISR context). */
        self->fifo_tail    = (self->fifo_tail + 1) % kFifoSize;
        self->uart_tx_busy = false;
    }

    static constexpr size_t kFifoSize = 64;
    UartHandler uart_handler_;
    OrganMidiFifoEntry fifo[kFifoSize];
    volatile size_t   fifo_head, fifo_tail;
    volatile bool     uart_tx_busy;
    volatile uint32_t tx_drops_ = 0; /* diagnostics: full-ring drops (A2) */

    chompi::OrganEngine *engine_;
    MidiUartHandler      uart_midi;
    MidiUsbHandler       usb_midi;
    bool                 usb_midi_active_ = true;
    void (*cc_level_callback_)(int pipe, int page) = nullptr;
};
