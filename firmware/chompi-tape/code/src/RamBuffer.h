#pragma once
#include "daisy_seed.h"
#include "daisysp.h"

namespace daisy
{

    static const size_t kMaxRamBuffSize = 31694848 / 2;
    // total size = 31694848, which goes evenly into 8K, which means SDRAM page alignment

    /** Looper undo: the old single looper allocation is split into two equal
     *  halves in SDRAM -- a live loop half and an undo snapshot half.
     *  The chompi/sampler buffer keeps the full kMaxRamBuffSize.
     */
    static const size_t kMaxLooperRamBuffSize = kMaxRamBuffSize / 2; // int16 elements, even

    /** One undo snapshot page: 256 stereo frames = 512 int16 = 1024 bytes.
     *  kMaxLooperRamBuffSize divides evenly into pages (15476 pages).
     */
    static const size_t kLooperUndoPageSize  = 512; // int16 elements (256 stereo frames, 1 KiB)
    static const size_t kLooperUndoNumPages  = kMaxLooperRamBuffSize / kLooperUndoPageSize;
    static_assert(kMaxLooperRamBuffSize % kLooperUndoPageSize == 0,
                  "looper half must divide evenly into undo pages");

    // just a buffer with its length
    struct RamBufferMemory
    {
        // pointer to array of size capacity (defaults to the full sampler capacity)
        void Init(int16_t* m, size_t cap = kMaxRamBuffSize)
        {
            mem = m;
            capacity = cap;
        }

        int16_t* mem;
        size_t length;
        size_t capacity = kMaxRamBuffSize;

        void Clear()
        {
            std::fill(&mem[0], &mem[capacity], 0);
        }
    };

    // keeps track of its own read and write heads
    class RamBuffer
    {
    public:
        void Init(RamBufferMemory* b)
        {
            buff = b;
            buff->Clear();
        }

        void Reset()
        {
            buff->length = read_head = write_head = 0;
        }

        void BlockRead(int16_t* copy_buff, size_t size)
        {
            if(read_head + size > buff->capacity)
                size = buff->capacity - read_head;
            if(read_head + size > buff->length)
                size = buff->length - read_head;

            std::copy(&buff->mem[read_head], &buff->mem[read_head + size], copy_buff);
            read_head += size;
        }

        void StereoRead(int16_t* l, int16_t* r, bool rev)
        {
            const size_t new_read_head = read_head + 2;
            if(!rev && new_read_head <= buff->length && new_read_head <= buff->capacity)
            {
                *l = buff->mem[read_head];
                *r = buff->mem[read_head + 1];
                read_head = new_read_head;
            }
            else if(rev && read_head >= 2)
            {
                *l = buff->mem[read_head - 2];
                *r = buff->mem[read_head - 1];
                read_head -= 2;
            }
            else
            {
                *l = *r = 0;
            }
        }

        void BlockWrite(int16_t* copy_buff, size_t size)
        {
            if(write_head + size > buff->capacity)
                size = buff->capacity - write_head;

            std::copy(copy_buff, &copy_buff[size], &buff->mem[write_head]);
            write_head += size;
            buff->length = write_head;
            buff->length -= buff->length % 2;
        }

        void StereoWrite(int16_t l, int16_t r, bool rev, bool set_len)
        {
            if(!rev)
            {
                const size_t new_write_head = write_head + 2;
                if(new_write_head <= buff->capacity)
                {
                    buff->mem[write_head] = l;
                    buff->mem[write_head + 1] = r;
                    write_head = new_write_head;
                    if(set_len)
                        buff->length = write_head;
                }
            }
            else if(write_head >= 2)
            {
                buff->mem[write_head - 2] = l;
                buff->mem[write_head - 1] = r;
                write_head -= 2;
            }
        }

        inline size_t GetSize() { return buff->length; }

        inline int16_t Peek(size_t idx) const
        {
            return buff->mem[idx];
        }

        inline void Poke(size_t idx, int16_t v)
        {
            buff->mem[idx] = v;
        }

        /** raw storage access for page-granular snapshot copies (undo) */
        inline int16_t* RawMem() { return buff->mem; }
        inline size_t   GetCapacity() const { return buff->capacity; }

        inline size_t GetReadHead() { return read_head; }
        inline bool ReadEOF() { return read_head >= buff->length || read_head >= buff->capacity; }
        inline bool ReadLoop(bool rev) { return (rev && read_head == 0) || (!rev && ReadEOF()); }

        void SetReadHead(size_t pos) 
        { 
            if(pos <= buff->capacity && pos <= buff->length)
                read_head = pos;
        }
        
        size_t GetRemainingRead(bool rev)
        {
            if(!rev && !ReadEOF())
                return buff->length - read_head;
            else if(rev)
                return read_head;

            return 0;
        }

        inline size_t GetWriteHead() { return write_head; }
        inline bool WriteEOF() { return write_head >= buff->length || write_head >= buff->capacity; }
        inline bool WriteFullLength() { return write_head >= buff->capacity; }
        inline bool WriteLoop(bool rev) { return (rev && write_head == 0) || (!rev && WriteEOF()); }

        void SetWriteHead(size_t pos) 
        {
            if(pos <= buff->capacity && pos <= buff->length)
                write_head = pos;
        }

        size_t GetRemainingWrite(bool rev)
        {
            if(!rev && !WriteEOF())
                return buff->length - write_head;
            else if(rev)
                return write_head;

            return 0;            
        }

        void AdvanceWrite(bool rev)
        {
            if(rev && write_head >= 2)
                write_head -= 2;
            else if(!rev && write_head + 2 <= buff->length && write_head + 2 < buff->capacity)
                write_head += 2;
        }

    private:
        RamBufferMemory* buff;
        size_t read_head, write_head;
    };
} // namespace daisy