#pragma once
#include "SharedMemoryLayout.h"
#include <Windows.h>
#include <cstring>

class FrameBuffer
{
public:
    bool Init()
    {
        m_hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
            PAGE_READWRITE, 0, SHM_FRAME_TOTAL, SHM_FRAME_NAME);
        if (!m_hMap) return false;

        m_pData = MapViewOfFile(m_hMap, FILE_MAP_WRITE, 0, 0, SHM_FRAME_TOTAL);
        if (!m_pData) return false;

        m_hEvent = CreateEventW(nullptr, FALSE, FALSE, EVT_FRAME_READY);
        if (!m_hEvent) return false;

        memset(m_pData, 0, SHM_FRAME_TOTAL);
        return true;
    }

    void WriteFrame(uint32_t width, uint32_t height, const void* bgra_data, size_t data_size)
    {
        if (!m_pData) return;

        FrameHeader* header = reinterpret_cast<FrameHeader*>(m_pData);

        // Write into the slot that is NOT currently being read.
        const uint32_t writeSlot = 1u - (header->write_slot & 1u);

        uint8_t* pixels = reinterpret_cast<uint8_t*>(m_pData) + sizeof(FrameHeader)
            + static_cast<size_t>(writeSlot) * SHM_FRAME_SIZE;
        memcpy(pixels, bgra_data, data_size);

        // Commit: update dimensions and flip slot atomically-ish via sequence bump.
        // Reader always checks sequence first, then reads write_slot.
        header->width = width;
        header->height = height;
        header->write_slot = writeSlot;
        _WriteBarrier();          // MSVC: prevent reorder before slot/dims are visible
        header->sequence++;

        SetEvent(m_hEvent);
    }

    void Shutdown()
    {
        if (m_pData) { UnmapViewOfFile(m_pData);  m_pData = nullptr; }
        if (m_hMap) { CloseHandle(m_hMap);        m_hMap = nullptr; }
        if (m_hEvent) { CloseHandle(m_hEvent);      m_hEvent = nullptr; }
    }

    ~FrameBuffer() { Shutdown(); }

    FrameHeader* GetHeader() const
    {
        return reinterpret_cast<FrameHeader*>(m_pData);
    }
    HANDLE GetEvent() const {return m_hEvent;}
private:
    HANDLE m_hMap = nullptr;
    HANDLE m_hEvent = nullptr;
    void* m_pData = nullptr;
};