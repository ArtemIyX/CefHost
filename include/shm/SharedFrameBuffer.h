#pragma once
#include "SharedMemoryLayout.h"
#include <Windows.h>
#include <cstring>
#include <atomic>

/**
 * @brief Shared frame-metadata region and ready-event owner.
 *
 * Producer writes FrameHeader metadata (and optional CPU pixel payload) and
 * signals EVT_FRAME_READY to notify consumer.
 */
class SharedFrameBuffer
{
public:
	/**
     * @brief Creates/opens shared frame mapping and ready event.
     * @return true when mapping and event are available.
     */
	bool Init()
	{
		m_hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
			PAGE_READWRITE, 0, SHM_FRAME_TOTAL, SHM_FRAME_NAME);
		if (!m_hMap)
			return false;

		m_pData = MapViewOfFile(m_hMap, FILE_MAP_WRITE, 0, 0, SHM_FRAME_TOTAL);
		if (!m_pData)
			return false;

		m_hEvent = CreateEventW(nullptr, FALSE, FALSE, EVT_FRAME_READY);
		if (!m_hEvent)
			return false;

		memset(m_pData, 0, SHM_FRAME_TOTAL);
		return true;
	}

	/**
     * @brief Writes a CPU BGRA frame into ring slot and publishes metadata.
     * @param width Frame width.
     * @param height Frame height.
     * @param bgra_data Pixel source pointer.
     * @param data_size Source size in bytes.
     */
	void WriteFrame(uint32_t width, uint32_t height, const void* bgra_data, size_t data_size)
	{
		if (!m_pData)
			return;

		FrameHeader* header = reinterpret_cast<FrameHeader*>(m_pData);

		uint32_t slotCount = header->slot_count;
		if (slotCount == 0 || slotCount > SHM_FRAME_SLOT_COUNT)
			slotCount = SHM_FRAME_SLOT_COUNT;

		const uint32_t writeSlot = (header->write_slot + 1u) % slotCount;

		uint8_t* pixels = reinterpret_cast<uint8_t*>(m_pData) + sizeof(FrameHeader)
			+ static_cast<size_t>(writeSlot) * SHM_FRAME_SIZE;
		memcpy(pixels, bgra_data, data_size);

		// Commit: update dimensions and slot, then publish frame.
		header->protocol_magic = SHM_PROTOCOL_MAGIC;
		header->version = SHM_PROTOCOL_VERSION;
		header->slot_count = slotCount;
		header->width = width;
		header->height = height;
		header->write_slot = writeSlot;
		std::atomic_thread_fence(std::memory_order_release);
		header->sequence++;
		header->frame_id++;
		header->present_id = header->frame_id;

		SetEvent(m_hEvent);
	}

	/** @brief Releases mapping and event handles. */
	void Shutdown()
	{
		if (m_pData)
		{
			UnmapViewOfFile(m_pData);
			m_pData = nullptr;
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

	~SharedFrameBuffer() { Shutdown(); }

	/** @brief Returns mapped frame header pointer. */
	FrameHeader* GetHeader() const
	{
		return reinterpret_cast<FrameHeader*>(m_pData);
	}
	/** @brief Returns frame-ready event handle. */
	HANDLE GetEvent() const { return m_hEvent; }

private:
	HANDLE m_hMap = nullptr;
	HANDLE m_hEvent = nullptr;
	void* m_pData = nullptr;
};
