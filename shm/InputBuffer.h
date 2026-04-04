#pragma once
#include "SharedMemoryLayout.h"
#include <Windows.h>
#include <cstring>

class InputBuffer
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

        memset(m_pRing, 0, sizeof(InputRingBuffer));
        m_pRing->capacity = INPUT_RING_CAPACITY;
        return true;
    }

    // Returns false if no event available
    bool ReadEvent(InputEvent& out_event)
    {
        if (!m_pRing) return false;
        if (m_pRing->read_index == m_pRing->write_index) return false;

        out_event = m_pRing->events[m_pRing->read_index % INPUT_RING_CAPACITY];
        m_pRing->read_index++;
        return true;
    }

    HANDLE GetEvent() const { return m_hEvent; }

    void Shutdown()
    {
        if (m_pRing) { UnmapViewOfFile(m_pRing);  m_pRing = nullptr; }
        if (m_hMap) { CloseHandle(m_hMap);        m_hMap = nullptr; }
        if (m_hEvent) { CloseHandle(m_hEvent);      m_hEvent = nullptr; }
    }

    ~InputBuffer() { Shutdown(); }

private:
    HANDLE           m_hMap = nullptr;
    HANDLE           m_hEvent = nullptr;
    InputRingBuffer* m_pRing = nullptr;
};