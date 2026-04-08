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

// Mirrors cef_cursor_type_t values we care about.
// Values match CEF enum so casting is safe.
enum class CefCursorType : uint8_t
{
    CT_POINTER,
    CT_CROSS,
    CT_HAND,
    CT_IBEAM,
    CT_WAIT,
    CT_HELP,
    CT_EASTRESIZE,
    CT_NORTHRESIZE,
    CT_NORTHEASTRESIZE,
    CT_NORTHWESTRESIZE,
    CT_SOUTHRESIZE,
    CT_SOUTHEASTRESIZE,
    CT_SOUTHWESTRESIZE,
    CT_WESTRESIZE,
    CT_NORTHSOUTHRESIZE,
    CT_EASTWESTRESIZE,
    CT_NORTHEASTSOUTHWESTRESIZE,
    CT_NORTHWESTSOUTHEASTRESIZE,
    CT_COLUMNRESIZE,
    CT_ROWRESIZE,
    CT_MIDDLEPANNING,
    CT_EASTPANNING,
    CT_NORTHPANNING,
    CT_NORTHEASTPANNING,
    CT_NORTHWESTPANNING,
    CT_SOUTHPANNING,
    CT_SOUTHEASTPANNING,
    CT_SOUTHWESTPANNING,
    CT_WESTPANNING,
    CT_MOVE,
    CT_VERTICALTEXT,
    CT_CELL,
    CT_CONTEXTMENU,
    CT_ALIAS,
    CT_PROGRESS,
    CT_NODROP,
    CT_COPY,
    CT_NONE,
    CT_NOTALLOWED,
    CT_ZOOMIN,
    CT_ZOOMOUT,
    CT_GRAB,
    CT_GRABBING,
    CT_MIDDLE_PANNING_VERTICAL,
    CT_MIDDLE_PANNING_HORIZONTAL,
    CT_CUSTOM,
    CT_DND_NONE,
    CT_DND_MOVE,
    CT_DND_COPY,
    CT_DND_LINK,
    CT_NUM_VALUES,
};

struct FrameHeader
{
    uint32_t      width;
    uint32_t      height;
    uint32_t      sequence;    // incremented each frame
    uint32_t      write_slot;  // 0 or 1 - which pixel buffer holds the latest complete frame
    CefCursorType cursor_type; // updated by OnCursorChange, read by UE5
    uint8_t       reserved[3];
};

// Layout: [FrameHeader][pixel buffer 0][pixel buffer 1]
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