#include "usb_msc.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "stm32h7xx_hal.h"
extern "C"
{
#include "util/bsp_sd_diskio.h"
}
#include "usbd_core.h"
#include "usbd_ctlreq.h"
#include "usbd_def.h"
#include "diskio.h"

extern SD_HandleTypeDef hsd1;

namespace
{
constexpr uint8_t kBulkInEp = 0x81;
constexpr uint8_t kBulkOutEp = 0x01;
constexpr uint16_t kEndpointSize = 64;
constexpr uint32_t kBlockSize = 512;
// Sectors moved per SD command and per USB transfer. One multi-block command
// per 32 KB instead of one command per sector is what makes writes usable:
// the card runs a full program cycle per command. libDaisy's
// USBD_LL_Transmit/PrepareReceive take a 16-bit length, which caps this.
constexpr uint32_t kChunkBlocks = 64;
constexpr uint32_t kChunkBytes = kChunkBlocks * kBlockSize;
static_assert(kChunkBytes <= 0xFFFF, "USBD_LL_* lengths are 16-bit");
constexpr uint32_t kCbwSignature = 0x43425355;
constexpr uint32_t kCswSignature = 0x53425355;

enum class BotState : uint8_t
{
    AwaitingCbw,
    SendingResponse,
    ReadingBlocks,
    WritingBlocks,
    Padding,
    SendingCsw,
};

USBD_HandleTypeDef usb_device;
uint32_t media_blocks = 0;
bool media_ready = false;
BotState bot_state = BotState::AwaitingCbw;
volatile bool io_pending = false;

uint8_t cbw[31];
uint8_t csw[13];
uint8_t response[64];
alignas(32) uint8_t sector[kChunkBytes];
uint32_t command_tag = 0;
uint32_t residue = 0;
uint32_t current_lba = 0;
uint32_t blocks_left = 0;
uint32_t chunk_blocks = 0; // sectors in the transfer currently in flight
uint8_t command_status = 0;
uint8_t sense_key = 0;
uint8_t sense_asc = 0;
uint8_t sense_ascq = 0;
constexpr uint32_t kTraceSize = 128;

// Op 0xF1-0xF3 mark bus events: reset, GET_MAX_LUN, configured.
struct TraceEntry
{
    uint32_t lba;
    uint32_t length;
    uint32_t sd_error;
    uint8_t op;
    uint8_t status;
    uint8_t key;
    uint8_t asc;
};

TraceEntry trace[kTraceSize];
volatile uint32_t trace_count = 0;
volatile bool eject_seen = false;

void TraceEvent(uint8_t op, uint32_t lba, uint32_t length)
{
    TraceEntry& entry = trace[trace_count % kTraceSize];
    entry = {lba, length, 0, op, 0xFF, 0, 0};
    trace_count = trace_count + 1;
}

TraceEntry* CurrentTrace()
{
    return trace_count == 0 ? nullptr : &trace[(trace_count - 1) % kTraceSize];
}

uint8_t config_descriptor[] = {
    9, 0x02, 32, 0, 1, 1, 4, 0xC0, 0,
    9, 0x04, 0, 0, 2, 0x08, 0x06, 0x50, 5,
    7, 0x05, kBulkInEp, 0x02, kEndpointSize, 0, 0,
    7, 0x05, kBulkOutEp, 0x02, kEndpointSize, 0, 0,
};
uint8_t other_speed_descriptor[sizeof(config_descriptor)];
uint8_t qualifier_descriptor[] = {10, 0x06, 0x00, 0x02, 0, 0, 0, 64, 1, 0};
uint8_t device_descriptor[] = {
    18, 0x01, 0x00, 0x02, 0, 0, 0, 64,
    0x09, 0x12, 0xA1, 0xC0, 0x00, 0x01, 1, 2, 3, 1,
};

uint32_t ReadLe32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0])
           | (static_cast<uint32_t>(data[1]) << 8)
           | (static_cast<uint32_t>(data[2]) << 16)
           | (static_cast<uint32_t>(data[3]) << 24);
}

uint16_t ReadBe16(const uint8_t* data)
{
    return (static_cast<uint16_t>(data[0]) << 8)
           | static_cast<uint16_t>(data[1]);
}

uint32_t ReadBe32(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24)
           | (static_cast<uint32_t>(data[1]) << 16)
           | (static_cast<uint32_t>(data[2]) << 8)
           | static_cast<uint32_t>(data[3]);
}

void WriteBe32(uint8_t* data, uint32_t value)
{
    data[0] = static_cast<uint8_t>(value >> 24);
    data[1] = static_cast<uint8_t>(value >> 16);
    data[2] = static_cast<uint8_t>(value >> 8);
    data[3] = static_cast<uint8_t>(value);
}

void SetSense(uint8_t key, uint8_t asc, uint8_t ascq = 0)
{
    sense_key = key;
    sense_asc = asc;
    sense_ascq = ascq;
}

void ArmCbw(USBD_HandleTypeDef* device)
{
    bot_state = BotState::AwaitingCbw;
    USBD_LL_PrepareReceive(device, kBulkOutEp, cbw, sizeof(cbw));
}

void SendCsw(USBD_HandleTypeDef* device)
{
    TraceEntry* entry = CurrentTrace();
    if(entry != nullptr && entry->op < 0xF0)
    {
        entry->status = command_status;
        entry->key = sense_key;
        entry->asc = sense_asc;
    }
    csw[0] = static_cast<uint8_t>(kCswSignature);
    csw[1] = static_cast<uint8_t>(kCswSignature >> 8);
    csw[2] = static_cast<uint8_t>(kCswSignature >> 16);
    csw[3] = static_cast<uint8_t>(kCswSignature >> 24);
    csw[4] = static_cast<uint8_t>(command_tag);
    csw[5] = static_cast<uint8_t>(command_tag >> 8);
    csw[6] = static_cast<uint8_t>(command_tag >> 16);
    csw[7] = static_cast<uint8_t>(command_tag >> 24);
    csw[8] = static_cast<uint8_t>(residue);
    csw[9] = static_cast<uint8_t>(residue >> 8);
    csw[10] = static_cast<uint8_t>(residue >> 16);
    csw[11] = static_cast<uint8_t>(residue >> 24);
    csw[12] = command_status;
    bot_state = BotState::SendingCsw;
    USBD_LL_Transmit(device, kBulkInEp, csw, sizeof(csw));
}

void SendResponse(USBD_HandleTypeDef* device, uint32_t length)
{
    const uint32_t transfer_length = std::min(length, residue);
    residue -= transfer_length;
    if(transfer_length == 0)
    {
        SendCsw(device);
        return;
    }

    bot_state = BotState::SendingResponse;
    USBD_LL_Transmit(device, kBulkInEp, response, transfer_length);
}

void SendPad(USBD_HandleTypeDef* device)
{
    if(residue == 0)
    {
        SendCsw(device);
        return;
    }

    const uint32_t length = std::min<uint32_t>(residue, sizeof(response));
    std::memset(response, 0, length);
    residue -= length;
    bot_state = BotState::Padding;
    USBD_LL_Transmit(device, kBulkInEp, response, length);
}

// Host expects a data-in phase, so fill it before reporting failure.
void FailCommand(USBD_HandleTypeDef* device, uint8_t key, uint8_t asc)
{
    SetSense(key, asc);
    command_status = 1;
    if((cbw[12] & 0x80) != 0 && residue > 0)
        SendPad(device);
    else
        SendCsw(device);
}

void ProcessReadBlock(USBD_HandleTypeDef* device)
{
    if(blocks_left == 0)
    {
        SendCsw(device);
        return;
    }

    const uint32_t count = std::min(blocks_left, kChunkBlocks);
    if(disk_read(0, sector, current_lba, count) != RES_OK)
    {
        TraceEntry* entry = CurrentTrace();
        if(entry != nullptr)
            entry->sd_error = hsd1.ErrorCode;
        FailCommand(device, 0x03, 0x11);
        return;
    }

    current_lba += count;
    blocks_left -= count;
    residue -= count * kBlockSize;
    USBD_LL_Transmit(device, kBulkInEp, sector,
                     static_cast<uint16_t>(count * kBlockSize));
}

void BeginRead(USBD_HandleTypeDef* device, uint32_t lba, uint32_t blocks)
{
    if(!media_ready)
    {
        FailCommand(device, 0x02, 0x3A);
        return;
    }

    const uint64_t end_block = static_cast<uint64_t>(lba) + blocks;
    if(end_block > media_blocks
       || residue != static_cast<uint64_t>(blocks) * kBlockSize)
    {
        FailCommand(device, 0x05, 0x21);
        return;
    }

    current_lba = lba;
    blocks_left = blocks;
    bot_state = BotState::ReadingBlocks;
    io_pending = blocks > 0;
    if(blocks == 0)
        SendCsw(device);
}

void ArmNextWriteBlock(USBD_HandleTypeDef* device)
{
    if(blocks_left == 0)
    {
        SendCsw(device);
        return;
    }

    chunk_blocks = std::min(blocks_left, kChunkBlocks);
    bot_state = BotState::WritingBlocks;
    USBD_LL_PrepareReceive(device, kBulkOutEp, sector,
                           static_cast<uint16_t>(chunk_blocks * kBlockSize));
}

void BeginWrite(USBD_HandleTypeDef* device, uint32_t lba, uint32_t blocks)
{
    const uint64_t end_block = static_cast<uint64_t>(lba) + blocks;
    if(!media_ready)
    {
        SetSense(0x02, 0x3A);
        command_status = 1;
        SendCsw(device);
        return;
    }
    if(end_block > media_blocks
       || residue != static_cast<uint64_t>(blocks) * kBlockSize)
    {
        SetSense(0x05, 0x21);
        command_status = 1;
        SendCsw(device);
        return;
    }

    current_lba = lba;
    blocks_left = blocks;
    ArmNextWriteBlock(device);
}

void HandleCommand(USBD_HandleTypeDef* device)
{
    command_tag = ReadLe32(&cbw[4]);
    residue = ReadLe32(&cbw[8]);
    command_status = 0;
    const uint8_t* cdb = &cbw[15];

    if(ReadLe32(cbw) != kCbwSignature || (cbw[13] & 0x0F) != 0
       || (cbw[14] & 0x1F) == 0 || (cbw[14] & 0x1F) > 16)
    {
        USBD_LL_StallEP(device, kBulkInEp);
        USBD_LL_StallEP(device, kBulkOutEp);
        TraceEvent(0xF0, ReadLe32(cbw), cbw[14]);
        return;
    }

    TraceEntry* previous = CurrentTrace();
    const bool repeat_poll = cdb[0] == 0x00 && previous != nullptr
                             && previous->op == 0x00;
    if(!repeat_poll)
        TraceEvent(cdb[0],
                   (cdb[0] == 0x28 || cdb[0] == 0x2A) ? ReadBe32(&cdb[2]) : 0,
                   residue);
    switch(cdb[0])
    {
        case 0x00: // TEST UNIT READY
            if(!media_ready)
            {
                SetSense(0x02, 0x3A);
                command_status = 1;
            }
            SendCsw(device);
            break;

        case 0x03: // REQUEST SENSE
            std::memset(response, 0, 18);
            response[0] = 0x70;
            response[2] = sense_key;
            response[7] = 10;
            response[12] = sense_asc;
            response[13] = sense_ascq;
            SendResponse(device, 18);
            break;

        case 0x12: // INQUIRY
            if((cdb[1] & 0x03) != 0)
            {
                FailCommand(device, 0x05, 0x24);
                break;
            }
            std::memset(response, 0, 36);
            response[0] = 0x00;
            response[1] = 0x80;
            response[2] = 0x06;
            response[3] = 0x02;
            response[4] = 31;
            std::memcpy(&response[8], "CHOMPI  ", 8);
            std::memcpy(&response[16], "SD STORAGE      ", 16);
            std::memcpy(&response[32], "1.0 ", 4);
            SendResponse(device, 36);
            break;

        case 0x1A: // MODE SENSE(6)
            std::memset(response, 0, 4);
            response[0] = 3;
            SendResponse(device, 4);
            break;

        case 0x1B: // START STOP UNIT
            if((cdb[4] & 0x03) == 0x02)
                eject_seen = true;
            SendCsw(device);
            break;

        case 0x1E: // PREVENT ALLOW MEDIUM REMOVAL
        case 0x2F: // VERIFY(10)
        case 0x35: // SYNCHRONIZE CACHE(10)
            SendCsw(device);
            break;

        case 0x23: // READ FORMAT CAPACITIES
            if(!media_ready)
            {
                FailCommand(device, 0x02, 0x3A);
                break;
            }
            std::memset(response, 0, 12);
            response[3] = 8;
            WriteBe32(&response[4], media_blocks);
            response[8] = 2;
            response[10] = 2;
            SendResponse(device, 12);
            break;

        case 0x25: // READ CAPACITY(10)
            if(!media_ready || media_blocks == 0)
            {
                FailCommand(device, 0x02, 0x3A);
                break;
            }
            WriteBe32(&response[0], media_blocks - 1);
            WriteBe32(&response[4], kBlockSize);
            SendResponse(device, 8);
            break;

        case 0x28: // READ(10)
            BeginRead(device, ReadBe32(&cdb[2]), ReadBe16(&cdb[7]));
            break;

        case 0x2A: // WRITE(10)
            BeginWrite(device, ReadBe32(&cdb[2]), ReadBe16(&cdb[7]));
            break;

        case 0x5A: // MODE SENSE(10)
            std::memset(response, 0, 8);
            response[1] = 6;
            SendResponse(device, 8);
            break;

        case 0x9E: // READ CAPACITY(16)
            if((cdb[1] & 0x1F) != 0x10 || !media_ready || media_blocks == 0)
            {
                FailCommand(device, 0x05, 0x20);
                break;
            }
            std::memset(response, 0, 16);
            WriteBe32(&response[4], media_blocks - 1);
            WriteBe32(&response[8], kBlockSize);
            SendResponse(device, 16);
            break;

        case 0xA0: // REPORT LUNS
            std::memset(response, 0, 8);
            response[3] = 8;
            SendResponse(device, 8);
            break;

        default:
            FailCommand(device, 0x05, 0x20);
            break;
    }
}

uint8_t MscInit(USBD_HandleTypeDef* device, uint8_t)
{
    USBD_LL_OpenEP(device, kBulkInEp, USBD_EP_TYPE_BULK, kEndpointSize);
    USBD_LL_OpenEP(device, kBulkOutEp, USBD_EP_TYPE_BULK, kEndpointSize);
    device->ep_in[kBulkInEp & 0x0F].is_used = 1U;
    device->ep_out[kBulkOutEp & 0x0F].is_used = 1U;
    TraceEvent(0xF3, 0, 0);
    ArmCbw(device);
    return USBD_OK;
}

uint8_t MscDeInit(USBD_HandleTypeDef* device, uint8_t)
{
    USBD_LL_CloseEP(device, kBulkInEp);
    USBD_LL_CloseEP(device, kBulkOutEp);
    device->ep_in[kBulkInEp & 0x0F].is_used = 0U;
    device->ep_out[kBulkOutEp & 0x0F].is_used = 0U;
    bot_state = BotState::AwaitingCbw;
    io_pending = false;
    return USBD_OK;
}

uint8_t MscSetup(USBD_HandleTypeDef* device, USBD_SetupReqTypedef* request)
{
    if((request->bmRequest & 0x60) != 0x20)
    {
        USBD_CtlError(device, request);
        return USBD_FAIL;
    }

    if(request->bRequest == 0xFE && (request->bmRequest & 0x80) != 0
       && request->wIndex == 0 && request->wLength == 1)
    {
        static uint8_t max_lun = 0;
        TraceEvent(0xF2, 0, 0);
        USBD_CtlSendData(device, &max_lun, sizeof(max_lun));
        return USBD_OK;
    }

    if(request->bRequest == 0xFF && (request->bmRequest & 0x80) == 0
       && request->wIndex == 0)
    {
        TraceEvent(0xF1, 0, 0);
        USBD_LL_FlushEP(device, kBulkInEp);
        USBD_LL_FlushEP(device, kBulkOutEp);
        USBD_LL_ClearStallEP(device, kBulkInEp);
        USBD_LL_ClearStallEP(device, kBulkOutEp);
        command_status = 0;
        residue = 0;
        io_pending = false;
        USBD_CtlSendStatus(device);
        ArmCbw(device);
        return USBD_OK;
    }

    USBD_CtlError(device, request);
    return USBD_FAIL;
}

uint8_t MscDataIn(USBD_HandleTypeDef* device, uint8_t endpoint)
{
    if(endpoint != (kBulkInEp & 0x7F))
        return USBD_OK;

    switch(bot_state)
    {
        case BotState::SendingResponse: SendCsw(device); break;
        case BotState::Padding: SendPad(device); break;
        case BotState::ReadingBlocks:
            if(blocks_left == 0)
                SendCsw(device);
            else
                io_pending = true;
            break;
        case BotState::SendingCsw: ArmCbw(device); break;
        default: break;
    }
    return USBD_OK;
}

uint8_t MscDataOut(USBD_HandleTypeDef* device, uint8_t endpoint)
{
    if(endpoint != (kBulkOutEp & 0x7F))
        return USBD_OK;

    if(bot_state == BotState::AwaitingCbw)
    {
        if(USBD_LL_GetRxDataSize(device, kBulkOutEp) != sizeof(cbw))
        {
            USBD_LL_StallEP(device, kBulkInEp);
            USBD_LL_StallEP(device, kBulkOutEp);
            return USBD_FAIL;
        }
        HandleCommand(device);
    }
    else if(bot_state == BotState::WritingBlocks)
    {
        if(USBD_LL_GetRxDataSize(device, kBulkOutEp)
           != chunk_blocks * kBlockSize)
        {
            SetSense(0x05, 0x24);
            command_status = 1;
            SendCsw(device);
            return USBD_FAIL;
        }
        io_pending = true;
    }
    return USBD_OK;
}

uint8_t MscEp0TxSent(USBD_HandleTypeDef*) { return USBD_OK; }
uint8_t MscEp0RxReady(USBD_HandleTypeDef*) { return USBD_OK; }
uint8_t MscSof(USBD_HandleTypeDef*) { return USBD_OK; }
uint8_t MscIsoIn(USBD_HandleTypeDef*, uint8_t) { return USBD_OK; }
uint8_t MscIsoOut(USBD_HandleTypeDef*, uint8_t) { return USBD_OK; }

uint8_t* GetConfigDescriptor(uint16_t* length)
{
    *length = sizeof(config_descriptor);
    return config_descriptor;
}

uint8_t* GetOtherSpeedConfigDescriptor(uint16_t* length)
{
    std::memcpy(other_speed_descriptor, config_descriptor,
                sizeof(config_descriptor));
    other_speed_descriptor[1] = 0x07;
    *length = sizeof(other_speed_descriptor);
    return other_speed_descriptor;
}

uint8_t* GetQualifierDescriptor(uint16_t* length)
{
    *length = sizeof(qualifier_descriptor);
    return qualifier_descriptor;
}

uint8_t* GetStringDescriptor(const char* text, uint16_t* length)
{
    static uint8_t descriptor[64];
    const size_t text_length = std::strlen(text);
    descriptor[0] = static_cast<uint8_t>(2 + text_length * 2);
    descriptor[1] = 0x03;
    for(size_t index = 0; index < text_length; ++index)
    {
        descriptor[2 + index * 2] = static_cast<uint8_t>(text[index]);
        descriptor[3 + index * 2] = 0;
    }
    *length = descriptor[0];
    return descriptor;
}

uint8_t* GetDeviceDescriptor(USBD_SpeedTypeDef, uint16_t* length)
{
    *length = sizeof(device_descriptor);
    return device_descriptor;
}

uint8_t* GetLangId(USBD_SpeedTypeDef, uint16_t* length)
{
    static uint8_t descriptor[] = {4, 0x03, 0x09, 0x04};
    *length = sizeof(descriptor);
    return descriptor;
}

uint8_t* GetManufacturer(USBD_SpeedTypeDef, uint16_t* length)
{
    return GetStringDescriptor("CHOMPI", length);
}

uint8_t* GetProduct(USBD_SpeedTypeDef, uint16_t* length)
{
    return GetStringDescriptor("CHOMPI", length);
}

uint8_t* GetSerial(USBD_SpeedTypeDef, uint16_t* length)
{
    return GetStringDescriptor("CHOMPI000001", length);
}

uint8_t* GetConfigurationString(USBD_SpeedTypeDef, uint16_t* length)
{
    return GetStringDescriptor("SD Card Storage", length);
}

uint8_t* GetInterfaceString(USBD_SpeedTypeDef, uint16_t* length)
{
    return GetStringDescriptor("Mass Storage", length);
}

uint8_t* GetUserString(USBD_HandleTypeDef*, uint8_t, uint16_t* length)
{
    *length = 0;
    return nullptr;
}

USBD_DescriptorsTypeDef descriptors = {
    GetDeviceDescriptor,
    GetLangId,
    GetManufacturer,
    GetProduct,
    GetSerial,
    GetConfigurationString,
    GetInterfaceString,
};

USBD_ClassTypeDef msc_class = {
    MscInit,
    MscDeInit,
    MscSetup,
    MscEp0TxSent,
    MscEp0RxReady,
    MscDataIn,
    MscDataOut,
    MscSof,
    MscIsoIn,
    MscIsoOut,
    GetConfigDescriptor,
    GetConfigDescriptor,
    GetOtherSpeedConfigDescriptor,
    GetQualifierDescriptor,
    GetUserString,
};
}

void UsbMscSetMedia(uint32_t block_count, bool ready)
{
    media_blocks = block_count;
    media_ready = ready;
}

UsbMscInitResult UsbMscInit()
{
    HAL_PWREx_EnableUSBVoltageDetector();
    if(USBD_Init(&usb_device, &descriptors, DEVICE_HS) != USBD_OK)
        return UsbMscInitResult::CoreInitFailed;
    if(USBD_RegisterClass(&usb_device, &msc_class) != USBD_OK)
    {
        USBD_DeInit(&usb_device);
        return UsbMscInitResult::ClassRegistrationFailed;
    }
    if(USBD_Start(&usb_device) != USBD_OK)
    {
        USBD_DeInit(&usb_device);
        return UsbMscInitResult::DeviceStartFailed;
    }
    return UsbMscInitResult::Ready;
}

uint32_t UsbMscTraceCount() { return trace_count; }

bool UsbMscFormatTrace(uint32_t seq, char* line, size_t size)
{
    const uint32_t count = trace_count;
    if(seq >= count || count - seq > kTraceSize)
        return false;

    const TraceEntry entry = trace[seq % kTraceSize];
    if(entry.op >= 0xF0)
        std::snprintf(line, size, "T%lu EVT=%02X A=%lu B=%lu\r\n",
                      static_cast<unsigned long>(seq), entry.op,
                      static_cast<unsigned long>(entry.lba),
                      static_cast<unsigned long>(entry.length));
    else
        std::snprintf(line, size,
                      "T%lu OP=%02X LBA=%lu LEN=%lu ST=%02X KEY=%02X ASC=%02X "
                      "SDERR=%08lX\r\n",
                      static_cast<unsigned long>(seq), entry.op,
                      static_cast<unsigned long>(entry.lba),
                      static_cast<unsigned long>(entry.length), entry.status,
                      entry.key, entry.asc,
                      static_cast<unsigned long>(entry.sd_error));
    return true;
}

bool UsbMscBusIdle()
{
    return eject_seen || usb_device.dev_state == USBD_STATE_SUSPENDED;
}

/** The host ejected the drive (START STOP UNIT, LoEj set, Start clear). */
bool UsbMscEjected() { return eject_seen; }

void UsbMscStop() { USBD_Stop(&usb_device); }

void UsbMscProcess()
{
    if(!io_pending)
        return;

    io_pending = false;
    if(bot_state == BotState::ReadingBlocks)
    {
        ProcessReadBlock(&usb_device);
    }
    else if(bot_state == BotState::WritingBlocks)
    {
        if(disk_write(0, sector, current_lba, chunk_blocks) != RES_OK)
        {
            SetSense(0x03, 0x0C);
            command_status = 1;
            SendCsw(&usb_device);
            return;
        }

        current_lba += chunk_blocks;
        blocks_left -= chunk_blocks;
        residue -= chunk_blocks * kBlockSize;
        ArmNextWriteBlock(&usb_device);
    }
}
