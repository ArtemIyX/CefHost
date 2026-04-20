#pragma once
#include "SharedMemoryLayout.h"
#include <Windows.h>
#include <atomic>

/**
 * @brief Shared console-log ring written by host and read by consumer.
 */
class SharedConsoleBuffer
{
public:
	/**
	 * @brief Creates/opens console ring mapping and ready event.
	 * @return true when initialization succeeded.
	 */
	bool Init()
	{
		m_hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
			PAGE_READWRITE, 0, sizeof(ConsoleRingBuffer), SHM_CONSOLE_NAME);
		if (!m_hMap) return false;

		m_pRing = reinterpret_cast<ConsoleRingBuffer*>(
			MapViewOfFile(m_hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(ConsoleRingBuffer)));
		if (!m_pRing) return false;

		m_hEvent = CreateEventW(nullptr, FALSE, FALSE, EVT_CONSOLE_READY);
		if (!m_hEvent) return false;

		new (&m_pRing->write_index) std::atomic<uint32_t>(0);
		new (&m_pRing->read_index)  std::atomic<uint32_t>(0);
		m_pRing->capacity = CONSOLE_RING_CAPACITY;
		m_pRing->reserved = 0;
		return true;
	}

	/**
	 * @brief Pushes one console event to ring and signals consumer.
	 * @param inEvent Console payload to store.
	 */
	void WriteEvent(const ConsoleLogEvent& inEvent)
	{
		if (!m_pRing || !m_hEvent) return;

		const uint32_t w = m_pRing->write_index.load(std::memory_order_relaxed);
		const uint32_t r = m_pRing->read_index.load(std::memory_order_acquire);
		if (w - r >= CONSOLE_RING_CAPACITY)
		{
			// Drop oldest to keep newest logs.
			m_pRing->read_index.store(r + 1, std::memory_order_release);
		}

		const uint32_t writeIndex = m_pRing->write_index.load(std::memory_order_relaxed);
		m_pRing->events[writeIndex % CONSOLE_RING_CAPACITY] = inEvent;
		m_pRing->write_index.store(writeIndex + 1, std::memory_order_release);
		SetEvent(m_hEvent);
	}

	/** @brief Returns console-ready event handle. */
	HANDLE GetEvent() const { return m_hEvent; }

	/** @brief Releases mapping and event handles. */
	void Shutdown()
	{
		if (m_pRing) { UnmapViewOfFile(m_pRing);  m_pRing = nullptr; }
		if (m_hMap) { CloseHandle(m_hMap);        m_hMap = nullptr; }
		if (m_hEvent) { CloseHandle(m_hEvent);    m_hEvent = nullptr; }
	}

	~SharedConsoleBuffer() { Shutdown(); }

private:
	HANDLE             m_hMap = nullptr;
	HANDLE             m_hEvent = nullptr;
	ConsoleRingBuffer* m_pRing = nullptr;
};
