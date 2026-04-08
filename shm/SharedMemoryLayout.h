#pragma once
#include <cstdint>
#include <atomic>

constexpr uint32_t SHM_MAX_WIDTH = 3840;
constexpr uint32_t SHM_MAX_HEIGHT = 2160;
constexpr uint32_t SHM_FRAME_SIZE = SHM_MAX_WIDTH * SHM_MAX_HEIGHT * 4; // BGRA

constexpr uint32_t INPUT_RING_CAPACITY = 256;

constexpr const wchar_t* SHM_FRAME_NAME = L"CEFHost_Frame";
constexpr const wchar_t* SHM_INPUT_NAME = L"CEFHost_Input";
constexpr const wchar_t* EVT_FRAME_READY = L"CEFHost_FrameReady";
constexpr const wchar_t* EVT_INPUT_READY = L"CEFHost_InputReady";

#pragma pack(push, 1)

struct FrameHeader
{
    uint32_t width;
    uint32_t height;
    uint32_t sequence;   // incremented each frame
    uint32_t write_slot; // 0 or 1 — which pixel buffer holds the latest complete frame
};

// Layout: [FrameHeader][pixel buffer 0][pixel buffer 1]
// Writer alternates slots; reader consumes write_slot after sequence changes.
constexpr uint32_t SHM_FRAME_TOTAL = sizeof(FrameHeader) + SHM_FRAME_SIZE * 2;

enum class InputEventType : uint8_t
{
    MouseMove = 0,
    MouseDown = 1,
    MouseUp = 2,
    MouseScroll = 3,
    KeyDown = 4,
    KeyUp = 5,
    KeyChar = 6,
};

struct InputEvent
{
    InputEventType type;
    uint8_t        reserved[3];

    union
    {
        struct { int32_t x, y; uint8_t button; }                 mouse;
        struct { int32_t x, y; float delta_x, delta_y; }         scroll;
        struct { uint32_t windows_key_code; uint32_t modifiers; } key;
        struct { uint16_t character; }                            char_event;
    };
};

// std::atomic<uint32_t> is safe for cross-process SHM on x86/x64 (lock-free, same layout).
struct InputRingBuffer
{
    std::atomic<uint32_t> write_index;
    std::atomic<uint32_t> read_index;
    uint32_t              capacity;
    uint32_t              reserved;
    InputEvent            events[INPUT_RING_CAPACITY];
};

#pragma pack(pop)