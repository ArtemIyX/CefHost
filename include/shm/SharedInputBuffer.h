#pragma once
#include "SharedMemoryLayout.h"
#include <Windows.h>
#include <cstring>

// Shared ring buffer for input events sent from consumer to host.
class SharedInputBuffer
{
public:
    bool Init()
    {
        m_hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
            PAGE_READWRITE, 0, sizeof(InputRingBuffer), SHM_INPUT_NAME);
        if (!m_hMap) return false;

        m_pRing = reinterpret_cast<InputRingBuffer*>(
            MapViewOfFile(m_hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(InputRingBuffer)));
        if (!m_pRing) return false;

        m_hEvent = CreateEventW(nullptr, FALSE, FALSE, EVT_INPUT_READY);
        if (!m_hEvent) return false;

        // Placement-initialize atomics inside the SHM region.
        new (&m_pRing->write_index) std::atomic<uint32_t>(0);
        new (&m_pRing->read_index)  std::atomic<uint32_t>(0);
        m_pRing->capacity = INPUT_RING_CAPACITY;
        m_pRing->reserved = 0;
        return true;
    }

    // Returns false if ring is empty.
    bool ReadEvent(InputEvent& out_event)
    {
        if (!m_pRing) return false;

        const uint32_t r = m_pRing->read_index.load(std::memory_order_acquire);
        const uint32_t w = m_pRing->write_index.load(std::memory_order_acquire);
        if (r == w) return false;

        out_event = m_pRing->events[r % INPUT_RING_CAPACITY];
        m_pRing->read_index.store(r + 1, std::memory_order_release);
        return true;
    }

    bool HasPendingEvents() const
    {
        if (!m_pRing) return false;
        return m_pRing->read_index.load(std::memory_order_acquire) !=
               m_pRing->write_index.load(std::memory_order_acquire);
    }

    HANDLE GetEvent() const { return m_hEvent; }

    void Shutdown()
    {
        if (m_pRing) { UnmapViewOfFile(m_pRing);  m_pRing = nullptr; }
        if (m_hMap) { CloseHandle(m_hMap);        m_hMap = nullptr; }
        if (m_hEvent) { CloseHandle(m_hEvent);      m_hEvent = nullptr; }
    }

    ~SharedInputBuffer() { Shutdown(); }

private:
    HANDLE           m_hMap = nullptr;
    HANDLE           m_hEvent = nullptr;
    InputRingBuffer* m_pRing = nullptr;
};
