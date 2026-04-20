#pragma once
#include <atomic>
#include <cstdint>

/** @brief Maximum supported shared frame width. */
constexpr uint32_t SHM_MAX_WIDTH = 3840;
/** @brief Maximum supported shared frame height. */
constexpr uint32_t SHM_MAX_HEIGHT = 2160;
/** @brief BGRA byte size for one max-size frame. */
constexpr uint32_t SHM_FRAME_SIZE = SHM_MAX_WIDTH * SHM_MAX_HEIGHT * 4;
/** @brief Number of shared frame ring slots. */
constexpr uint32_t SHM_FRAME_SLOT_COUNT = 3;
/** @brief Shared protocol version used by host/consumer. */
constexpr uint32_t SHM_PROTOCOL_VERSION = 4;
/** @brief Shared protocol magic marker ('CEFH'). */
constexpr uint32_t SHM_PROTOCOL_MAGIC = 0x43454648; // 'CEFH'

/** @brief Input ring capacity in events. */
constexpr uint32_t INPUT_RING_CAPACITY = 256;
/** @brief Control ring capacity in events. */
constexpr uint32_t CONTROL_RING_CAPACITY = 64;
/** @brief Console ring capacity in events. */
constexpr uint32_t CONSOLE_RING_CAPACITY = 256;
/** @brief Max UTF-16 console message length. */
constexpr uint32_t CONSOLE_MESSAGE_MAX = 1024;
/** @brief Max UTF-16 console source length. */
constexpr uint32_t CONSOLE_SOURCE_MAX = 256;

/** @brief Shared mapping name for frame channel. */
constexpr const wchar_t* SHM_FRAME_NAME = L"CEFHost_Frame";
/** @brief Shared mapping name for input channel. */
constexpr const wchar_t* SHM_INPUT_NAME = L"CEFHost_Input";
/** @brief Auto-reset event name for frame-ready notifications. */
constexpr const wchar_t* EVT_FRAME_READY = L"CEFHost_FrameReady";
/** @brief Auto-reset event name for input-ready notifications. */
constexpr const wchar_t* EVT_INPUT_READY = L"CEFHost_InputReady";
/** @brief Shared mapping name for control channel. */
constexpr const wchar_t* SHM_CONTROL_NAME = L"CEFHost_Control";
/** @brief Auto-reset event name for control-ready notifications. */
constexpr const wchar_t* EVT_CONTROL_READY = L"CEFHost_ControlReady";
/** @brief Shared mapping name for console channel. */
constexpr const wchar_t* SHM_CONSOLE_NAME = L"CEFHost_Console";
/** @brief Auto-reset event name for console-ready notifications. */
constexpr const wchar_t* EVT_CONSOLE_READY = L"CEFHost_ConsoleReady";
/** @brief Global shutdown event name used for cross-process stop signals. */
constexpr const wchar_t* EVT_SHUTDOWN = L"CEFHost_Shutdown";
/** @brief Named shared GPU fence object. */
constexpr const wchar_t* SHM_GPU_FENCE_NAME = L"Global\\CEFHost_SharedFence";

/** @brief Cursor values mirrored from CEF cursor types. */
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

/** @brief Main-frame load state published to consumer. */
enum class CefLoadState : uint8_t
{
	Idle = 0,
	Loading = 1,
	Ready = 2,
	Error = 3,
};

/** @brief Maximum number of dirty rects published in one frame. */
constexpr uint32_t MAX_DIRTY_RECTS = 16;

/** @brief Rectangle in frame pixel space. */
struct DirtyRect
{
	int32_t x, y, w, h;
};

/** @brief Per-frame publish flags in FrameHeader::flags. */
enum FrameFlags : uint32_t
{
	FRAME_FLAG_NONE = 0,
	FRAME_FLAG_FULL_FRAME = 1u << 0,
	FRAME_FLAG_DIRTY_ONLY = 1u << 1,
	FRAME_FLAG_OVERFLOW = 1u << 2,
	FRAME_FLAG_RESIZED = 1u << 3,
	FRAME_FLAG_POPUP_PLANE = 1u << 4,
};

/**
 * @brief Shared frame metadata header.
 *
 * Written by producer before incrementing sequence and signaling EVT_FRAME_READY.
 */
struct FrameHeader
{
	uint32_t protocol_magic;
	uint32_t version;
	uint32_t slot_count;
	uint32_t width;
	uint32_t height;
	uint64_t frame_id;
	uint64_t present_id;
	uint64_t gpu_fence_value;
	uint32_t sequence;
	uint32_t write_slot;
	uint32_t flags;
	CefCursorType cursor_type;
	CefLoadState load_state;
	uint8_t popup_visible;
	uint8_t dirty_count; // 0 = full frame
	uint8_t reserved[2];
	DirtyRect popup_rect;
	DirtyRect dirty_rects[MAX_DIRTY_RECTS];
};
/** @brief Total legacy frame mapping byte size (header + CPU pixel ring). */
// Pixel buffers kept for layout compat but unused in GPU path.
constexpr uint32_t SHM_FRAME_TOTAL = sizeof(FrameHeader) + SHM_FRAME_SIZE * SHM_FRAME_SLOT_COUNT;

/** @brief Input event kinds sent from consumer to host. */
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

/** @brief Union payload for one input event in shared input ring. */
struct InputEvent
{
	InputEventType type;
	uint8_t reserved[3];
	union
	{
		struct
		{
			int32_t x, y;
			uint8_t button;
		} mouse;
		struct
		{
			int32_t x, y;
			float delta_x, delta_y;
		} scroll;
		struct
		{
			uint32_t windows_key_code;
			uint32_t modifiers;
		} key;
		struct
		{
			uint16_t character;
		} char_event;
	};
};

/** @brief Single-producer/single-consumer style input ring in shared memory. */
struct InputRingBuffer
{
	std::atomic<uint32_t> write_index;
	std::atomic<uint32_t> read_index;
	uint32_t capacity;
	uint32_t reserved;
	InputEvent events[INPUT_RING_CAPACITY];
};

/** @brief Control command kinds sent from consumer to host. */
enum class ControlEventType : uint8_t
{
	GoBack = 0,
	GoForward = 1,
	StopLoad = 2,
	Reload = 3,
	SetURL = 4,
	SetPaused = 5,
	SetHidden = 6,
	SetFocus = 7,
	SetZoomLevel = 8,
	SetFrameRate = 9,
	ScrollTo = 10,
	Resize = 11,
	SetMuted = 12,
	OpenDevTools = 13,
	CloseDevTools = 14,
	SetInputEnabled = 15,
	ExecuteJS = 16,
	ClearCookies = 17,
	SetConsumerCadenceUs = 18,
	SetMaxInFlightBeginFrames = 19,
	SetFlushIntervalFrames = 20,
	SetKeyframeIntervalUs = 21,
	OpenLocalFile = 22,
	LoadHtmlString = 23,
};

/** @brief Max UTF-16 payload length for string-based control commands. */
constexpr uint32_t CONTROL_STRING_MAX = 2048;

/** @brief Union payload for one control event in shared control ring. */
struct ControlEvent
{
	ControlEventType type;
	uint8_t reserved[3];
	union
	{
		struct
		{
			uint32_t width;
			uint32_t height;
		} resize;
		struct
		{
			int32_t x;
			int32_t y;
		} scroll;
		struct
		{
			float value;
		} zoom;
		struct
		{
			uint32_t value;
		} frame_rate;
		struct
		{
			uint32_t value;
		} cadence_us;
		struct
		{
			bool value;
		} flag;
		struct
		{
			char16_t text[CONTROL_STRING_MAX];
		} string;
	};
};

/** @brief Single-producer/single-consumer style control ring in shared memory. */
struct ControlRingBuffer
{
	std::atomic<uint32_t> write_index;
	std::atomic<uint32_t> read_index;
	uint32_t capacity;
	uint32_t reserved;
	ControlEvent events[CONTROL_RING_CAPACITY];
};

/** @brief Console severity levels mirrored from CEF log severities. */
enum class ConsoleLogLevel : uint8_t
{
	Log = 0,
	Warning = 1,
	Error = 2,
};

/** @brief One console message event in shared console ring. */
struct ConsoleLogEvent
{
	ConsoleLogLevel level;
	uint8_t reserved[3];
	int32_t line;
	char16_t source[CONSOLE_SOURCE_MAX];
	char16_t message[CONSOLE_MESSAGE_MAX];
};

/** @brief Single-producer/single-consumer style console ring in shared memory. */
struct ConsoleRingBuffer
{
	std::atomic<uint32_t> write_index;
	std::atomic<uint32_t> read_index;
	uint32_t capacity;
	uint32_t reserved;
	ConsoleLogEvent events[CONSOLE_RING_CAPACITY];
};
