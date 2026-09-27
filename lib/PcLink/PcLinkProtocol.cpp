#include "PcLinkProtocol.h"

#ifdef CROSSPOINT_PC_LINK

#include <InflateReader.h>

#include <cstring>

#if defined(ESP_PLATFORM)
#include <esp_rom_crc.h>
#endif

namespace pclink {

// --- CRC-32 -------------------------------------------------------------------

uint32_t crc32Update(const uint32_t crc, const uint8_t* data, const size_t len) {
#if defined(ESP_PLATFORM)
  return esp_rom_crc32_le(crc, data, static_cast<uint32_t>(len));
#else
  uint32_t c = ~crc;
  for (size_t i = 0; i < len; ++i) {
    c ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0U - (c & 1U);
      c = (c >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return ~c;
#endif
}

// --- framing --------------------------------------------------------------------

bool isHostToDevice(const MsgType type) {
  const auto t = static_cast<uint8_t>(type);
  return t >= static_cast<uint8_t>(MsgType::Hello) && t <= static_cast<uint8_t>(MsgType::Bye);
}

bool isDeviceToHost(const MsgType type) {
  const auto t = static_cast<uint8_t>(type);
  return t >= static_cast<uint8_t>(MsgType::Caps) && t <= static_cast<uint8_t>(MsgType::Credit);
}

bool payloadLengthValid(const MsgType type, const uint32_t len) {
  switch (type) {
    case MsgType::Hello:
    case MsgType::Refresh:
    case MsgType::Clear:
    case MsgType::Ping:
    case MsgType::Credit:
    case MsgType::Err:
      return len == 4;
    case MsgType::Config:
    case MsgType::Ack:
    case MsgType::RefreshDone:
    case MsgType::Pong:
    case MsgType::ConfigState:
      return len == 8;
    case MsgType::Input:
      return len == 12;
    case MsgType::Bye:
      return len == 0;
    case MsgType::Blit:
      return len > BLIT_HEADER_SIZE && len <= MAX_PAYLOAD;
    case MsgType::Caps:
      return len >= 30 && len <= CAPS_MAX_PAYLOAD;
  }
  return false;
}

size_t encodeFrame(uint8_t* out, const size_t outCap, const MsgType type, const uint16_t seq,
                   const uint8_t* payload, const uint32_t len) {
  if (len > MAX_PAYLOAD || outCap < FRAME_OVERHEAD + len) return 0;
  out[0] = MAGIC0;
  out[1] = MAGIC1;
  out[2] = PROTO_VERSION;
  out[3] = static_cast<uint8_t>(type);
  wr16(out + 4, seq);
  wr32(out + 6, len);
  if (len) memcpy(out + HEADER_SIZE, payload, len);
  const uint32_t crc = crc32Update(0, out, HEADER_SIZE + len);
  wr32(out + HEADER_SIZE + len, crc);
  return FRAME_OVERHEAD + len;
}

FrameParser::FrameParser(uint8_t* buffer, const size_t capacity, const uint32_t maxPayload,
                         const Direction direction)
    : buf_(buffer), cap_(capacity), maxPayload_(maxPayload), dir_(direction) {
  // Never trust a length the buffer cannot hold.
  if (cap_ < FRAME_OVERHEAD) {
    maxPayload_ = 0;
  } else if (maxPayload_ > cap_ - FRAME_OVERHEAD) {
    maxPayload_ = static_cast<uint32_t>(cap_ - FRAME_OVERHEAD);
  }
}

void FrameParser::reset() {
  fill_ = 0;
  ready_ = false;
  headerOk_ = false;
  curLen_ = 0;
}

size_t FrameParser::wanted() const {
  if (ready_) return 0;
  if (!headerOk_) return fill_ < HEADER_SIZE ? HEADER_SIZE - fill_ : 0;
  const size_t total = FRAME_OVERHEAD + curLen_;
  return fill_ < total ? total - fill_ : 0;
}

size_t FrameParser::feed(const uint8_t* data, const size_t len) {
  size_t used = 0;
  while (used < len && !ready_) {
    // wanted() never lets fill_ pass the current candidate's end, so a
    // completed frame is always at the buffer's tail unless a resync shifted
    // already-buffered bytes forward (handled in release()).
    size_t take = wanted();
    if (take == 0) take = 1;  // defensive: always make progress
    if (take > len - used) take = len - used;
    if (take > cap_ - fill_) take = cap_ - fill_;
    if (take == 0) {
      // Cannot happen with a validated header; recover by dropping the lot.
      stats_.bytesDiscarded += static_cast<uint32_t>(fill_);
      reset();
      continue;
    }
    memcpy(buf_ + fill_, data + used, take);
    fill_ += take;
    used += take;
    advance();
  }
  return used;
}

void FrameParser::discardToNextMagic() {
  // Drop buf_[0] and everything up to the next possible frame start.
  size_t next = 1;
  while (next < fill_ && buf_[next] != MAGIC0) ++next;
  stats_.bytesDiscarded += static_cast<uint32_t>(next);
  memmove(buf_, buf_ + next, fill_ - next);
  fill_ -= next;
  headerOk_ = false;
}

void FrameParser::advance() {
  while (!ready_ && fill_ > 0) {
    if (buf_[0] != MAGIC0 || (fill_ >= 2 && buf_[1] != MAGIC1)) {
      discardToNextMagic();
      continue;
    }
    if (fill_ < HEADER_SIZE) return;

    if (!headerOk_) {
      const auto type = static_cast<MsgType>(buf_[3]);
      const uint32_t len = rd32(buf_ + 6);
      const bool dirOk = dir_ == Direction::HostToDevice ? isHostToDevice(type) : isDeviceToHost(type);
      if (buf_[2] != PROTO_VERSION || !dirOk || len > maxPayload_ || !payloadLengthValid(type, len)) {
        ++stats_.headerRejects;
        ++pendingRejects_;
        discardToNextMagic();
        continue;
      }
      headerOk_ = true;
      curLen_ = len;
    }

    const size_t total = FRAME_OVERHEAD + curLen_;
    if (fill_ < total) return;

    const uint32_t want = rd32(buf_ + HEADER_SIZE + curLen_);
    if (crc32Update(0, buf_, HEADER_SIZE + curLen_) != want) {
      ++stats_.crcErrors;
      ++pendingRejects_;
      discardToNextMagic();
      continue;
    }

    frame_.type = static_cast<MsgType>(buf_[3]);
    frame_.seq = rd16(buf_ + 4);
    frame_.len = curLen_;
    frame_.payload = buf_ + HEADER_SIZE;
    ++stats_.frames;
    ready_ = true;
  }
}

void FrameParser::release() {
  if (!ready_) return;
  const size_t total = FRAME_OVERHEAD + curLen_;
  const size_t rest = fill_ > total ? fill_ - total : 0;
  if (rest) memmove(buf_, buf_ + total, rest);
  fill_ = rest;
  ready_ = false;
  headerOk_ = false;
  curLen_ = 0;
  advance();
}

uint32_t FrameParser::takeRejects() {
  const uint32_t n = pendingRejects_;
  pendingRejects_ = 0;
  return n;
}

// --- wear policy ------------------------------------------------------------------

namespace {

uint16_t clampField(const uint16_t requested, const uint16_t current, const uint16_t lo, const uint16_t hi,
                    bool& clamped) {
  if (requested == 0) return current;
  if (requested < lo) {
    clamped = true;
    return lo;
  }
  if (requested > hi) {
    clamped = true;
    return hi;
  }
  return requested;
}

int modeRank(const RefreshMode m) {
  switch (m) {
    case RefreshMode::Fast:
      return 0;
    case RefreshMode::Half:
      return 1;
    case RefreshMode::Full:
      return 2;
    case RefreshMode::Gray:
      return 3;
  }
  return 0;
}

}  // namespace

bool applyConfig(WearConfig& cfg, const uint8_t* payload) {
  bool clamped = false;
  cfg.fullEveryN = clampField(rd16(payload + 0), cfg.fullEveryN, FLOOR_FULL_EVERY_N, CEIL_FULL_EVERY_N, clamped);
  cfg.fullEverySec = clampField(rd16(payload + 2), cfg.fullEverySec, FLOOR_FULL_EVERY_S, CEIL_FULL_EVERY_S, clamped);
  cfg.minGapMs = clampField(rd16(payload + 4), cfg.minGapMs, FLOOR_MIN_GAP_MS, CEIL_MIN_GAP_MS, clamped);
  cfg.inputMask = payload[6] & (INPUT_MASK_BUTTONS | INPUT_MASK_TOUCH);
  if (payload[6] & ~(INPUT_MASK_BUTTONS | INPUT_MASK_TOUCH)) clamped = true;
  return clamped;
}

void encodeConfig(const WearConfig& cfg, uint8_t* out8) {
  wr16(out8 + 0, cfg.fullEveryN);
  wr16(out8 + 2, cfg.fullEverySec);
  wr16(out8 + 4, cfg.minGapMs);
  out8[6] = cfg.inputMask;
  out8[7] = 0;
}

void RefreshPolicy::begin(const uint32_t nowMs) {
  pending_ = false;
  coalesced_ = false;
  anyActivation_ = false;
  lastCleanMs_ = nowMs;
  fastSinceClean_ = 0;
}

void RefreshPolicy::request(const RefreshMode mode, const uint16_t seq) {
  if (pending_) {
    coalesced_ = true;
    if (modeRank(mode) > modeRank(pendingMode_)) pendingMode_ = mode;
  } else {
    pending_ = true;
    coalesced_ = false;
    pendingMode_ = mode;
  }
  pendingSeq_ = seq;  // latest wins
}

bool RefreshPolicy::due(const uint32_t nowMs, Action& out) const {
  if (!pending_) return false;
  if (anyActivation_ && nowMs - lastActivationMs_ < cfg_.minGapMs) return false;
  out.mode = pendingMode_;
  out.seq = pendingSeq_;
  out.coalesced = coalesced_;
  out.promoted = false;
  if (pendingMode_ == RefreshMode::Fast && fastSinceClean_ > 0) {
    // fastSinceClean_ FAST waveforms already ran; this would be one more.
    const bool byCount = fastSinceClean_ >= cfg_.fullEveryN;
    const bool byTime = nowMs - lastCleanMs_ >= static_cast<uint32_t>(cfg_.fullEverySec) * 1000U;
    if (byCount || byTime) {
      out.mode = RefreshMode::Full;
      out.promoted = true;
    }
  }
  return true;
}

void RefreshPolicy::executed(const Action& action, const uint32_t doneMs) {
  pending_ = false;
  coalesced_ = false;
  anyActivation_ = true;
  lastActivationMs_ = doneMs;
  if (action.mode == RefreshMode::Fast) {
    if (fastSinceClean_ < 0xFFFF) ++fastSinceClean_;
  } else {
    fastSinceClean_ = 0;
    lastCleanMs_ = doneMs;
  }
  if (action.promoted) ++scrubs_;
}

// --- BLIT ----------------------------------------------------------------------------

Status parseBlit(const uint8_t* payload, const uint32_t len, BlitHeader& out) {
  if (len <= BLIT_HEADER_SIZE) return Status::BadLength;
  out.x = rd16(payload + 0);
  out.y = rd16(payload + 2);
  out.w = rd16(payload + 4);
  out.h = rd16(payload + 6);
  const uint8_t fmt = payload[8];
  const uint8_t enc = payload[9];
  if (fmt > static_cast<uint8_t>(PixelFormat::Bpp2) || enc > static_cast<uint8_t>(Encoding::Deflate)) {
    return Status::BadFormat;
  }
  out.fmt = static_cast<PixelFormat>(fmt);
  out.enc = static_cast<Encoding>(enc);
  if (out.w == 0 || out.h == 0 || (out.x % 8) != 0 || (out.w % 8) != 0) return Status::BadRect;
  if (static_cast<uint32_t>(out.x) + out.w > PANEL_WIDTH || static_cast<uint32_t>(out.y) + out.h > PANEL_HEIGHT) {
    return Status::BadRect;
  }
  const uint32_t bpp = out.fmt == PixelFormat::Bpp1 ? 1 : 2;
  out.decodedLen = static_cast<uint32_t>(out.w) * out.h * bpp / 8;
  out.data = payload + BLIT_HEADER_SIZE;
  out.dataLen = len - static_cast<uint32_t>(BLIT_HEADER_SIZE);
  if (out.enc == Encoding::Raw && out.dataLen != out.decodedLen) return Status::BadLength;
  return Status::Ok;
}

bool inflateExact(InflateReader& reader, const uint8_t* src, const uint32_t srcLen, uint8_t* dst,
                  const uint32_t expected) {
  reader.init(false);  // one-shot: dst is the whole output, no window allocated
  reader.setSource(src, srcLen);
  size_t produced = 0;
  // One byte of headroom: a stream that is exactly `expected` long ends with
  // Done; a longer one fills the headroom and reports Ok.
  const InflateStatus st = reader.readAtMost(dst, static_cast<size_t>(expected) + 1, &produced);
  // A truncated stream reads past the source and trips uzlib's sticky eof.
  return st == InflateStatus::Done && produced == expected && !reader.raw()->eof;
}

namespace {

constexpr uint32_t FB_STRIDE = PANEL_WIDTH / 8;    // 100
constexpr uint32_t GRAY_STRIDE = PANEL_WIDTH / 4;  // 200

// 1bpp nibble (4 px, MSB first) -> one 2bpp byte, bit 1 = white = level 3.
constexpr uint8_t expandNibble(const uint8_t n) {
  return static_cast<uint8_t>(((n & 8) ? 0xC0 : 0) | ((n & 4) ? 0x30 : 0) | ((n & 2) ? 0x0C : 0) |
                              ((n & 1) ? 0x03 : 0));
}

// 2bpp byte (4 px) -> 4-bit nibble; select(level) chooses which levels set the bit.
template <typename Select>
uint8_t packNibble(const uint8_t g, Select select) {
  uint8_t n = 0;
  for (int i = 0; i < 4; ++i) {
    const uint8_t level = static_cast<uint8_t>((g >> (6 - 2 * i)) & 3);
    if (select(level)) n |= static_cast<uint8_t>(8 >> i);
  }
  return n;
}

template <typename Select>
void grayRowToBits(const uint8_t* g, uint8_t* out, const uint32_t outBytes, Select select) {
  for (uint32_t i = 0; i < outBytes; ++i) {
    out[i] = static_cast<uint8_t>((packNibble(g[2 * i], select) << 4) | packNibble(g[2 * i + 1], select));
  }
}

}  // namespace

void applyBlit(const BlitHeader& b, const uint8_t* pixels, uint8_t* fb, uint8_t* gray) {
  const uint32_t fbBytes = b.w / 8;
  const uint32_t grayBytes = b.w / 4;
  const auto isWhite = [](const uint8_t level) { return level == 3; };
  for (uint32_t r = 0; r < b.h; ++r) {
    uint8_t* fbRow = fb + (b.y + r) * FB_STRIDE + b.x / 8;
    uint8_t* grayRow = gray + (b.y + r) * GRAY_STRIDE + b.x / 4;
    if (b.fmt == PixelFormat::Bpp1) {
      const uint8_t* src = pixels + r * fbBytes;
      memcpy(fbRow, src, fbBytes);
      for (uint32_t i = 0; i < fbBytes; ++i) {
        grayRow[2 * i] = expandNibble(static_cast<uint8_t>(src[i] >> 4));
        grayRow[2 * i + 1] = expandNibble(static_cast<uint8_t>(src[i] & 0x0F));
      }
    } else {
      const uint8_t* src = pixels + r * grayBytes;
      memcpy(grayRow, src, grayBytes);
      grayRowToBits(src, fbRow, fbBytes, isWhite);
    }
  }
}

void clearCanvases(const bool white, uint8_t* fb, uint8_t* gray) {
  memset(fb, white ? 0xFF : 0x00, FB_1BPP_BYTES);
  memset(gray, white ? 0xFF : 0x00, FB_2BPP_BYTES);
}

void buildGrayPlane(const uint8_t* gray, uint8_t* out, const bool msb) {
  if (msb) {
    grayRowToBits(gray, out, FB_1BPP_BYTES, [](const uint8_t level) { return level == 1 || level == 2; });
  } else {
    grayRowToBits(gray, out, FB_1BPP_BYTES, [](const uint8_t level) { return level == 1; });
  }
}

void buildGrayBase(const uint8_t* gray, uint8_t* fb) {
  grayRowToBits(gray, fb, FB_1BPP_BYTES, [](const uint8_t level) { return level == 3; });
}

void grayFromFramebuffer(const uint8_t* fb, uint8_t* gray) {
  for (uint32_t i = 0; i < FB_1BPP_BYTES; ++i) {
    gray[2 * i] = expandNibble(static_cast<uint8_t>(fb[i] >> 4));
    gray[2 * i + 1] = expandNibble(static_cast<uint8_t>(fb[i] & 0x0F));
  }
}

// --- message builders -------------------------------------------------------------------

size_t buildCaps(uint8_t* out, const size_t cap, const WearConfig& cfg, const uint32_t rxConsumed,
                 const char* fwVersion) {
  size_t fwLen = fwVersion ? strlen(fwVersion) : 0;
  if (fwLen > 32) fwLen = 32;
  const size_t need = 29 + BUTTON_COUNT + 1 + fwLen;
  if (cap < need) return 0;
  wr16(out + 0, PROTO_VERSION);
  wr16(out + 2, PANEL_WIDTH);
  wr16(out + 4, PANEL_HEIGHT);
  out[6] = 0x03;  // 1bpp | 2bpp
  out[7] = 0x03;  // raw | deflate
  wr32(out + 8, MAX_PAYLOAD);
  wr32(out + 12, RX_WINDOW);
  wr32(out + 16, rxConsumed);
  wr16(out + 20, cfg.fullEveryN);
  wr16(out + 22, cfg.fullEverySec);
  wr16(out + 24, cfg.minGapMs);
  out[26] = cfg.inputMask;
  out[27] = 1;  // touch events
  out[28] = BUTTON_COUNT;
  for (uint8_t i = 0; i < BUTTON_COUNT; ++i) out[29 + i] = i;
  out[29 + BUTTON_COUNT] = static_cast<uint8_t>(fwLen);
  if (fwLen) memcpy(out + 30 + BUTTON_COUNT, fwVersion, fwLen);
  return need;
}

void buildAck(uint8_t* out8, const uint16_t seq, const Status status, const uint32_t rxConsumed) {
  wr16(out8, seq);
  out8[2] = static_cast<uint8_t>(status);
  out8[3] = 0;
  wr32(out8 + 4, rxConsumed);
}

void buildRefreshDone(uint8_t* out8, const RefreshPolicy::Action& a, const uint32_t durationMs) {
  wr16(out8, a.seq);
  out8[2] = static_cast<uint8_t>(a.mode);
  out8[3] = static_cast<uint8_t>((a.promoted ? 1 : 0) | (a.coalesced ? 2 : 0));
  wr32(out8 + 4, durationMs);
}

void buildInput(uint8_t* out12, const uint8_t src, const uint8_t code, const uint8_t state, const uint16_t x,
                const uint16_t y, const uint32_t tMs) {
  out12[0] = src;
  out12[1] = code;
  out12[2] = state;
  out12[3] = 0;
  wr16(out12 + 4, x);
  wr16(out12 + 6, y);
  wr32(out12 + 8, tMs);
}

}  // namespace pclink

#endif  // CROSSPOINT_PC_LINK
