#pragma once

// PC Link — USB thin-client terminal protocol (CROSSPOINT_PC_LINK).
//
// The device runs a FIXED terminal protocol over the USB-Serial/JTAG CDC port
// (the console `Serial`, HWCDC — no TinyUSB). The host renders; the device only
// blits pixels, refreshes the panel under a wear policy, and streams input
// events back. No host-supplied code ever executes on the device.
//
// Everything here is device-independent (no Arduino/HAL includes) so
// host_tests/pclink builds and exercises the exact codec the firmware runs.
// The whole library is wrapped in CROSSPOINT_PC_LINK: PlatformIO compiles all
// of lib/ into every environment, and flag-off images must not change.
//
// ============================================================================
// WIRE FORMAT (protocol version 1). All multi-byte integers little-endian.
// ============================================================================
//
// Frame (both directions):
//   off size field
//   0   1    magic0 = 'X' (0x58)
//   1   1    magic1 = 'P' (0x50)
//   2   1    ver    = 1
//   3   1    type   (host->device 0x01..0x07, device->host 0x81..0x88)
//   4   2    seq    u16, sender-assigned, wraps
//   6   4    len    u32 payload length (bounded per type, <= MAX_PAYLOAD)
//   10  len  payload
//   10+len 4 crc    u32 CRC-32 (IEEE/zlib: poly 0xEDB88320, init/xorout ~0)
//                   over bytes [0, 10+len) — header AND payload
//
// Resync: receivers scan for "XP", reject impossible headers (ver, unknown
// type, length outside the type's bounds) immediately, and on a CRC mismatch
// rescan from the byte after the rejected magic, so a false "XP" inside log
// text or a corrupted frame costs one frame, never the stream. The device
// shares this port with its log output; the host must skip non-frame bytes.
//
// Host -> device:
//   0x01 HELLO    len 4   u16 host_proto, u16 flags(0)
//                         -> device answers CAPS (the host sends nothing else
//                            until CAPS arrives; CAPS.rx_consumed is its
//                            credit baseline).
//   0x02 BLIT     len >= 12
//                   0 u16 x      multiple of 8
//                   2 u16 y
//                   4 u16 w      multiple of 8, > 0
//                   6 u16 h      > 0; x+w <= 800, y+h <= 480
//                   8 u8  fmt    0 = 1bpp, 1 = 2bpp gray
//                   9 u8  enc    0 = raw, 1 = raw DEFLATE (RFC 1951, no zlib
//                                header — Python zlib.compressobj(wbits=-15))
//                  10 u16 reserved (0)
//                  12 ...  data; DECODED size must equal w*h*bpp/8 exactly
//                 Pixels are PHYSICAL panel coordinates (800x480 landscape,
//                 the same layout CMD:SCREENSHOT dumps), rows top to bottom.
//                 1bpp: MSB = leftmost pixel, bit 1 = white, 0 = black
//                       (the framebuffer's own format), w/8 bytes per row.
//                 2bpp: 4 px per byte, leftmost pixel in bits 7..6, level
//                       0 = black, 1 = dark gray, 2 = light gray, 3 = white,
//                       w/4 bytes per row.
//                 A BLIT only stages pixels; nothing reaches the glass until
//                 a REFRESH. Device answers ACK.
//   0x03 REFRESH  len 4   u8 mode (0 fast, 1 half, 2 full, 3 gray), u8[3] 0
//                         -> ACK now, REFRESH_DONE when the waveform ran.
//   0x04 CLEAR    len 4   u8 color (0 black, 1 white), u8[3] 0. Stages only.
//   0x05 CONFIG   len 8   u16 full_every_n, u16 full_every_s,
//                         u16 min_refresh_gap_ms, u8 input_mask, u8 0
//                         0 in a u16 field = keep the current value. Values
//                         are clamped to the floors below -> ACK (status
//                         CLAMPED if any was), then CONFIG_STATE.
//                         input_mask: bit0 buttons, bit1 touch.
//   0x06 PING     len 4   u32 token -> PONG
//   0x07 BYE      len 0   -> ACK; device shows its status screen again.
//
// Device -> host:
//   0x81 CAPS     len >= 30
//                   0 u16 proto (1)       2 u16 width (800)   4 u16 height (480)
//                   6 u8  formats (bit0 1bpp, bit1 2bpp)
//                   7 u8  encodings (bit0 raw, bit1 deflate)
//                   8 u32 max_payload     12 u32 rx_window
//                  16 u32 rx_consumed (credit baseline, see FLOW CONTROL)
//                  20 u16 full_every_n    22 u16 full_every_s
//                  24 u16 min_refresh_gap_ms
//                  26 u8  input_mask      27 u8 touch (1 = touch events)
//                  28 u8  nbuttons, then nbuttons x u8 button code
//                  .. u8  fw_len, then fw_len bytes of firmware version
//   0x82 ACK      len 8   u16 seq, u8 status, u8 0, u32 rx_consumed
//   0x83 REFRESH_DONE len 8
//                         u16 seq (latest coalesced REFRESH), u8 mode run,
//                         u8 flags (bit0 scrub-promoted to FULL, bit1
//                         coalesced), u32 waveform duration in ms
//   0x84 INPUT    len 12  u8 src (0 button, 1 touch), u8 code, u8 state
//                         (1 press/down, 0 release/up), u8 0, u16 x, u16 y
//                         (touch only, physical panel coords; 0xFFFF when
//                         unknown), u32 t_ms (device millis)
//                         button codes: 0 Back, 1 Confirm, 2 Left, 3 Right,
//                         4 Up, 5 Down, 6 Power (logical, MappedInputManager).
//                         Touch code 0 = contact.
//   0x85 PONG     len 8   u32 token, u32 rx_consumed
//   0x86 ERR      len 4   u16 seq (0xFFFF = frame lost to CRC/header error),
//                         u8 status code, u8 0
//   0x87 CONFIG_STATE len 8  effective config, CONFIG layout
//   0x88 CREDIT   len 4   u32 rx_consumed
//
// ACK/ERR status: 0 OK, 1 CLAMPED, 2 BAD_LENGTH, 3 BAD_RECT, 4 BAD_FORMAT,
// 5 DECODE_FAIL, 6 UNKNOWN_TYPE, 7 NOT_READY (no HELLO yet), 8 CRC.
//
// FLOW CONTROL (mandatory — HWCDC silently drops RX bytes once its queue is
// full, and the device stops draining it for the whole ~0.5-2 s of a blocking
// waveform). rx_consumed is a free-running u32 count of bytes the device has
// pulled out of its receive queue. The host keeps its own u32 count of bytes
// written since HELLO and may have at most rx_window bytes in flight:
//     (sent_since_hello) - (rx_consumed - caps.rx_consumed)  <=  rx_window
// (mod 2^32). The device reports rx_consumed in ACK, PONG and CREDIT; it
// sends a CREDIT whenever it has consumed rx_window/4 bytes since the last
// report, so a single frame larger than the window streams through. Reports
// are cumulative, so a lost one is healed by the next; a host stalled on
// credit for >0.5 s sends PING (control frames may use the CONTROL_SLACK
// bytes the device queue holds beyond rx_window).
//
// WEAR MODEL: the host pushes ONLY ON CHANGE. The device never refreshes on
// its own — no timers pushing pixels. Its only autonomous decision is the
// ghost scrub, which PROMOTES a host-requested FAST refresh to FULL; it never
// adds a waveform. Device-enforced defaults (host-overridable via CONFIG, never
// below the floors):
//   min gap between waveform activations   500 ms  (floor 300 ms)
//   ghost scrub: FULL after 60 FAST refreshes or 30 min since the last clean
//   refresh, whichever first               (floors: 20 refreshes / 5 min)
// REFRESH requests arriving inside the gap are COALESCED, not queued: one
// pending slot, the latest seq wins, and the mode is the strongest requested
// (fast < half < full < gray), so a coalesced FAST never downgrades a pending
// FULL. HALF, FULL and GRAY (whose base pass is HALF) are whole-panel clearing
// waveforms and reset the scrub counters; only FAST accrues toward a scrub.

#ifdef CROSSPOINT_PC_LINK

#include <cstddef>
#include <cstdint>

class InflateReader;  // lib/InflateReader (uzlib); only inflateExact() touches it

namespace pclink {

// --- constants ---------------------------------------------------------------

constexpr uint8_t MAGIC0 = 'X';
constexpr uint8_t MAGIC1 = 'P';
constexpr uint8_t PROTO_VERSION = 1;
constexpr size_t HEADER_SIZE = 10;
constexpr size_t CRC_SIZE = 4;
constexpr size_t FRAME_OVERHEAD = HEADER_SIZE + CRC_SIZE;

constexpr uint16_t PANEL_WIDTH = 800;
constexpr uint16_t PANEL_HEIGHT = 480;
constexpr uint32_t FB_1BPP_BYTES = static_cast<uint32_t>(PANEL_WIDTH) * PANEL_HEIGHT / 8;  // 48,000
constexpr uint32_t FB_2BPP_BYTES = static_cast<uint32_t>(PANEL_WIDTH) * PANEL_HEIGHT / 4;  // 96,000

constexpr size_t BLIT_HEADER_SIZE = 12;
// A full-panel raw 2bpp BLIT is the largest legal payload. A deflate stream
// that would not beat raw must be sent raw.
constexpr uint32_t MAX_PAYLOAD = BLIT_HEADER_SIZE + FB_2BPP_BYTES;  // 96,012
constexpr size_t MAX_FRAME = FRAME_OVERHEAD + MAX_PAYLOAD;

// Device receive queue (HWCDC) and the credit window advertised in CAPS. The
// slack beyond the window absorbs control frames (PING) sent while stalled.
constexpr uint32_t RX_QUEUE_BYTES = 4096;
constexpr uint32_t CONTROL_SLACK = 256;
constexpr uint32_t RX_WINDOW = RX_QUEUE_BYTES - CONTROL_SLACK;  // 3,840

enum class MsgType : uint8_t {
  // host -> device
  Hello = 0x01,
  Blit = 0x02,
  Refresh = 0x03,
  Clear = 0x04,
  Config = 0x05,
  Ping = 0x06,
  Bye = 0x07,
  // device -> host
  Caps = 0x81,
  Ack = 0x82,
  RefreshDone = 0x83,
  Input = 0x84,
  Pong = 0x85,
  Err = 0x86,
  ConfigState = 0x87,
  Credit = 0x88,
};

enum class Status : uint8_t {
  Ok = 0,
  Clamped = 1,
  BadLength = 2,
  BadRect = 3,
  BadFormat = 4,
  DecodeFail = 5,
  UnknownType = 6,
  NotReady = 7,
  Crc = 8,
};

enum class RefreshMode : uint8_t { Fast = 0, Half = 1, Full = 2, Gray = 3 };
enum class PixelFormat : uint8_t { Bpp1 = 0, Bpp2 = 1 };
enum class Encoding : uint8_t { Raw = 0, Deflate = 1 };

constexpr uint16_t SEQ_NONE = 0xFFFF;
constexpr uint16_t COORD_NONE = 0xFFFF;

constexpr uint8_t INPUT_SRC_BUTTON = 0;
constexpr uint8_t INPUT_SRC_TOUCH = 1;
constexpr uint8_t INPUT_MASK_BUTTONS = 0x01;
constexpr uint8_t INPUT_MASK_TOUCH = 0x02;

// Button codes carried in INPUT.code for src=button (logical buttons).
enum class ButtonCode : uint8_t { Back = 0, Confirm = 1, Left = 2, Right = 3, Up = 4, Down = 5, Power = 6 };
constexpr uint8_t BUTTON_COUNT = 7;

// --- little-endian helpers ----------------------------------------------------

inline uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
inline void wr16(uint8_t* p, const uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
inline void wr32(uint8_t* p, const uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

// --- CRC-32 -------------------------------------------------------------------

// zlib-compatible running CRC: crc32Update(0, data, n) == zlib.crc32(data),
// and chaining crc32Update(crc32Update(0, a), b) == crc of a||b. On the device
// this is the ROM's esp_rom_crc32_le (zero flash, table-driven — the same call
// FontDownloadActivity checks manifests with); on the host it is the bitwise
// form of lib/Serialization/CredentialIntegrity.h's crc32.
uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len);

// --- frame encoding -------------------------------------------------------------

// Writes one complete frame into out. Returns the frame size, or 0 if it does
// not fit in outCap or len exceeds MAX_PAYLOAD.
size_t encodeFrame(uint8_t* out, size_t outCap, MsgType type, uint16_t seq, const uint8_t* payload, uint32_t len);

// Payload length bounds per type. A header whose len falls outside them is
// rejected before any payload is buffered (the cheap false-magic filter).
bool payloadLengthValid(MsgType type, uint32_t len);
bool isHostToDevice(MsgType type);
bool isDeviceToHost(MsgType type);

// --- frame parser ---------------------------------------------------------------

struct Frame {
  MsgType type;
  uint16_t seq;
  uint32_t len;
  const uint8_t* payload;  // points into the parser's buffer; valid until release()
};

// Which direction's message types this parser accepts.
enum class Direction : uint8_t { HostToDevice, DeviceToHost };

// Streaming parser over a caller-owned buffer of at least
// FRAME_OVERHEAD + maxPayload bytes (allocated once; the parser never
// allocates). Feed it arbitrary chunks; it stops consuming the moment a frame
// completes so the caller can act on it, then release() it and keep feeding.
class FrameParser {
 public:
  struct Stats {
    uint32_t frames = 0;
    uint32_t crcErrors = 0;
    uint32_t headerRejects = 0;
    uint32_t bytesDiscarded = 0;
  };

  FrameParser(uint8_t* buffer, size_t capacity, uint32_t maxPayload, Direction direction);

  // Consumes up to len bytes and returns how many it took. Returns early
  // (possibly 0) while a completed frame is held.
  size_t feed(const uint8_t* data, size_t len);

  bool hasFrame() const { return ready_; }
  const Frame& frame() const { return frame_; }
  // Drops the held frame. Bytes buffered past it (only possible after a
  // resync) are re-parsed immediately, so check hasFrame() again.
  void release();
  void reset();

  // Bytes the parser can take right now without overshooting the current
  // frame (0 while a frame is held). Useful to size a read.
  size_t wanted() const;

  const Stats& stats() const { return stats_; }
  // Rejected-frame events since the last call (for ERR{SEQ_NONE, CRC}).
  uint32_t takeRejects();

 private:
  void advance();
  void discardToNextMagic();

  uint8_t* buf_;
  size_t cap_;
  uint32_t maxPayload_;
  Direction dir_;
  size_t fill_ = 0;
  bool ready_ = false;
  bool headerOk_ = false;
  uint32_t curLen_ = 0;
  Frame frame_{};
  Stats stats_{};
  uint32_t pendingRejects_ = 0;
};

// --- flow control -----------------------------------------------------------------

// Device side: counts bytes pulled from the receive queue and decides when a
// CREDIT report is due. Wrap-safe (u32 modular arithmetic).
class CreditReporter {
 public:
  explicit CreditReporter(uint32_t window) : window_(window) {}
  void consumed(uint32_t n) { consumed_ += n; }
  uint32_t total() const { return consumed_; }
  // True when at least window/4 bytes were consumed since the last report.
  bool reportDue() const { return consumed_ - reported_ >= window_ / 4; }
  // Call whenever rx_consumed goes out (CREDIT, ACK, PONG, CAPS).
  void reported() { reported_ = consumed_; }

 private:
  uint32_t window_;
  uint32_t consumed_ = 0;
  uint32_t reported_ = 0;
};

// Host side: in-flight accounting against the advertised window. Mirrored by
// pclink.py; tested here so both ends agree on the math.
class CreditWindow {
 public:
  // baseline = CAPS.rx_consumed; bytes sent before HELLO's CAPS never count.
  void begin(uint32_t window, uint32_t baseline) {
    window_ = window;
    base_ = baseline;
    sent_ = 0;
    consumed_ = 0;
  }
  // Latest cumulative rx_consumed from ACK/PONG/CREDIT. Stale (older) reports
  // are ignored, judged modulo 2^32 relative to the last accepted one. A report
  // past what was sent (a duplicate HELLO the device read after the baseline)
  // is clamped: it can only understate in-flight by that HELLO's 14 bytes,
  // which CONTROL_SLACK covers.
  void update(uint32_t rxConsumed) {
    const uint32_t rel = rxConsumed - base_;
    if (static_cast<int32_t>(rel - consumed_) > 0) consumed_ = rel <= sent_ ? rel : sent_;
  }
  uint32_t inFlight() const { return sent_ - consumed_; }
  uint32_t available() const { return inFlight() >= window_ ? 0 : window_ - inFlight(); }
  void sent(uint32_t n) { sent_ += n; }

 private:
  uint32_t window_ = 0;
  uint32_t base_ = 0;
  uint32_t sent_ = 0;      // bytes since baseline
  uint32_t consumed_ = 0;  // device-consumed bytes since baseline
};

// --- wear policy ---------------------------------------------------------------------

struct WearConfig {
  uint16_t fullEveryN = 60;         // FAST refreshes between scrubs
  uint16_t fullEverySec = 30 * 60;  // seconds since the last clean refresh
  uint16_t minGapMs = 500;          // between waveform activations
  uint8_t inputMask = INPUT_MASK_BUTTONS | INPUT_MASK_TOUCH;
};

constexpr uint16_t FLOOR_FULL_EVERY_N = 20;
constexpr uint16_t CEIL_FULL_EVERY_N = 1000;
constexpr uint16_t FLOOR_FULL_EVERY_S = 5 * 60;
constexpr uint16_t CEIL_FULL_EVERY_S = 4 * 60 * 60;
constexpr uint16_t FLOOR_MIN_GAP_MS = 300;
constexpr uint16_t CEIL_MIN_GAP_MS = 60000;

// Applies a CONFIG payload (8 bytes) onto cfg with clamping. Returns true if
// any requested value had to be clamped.
bool applyConfig(WearConfig& cfg, const uint8_t* payload);
void encodeConfig(const WearConfig& cfg, uint8_t* out8);

// Single-slot refresh scheduler. The activity asks it when a waveform may run
// and what it should be; it never decides to refresh by itself.
class RefreshPolicy {
 public:
  struct Action {
    RefreshMode mode;   // what to run (after scrub promotion)
    uint16_t seq;       // latest coalesced REFRESH seq
    bool promoted;      // FAST promoted to FULL by the scrub rule
    bool coalesced;     // more than one request folded into this one
  };

  void setConfig(const WearConfig& cfg) { cfg_ = cfg; }
  const WearConfig& config() const { return cfg_; }
  // Anchor the scrub clock at session start.
  void begin(uint32_t nowMs);

  void request(RefreshMode mode, uint16_t seq);
  bool pending() const { return pending_; }
  // True (and fills out) when a pending request may run now. Does not clear
  // the slot: call executed() after the waveform finishes.
  bool due(uint32_t nowMs, Action& out) const;
  void executed(const Action& action, uint32_t doneMs);

  uint16_t fastSinceClean() const { return fastSinceClean_; }
  uint32_t scrubs() const { return scrubs_; }

 private:
  WearConfig cfg_{};
  bool pending_ = false;
  bool coalesced_ = false;
  RefreshMode pendingMode_ = RefreshMode::Fast;
  uint16_t pendingSeq_ = 0;
  bool anyActivation_ = false;
  uint32_t lastActivationMs_ = 0;
  uint32_t lastCleanMs_ = 0;
  uint16_t fastSinceClean_ = 0;
  uint32_t scrubs_ = 0;
};

// --- BLIT handling -----------------------------------------------------------------------

struct BlitHeader {
  uint16_t x, y, w, h;
  PixelFormat fmt;
  Encoding enc;
  const uint8_t* data;
  uint32_t dataLen;
  uint32_t decodedLen;  // w*h*bpp/8
};

// Parses and validates a BLIT payload: geometry, format, encoding, and for raw
// data the exact length. Deflate data is length-checked by inflateExact().
Status parseBlit(const uint8_t* payload, uint32_t len, BlitHeader& out);

// One-shot raw-DEFLATE decode requiring the output to be EXACTLY expected
// bytes. dst must hold expected + 1 bytes (the extra byte is how an overlong
// stream is detected). No allocation: the caller owns the ~1.2 KB decoder
// state (kept off the loop task's stack) and reuses it for every BLIT.
bool inflateExact(InflateReader& reader, const uint8_t* src, uint32_t srcLen, uint8_t* dst, uint32_t expected);

// Canvas operations. fb is the 1bpp panel framebuffer (100 B/row), gray the
// 2bpp staging canvas (200 B/row). Both are always kept in step: a 1bpp BLIT
// mirrors into gray as black/white, a 2bpp BLIT thresholds into fb (anything
// but white is black — the grayscale pipeline's base frame).
void applyBlit(const BlitHeader& b, const uint8_t* pixels, uint8_t* fb, uint8_t* gray);
void clearCanvases(bool white, uint8_t* fb, uint8_t* gray);
// Grayscale planes for the overlay pipeline (lib/GfxRenderer/BitmapHelpers.h
// grayPlanePixel, overlay mode): LSB plane bit set where level == 1, MSB
// plane bit set where level is 1 or 2. out is a 1bpp full-panel buffer.
void buildGrayPlane(const uint8_t* gray, uint8_t* out, bool msb);
// The B/W base of the gray canvas (white only where level == 3) into fb.
void buildGrayBase(const uint8_t* gray, uint8_t* fb);
// The inverse sync: the gray canvas becomes the framebuffer's black/white
// (levels 0/3). Used when a session starts over whatever is on screen.
void grayFromFramebuffer(const uint8_t* fb, uint8_t* gray);

// --- small message builders (device side) ----------------------------------------------------

constexpr size_t CAPS_MAX_PAYLOAD = 29 + BUTTON_COUNT + 1 + 32;

size_t buildCaps(uint8_t* out, size_t cap, const WearConfig& cfg, uint32_t rxConsumed, const char* fwVersion);
void buildAck(uint8_t* out8, uint16_t seq, Status status, uint32_t rxConsumed);
void buildRefreshDone(uint8_t* out8, const RefreshPolicy::Action& a, uint32_t durationMs);
void buildInput(uint8_t* out12, uint8_t src, uint8_t code, uint8_t state, uint16_t x, uint16_t y, uint32_t tMs);

}  // namespace pclink

#endif  // CROSSPOINT_PC_LINK
