#pragma once
#include "SharedMemoryLayout.h"
#include <Windows.h>

/**
 * @brief Shared control-command ring read by host process.
 */
class SharedControlBuffer
{
public:
	/**
	 * @brief Creates/opens control ring mapping and ready event.
	 * @return true when initialization succeeded.
	 */
	bool Init()
	{
		m_hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
			PAGE_READWRITE, 0, sizeof(ControlRingBuffer), SHM_CONTROL_NAME);
		if (!m_hMap)
			return false;

		m_pRing = reinterpret_cast<ControlRingBuffer*>(
			MapViewOfFile(m_hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(ControlRingBuffer)));
		if (!m_pRing)
			return false;

		m_hEvent = CreateEventW(nullptr, FALSE, FALSE, EVT_CONTROL_READY);
		if (!m_hEvent)
			return false;

		new (&m_pRing->write_index) std::atomic<uint32_t>(0);
		new (&m_pRing->read_index) std::atomic<uint32_t>(0);
		m_pRing->capacity = CONTROL_RING_CAPACITY;
		m_pRing->reserved = 0;
		return true;
	}

	/**
	 * @brief Pops one control event from ring.
	 * @param out_event Output event when available.
	 * @return false when ring is empty or not initialized.
	 */
	bool ReadEvent(ControlEvent& out_event)
	{
		if (!m_pRing)
			return false;

		const uint32_t r = m_pRing->read_index.load(std::memory_order_acquire);
		const uint32_t w = m_pRing->write_index.load(std::memory_order_acquire);
		if (r == w)
			return false;

		out_event = m_pRing->events[r % CONTROL_RING_CAPACITY];
		m_pRing->read_index.store(r + 1, std::memory_order_release);
		return true;
	}

	/** @brief Returns control-ready event handle. */
	HANDLE GetEvent() const { return m_hEvent; }

	/** @brief Releases mapping and event handles. */
	void Shutdown()
	{
		if (m_pRing)
		{
			UnmapViewOfFile(m_pRing);
			m_pRing = nullptr;
		}
		if (m_hMap)
		{
			CloseHandle(m_hMap);
			m_hMap = nullptr;
		}
		if (m_hEvent)
		{
			CloseHandle(m_hEvent);
			m_hEvent = nullptr;
		}
	}

	~SharedControlBuffer() { Shutdown(); }

private:
	HANDLE m_hMap = nullptr;
	HANDLE m_hEvent = nullptr;
	ControlRingBuffer* m_pRing = nullptr;
};
