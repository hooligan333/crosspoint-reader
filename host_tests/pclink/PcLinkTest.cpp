// Host tests for lib/PcLink. Built by build.sh with g++ -DCROSSPOINT_PC_LINK;
// never compiled into the firmware (host_tests/ is outside src/ and lib/).
//
// Covers the device-independent half of PC Link: framing round trips, resync
// after garbage (including a false header that swallows a real frame), CRC and
// header rejection, BLIT validation and pixel conversion, the one-shot
// DEFLATE path through the firmware's own InflateReader/uzlib, credit-window
// math (with a simulated HWCDC queue that must never overflow), and the wear
// policy's gap / coalescing / scrub rules.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "InflateReader.h"
#include "PcLinkProtocol.h"

#ifdef PCLINK_TEST_ZLIB
#include <zlib.h>
#endif

using namespace pclink;

namespace {

// --- tiny harness ------------------------------------------------------------

int g_checks = 0;
int g_failures = 0;
const char* g_group = "";

void beginGroup(const char* name) { g_group = name; }

void record(bool ok, const char* expr, int line) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    printf("FAIL [%s] line %d: %s\n", g_group, line, expr);
  }
}

#define EXPECT(cond) record((cond), #cond, __LINE__)

#define EXPECT_EQ_U(actual, expected)                                                                               \
  do {                                                                                                              \
    const unsigned long long a_ = static_cast<unsigned long long>(actual);                                          \
    const unsigned long long e_ = static_cast<unsigned long long>(expected);                                        \
    ++g_checks;                                                                                                     \
    if (a_ != e_) {                                                                                                 \
      ++g_failures;                                                                                                 \
      printf("FAIL [%s] line %d: %s == %s (got %llu, want %llu)\n", g_group, __LINE__, #actual, #expected, a_, e_); \
    }                                                                                                               \
  } while (0)

// Deterministic xorshift so failures reproduce.
uint32_t g_rng = 0x12345678U;
uint32_t rnd() {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return g_rng;
}

std::vector<uint8_t> frameBytes(MsgType type, uint16_t seq, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> out(FRAME_OVERHEAD + payload.size());
  const size_t n = encodeFrame(out.data(), out.size(), type, seq, payload.empty() ? nullptr : payload.data(),
                               static_cast<uint32_t>(payload.size()));
  out.resize(n);
  return out;
}

struct Collected {
  MsgType type;
  uint16_t seq;
  std::vector<uint8_t> payload;
};

// Feed `stream` in chunks of chunkMax (0 = random 1..64) and collect frames.
std::vector<Collected> parseAll(FrameParser& p, const std::vector<uint8_t>& stream, size_t chunkMax) {
  std::vector<Collected> got;
  size_t pos = 0;
  auto drain = [&]() {
    while (p.hasFrame()) {
      const Frame& f = p.frame();
      got.push_back({f.type, f.seq, std::vector<uint8_t>(f.payload, f.payload + f.len)});
      p.release();
    }
  };
  while (pos < stream.size()) {
    size_t chunk = chunkMax ? chunkMax : 1 + rnd() % 64;
    if (chunk > stream.size() - pos) chunk = stream.size() - pos;
    size_t off = 0;
    while (off < chunk) {
      off += p.feed(stream.data() + pos + off, chunk - off);
      drain();
    }
    pos += chunk;
  }
  drain();
  return got;
}

std::vector<uint8_t> g_parseBuf(MAX_FRAME);

// --- CRC ----------------------------------------------------------------------

void testCrc() {
  beginGroup("crc32");
  const char* check = "123456789";
  EXPECT_EQ_U(crc32Update(0, reinterpret_cast<const uint8_t*>(check), 9), 0xCBF43926U);
  EXPECT_EQ_U(crc32Update(0, nullptr, 0), 0U);
  std::vector<uint8_t> data(5000);
  for (auto& b : data) b = static_cast<uint8_t>(rnd());
  const uint32_t whole = crc32Update(0, data.data(), data.size());
  const uint32_t chained = crc32Update(crc32Update(0, data.data(), 1234), data.data() + 1234, data.size() - 1234);
  EXPECT_EQ_U(chained, whole);
#ifdef PCLINK_TEST_ZLIB
  EXPECT_EQ_U(whole, crc32(0L, data.data(), static_cast<uInt>(data.size())));
#endif
}

// --- framing ------------------------------------------------------------------

void testRoundTrip() {
  beginGroup("round-trip");
  std::vector<uint8_t> stream;
  std::vector<Collected> sent;
  const std::vector<uint8_t> p4 = {1, 0, 0, 0};
  const std::vector<uint8_t> p8 = {60, 0, 8, 7, 244, 1, 3, 0};
  std::vector<uint8_t> blit(BLIT_HEADER_SIZE + 8 * 4 / 8);
  wr16(&blit[0], 16);
  wr16(&blit[2], 20);
  wr16(&blit[4], 8);
  wr16(&blit[6], 4);
  blit[8] = 0;
  blit[9] = 0;
  for (size_t i = BLIT_HEADER_SIZE; i < blit.size(); ++i) blit[i] = static_cast<uint8_t>(0xA0 + i);
  const struct {
    MsgType t;
    const std::vector<uint8_t>* p;
  } plan[] = {{MsgType::Hello, &p4}, {MsgType::Blit, &blit}, {MsgType::Refresh, &p4}, {MsgType::Clear, &p4},
              {MsgType::Config, &p8}, {MsgType::Ping, &p4},  {MsgType::Bye, nullptr}};
  uint16_t seq = 0xFFFE;  // exercise wrap
  for (const auto& step : plan) {
    const std::vector<uint8_t> payload = step.p ? *step.p : std::vector<uint8_t>{};
    const auto f = frameBytes(step.t, seq, payload);
    EXPECT_EQ_U(f.size(), FRAME_OVERHEAD + payload.size());
    EXPECT(f[0] == 'X' && f[1] == 'P' && f[2] == PROTO_VERSION);
    stream.insert(stream.end(), f.begin(), f.end());
    sent.push_back({step.t, seq, payload});
    ++seq;
  }
  for (size_t chunk : {size_t(1), size_t(7), size_t(4096), size_t(0)}) {
    FrameParser p(g_parseBuf.data(), g_parseBuf.size(), MAX_PAYLOAD, Direction::HostToDevice);
    const auto got = parseAll(p, stream, chunk);
    EXPECT_EQ_U(got.size(), sent.size());
    for (size_t i = 0; i < got.size() && i < sent.size(); ++i) {
      EXPECT(got[i].type == sent[i].type);
      EXPECT_EQ_U(got[i].seq, sent[i].seq);
      EXPECT(got[i].payload == sent[i].payload);
    }
    EXPECT_EQ_U(p.stats().crcErrors, 0);
    EXPECT_EQ_U(p.stats().bytesDiscarded, 0);
  }

  // Device -> host direction with the real builders.
  WearConfig cfg;
  std::vector<uint8_t> caps(CAPS_MAX_PAYLOAD);
  const size_t capsLen = buildCaps(caps.data(), caps.size(), cfg, 0xDEADBEEF, "1.6.5-x4pro");
  caps.resize(capsLen);
  EXPECT(payloadLengthValid(MsgType::Caps, static_cast<uint32_t>(capsLen)));
  EXPECT_EQ_U(rd32(&caps[16]), 0xDEADBEEFU);
  EXPECT_EQ_U(rd16(&caps[2]), 800);
  EXPECT_EQ_U(rd16(&caps[4]), 480);
  EXPECT_EQ_U(rd32(&caps[8]), MAX_PAYLOAD);
  EXPECT_EQ_U(rd32(&caps[12]), RX_WINDOW);
  EXPECT_EQ_U(rd16(&caps[24]), 500);
  EXPECT_EQ_U(caps[28], BUTTON_COUNT);
  EXPECT_EQ_U(caps[29 + BUTTON_COUNT], 11);
  EXPECT(memcmp(&caps[30 + BUTTON_COUNT], "1.6.5-x4pro", 11) == 0);
  std::vector<uint8_t> ack(8);
  buildAck(ack.data(), 77, Status::BadRect, 1234);
  std::vector<uint8_t> input(12);
  buildInput(input.data(), INPUT_SRC_TOUCH, 0, 1, 799, 479, 99999);
  std::vector<uint8_t> dstream;
  for (const auto& f : {frameBytes(MsgType::Caps, 1, caps), frameBytes(MsgType::Ack, 2, ack),
                        frameBytes(MsgType::Input, 3, input)}) {
    dstream.insert(dstream.end(), f.begin(), f.end());
  }
  FrameParser hp(g_parseBuf.data(), g_parseBuf.size(), MAX_PAYLOAD, Direction::DeviceToHost);
  const auto got = parseAll(hp, dstream, 0);
  EXPECT_EQ_U(got.size(), 3);
  if (got.size() == 3) {
    EXPECT(got[0].type == MsgType::Caps && got[0].payload == caps);
    EXPECT_EQ_U(rd16(&got[1].payload[0]), 77);
    EXPECT_EQ_U(got[1].payload[2], static_cast<uint8_t>(Status::BadRect));
    EXPECT_EQ_U(rd32(&got[1].payload[4]), 1234);
    EXPECT_EQ_U(rd16(&got[2].payload[4]), 799);
    EXPECT_EQ_U(rd32(&got[2].payload[8]), 99999);
  }
  // A host->device parser must not accept device->host frames (and v.v.).
  FrameParser wrongDir(g_parseBuf.data(), g_parseBuf.size(), MAX_PAYLOAD, Direction::HostToDevice);
  EXPECT_EQ_U(parseAll(wrongDir, dstream, 0).size(), 0);
  EXPECT(wrongDir.stats().headerRejects >= 3);
}

void testResync() {
  beginGroup("resync");
  const auto good1 = frameBytes(MsgType::Ping, 10, {1, 2, 3, 4});
  const auto good2 = frameBytes(MsgType::Refresh, 11, {0, 0, 0, 0});

  // 1. Log text and random noise (with stray 'X' and "XP") before a frame.
  std::vector<uint8_t> s;
  const char* log = "[123] INF MEM: Free XP 1234 XPXPX\nXP\x01";
  s.insert(s.end(), log, log + strlen(log));
  for (int i = 0; i < 300; ++i) s.push_back(static_cast<uint8_t>(rnd()));
  s.insert(s.end(), good1.begin(), good1.end());
  s.insert(s.end(), {'X', 'X', 'P'});
  s.insert(s.end(), good2.begin(), good2.end());
  {
    FrameParser p(g_parseBuf.data(), g_parseBuf.size(), MAX_PAYLOAD, Direction::HostToDevice);
    const auto got = parseAll(p, s, 0);
    EXPECT_EQ_U(got.size(), 2);
    if (got.size() == 2) {
      EXPECT_EQ_U(got[0].seq, 10);
      EXPECT_EQ_U(got[1].seq, 11);
    }
    EXPECT(p.stats().bytesDiscarded > 0);
  }

  // 2. A plausible false header (valid BLIT length) that swallows two real
  // frames: the CRC fails, and the parser must rescan the bytes it already
  // buffered to find both frames inside.
  {
    std::vector<uint8_t> t = {'X', 'P', PROTO_VERSION, static_cast<uint8_t>(MsgType::Blit), 0, 0};
    uint8_t len[4];
    wr32(len, 60);
    t.insert(t.end(), len, len + 4);
    t.insert(t.end(), good1.begin(), good1.end());  // 18 bytes
    t.insert(t.end(), good2.begin(), good2.end());  // 18 bytes
    for (int i = 0; i < 40; ++i) t.push_back(0x20);  // pads the fake frame to completion
    for (size_t chunk : {size_t(1), size_t(4096)}) {
      FrameParser p(g_parseBuf.data(), g_parseBuf.size(), MAX_PAYLOAD, Direction::HostToDevice);
      const auto got = parseAll(p, t, chunk);
      EXPECT_EQ_U(got.size(), 2);
      EXPECT_EQ_U(p.stats().crcErrors, 1);
      if (got.size() == 2) {
        EXPECT_EQ_U(got[0].seq, 10);
        EXPECT_EQ_U(got[1].seq, 11);
      }
    }
  }

  // 3. Truncated frame followed by a good one: the good one must survive.
  {
    std::vector<uint8_t> t(good1.begin(), good1.begin() + 12);
    t.insert(t.end(), good2.begin(), good2.end());
    t.insert(t.end(), good1.begin(), good1.end());
    FrameParser p(g_parseBuf.data(), g_parseBuf.size(), MAX_PAYLOAD, Direction::HostToDevice);
    const auto got = parseAll(p, t, 0);
    // good2 is swallowed into the truncated candidate and recovered by rescan.
    EXPECT_EQ_U(got.size(), 2);
    if (got.size() == 2) {
      EXPECT_EQ_U(got[0].seq, 11);
      EXPECT_EQ_U(got[1].seq, 10);
    }
  }
}

void testCrcRejection() {
  beginGroup("crc-rejection");
  auto bad = frameBytes(MsgType::Ping, 5, {9, 9, 9, 9});
  bad[HEADER_SIZE + 1] ^= 0x10;  // payload bit flip
  auto badCrc = frameBytes(MsgType::Ping, 6, {9, 9, 9, 9});
  badCrc.back() ^= 0x01;
  auto badHdr = frameBytes(MsgType::Ping, 8, {9, 9, 9, 9});
  badHdr[4] ^= 0x01;  // seq bit flip: the CRC covers the header too
  const auto good = frameBytes(MsgType::Ping, 7, {1, 1, 1, 1});
  std::vector<uint8_t> s(bad);
  s.insert(s.end(), badCrc.begin(), badCrc.end());
  s.insert(s.end(), badHdr.begin(), badHdr.end());
  s.insert(s.end(), good.begin(), good.end());
  FrameParser p(g_parseBuf.data(), g_parseBuf.size(), MAX_PAYLOAD, Direction::HostToDevice);
  const auto got = parseAll(p, s, 0);
  EXPECT_EQ_U(got.size(), 1);
  if (!got.empty()) EXPECT_EQ_U(got[0].seq, 7);
  EXPECT_EQ_U(p.stats().crcErrors, 3);
  EXPECT_EQ_U(p.takeRejects(), 3);
  EXPECT_EQ_U(p.takeRejects(), 0);
}

void testOversize() {
  beginGroup("oversize");
  auto hdr = [](uint8_t ver, uint8_t type, uint32_t len) {
    std::vector<uint8_t> h = {'X', 'P', ver, type, 0, 0, 0, 0, 0, 0};
    wr32(&h[6], len);
    return h;
  };
  const auto good = frameBytes(MsgType::Ping, 1, {0, 0, 0, 0});
  const std::vector<std::vector<uint8_t>> bads = {
      hdr(PROTO_VERSION, static_cast<uint8_t>(MsgType::Blit), MAX_PAYLOAD + 1),  // over the cap
      hdr(PROTO_VERSION, static_cast<uint8_t>(MsgType::Blit), 0xFFFFFFFFU),     // absurd
      hdr(PROTO_VERSION, static_cast<uint8_t>(MsgType::Blit), BLIT_HEADER_SIZE),  // no pixel data
      hdr(PROTO_VERSION, static_cast<uint8_t>(MsgType::Ping), 5),                 // fixed-length mismatch
      hdr(PROTO_VERSION, 0x42, 4),                                                // unknown type
      hdr(2, static_cast<uint8_t>(MsgType::Ping), 4),                             // future version
  };
  for (const auto& b : bads) {
    std::vector<uint8_t> s(b);
    s.insert(s.end(), good.begin(), good.end());
    FrameParser p(g_parseBuf.data(), g_parseBuf.size(), MAX_PAYLOAD, Direction::HostToDevice);
    const auto got = parseAll(p, s, 3);
    EXPECT_EQ_U(got.size(), 1);  // rejected at the header, never waits for len bytes
    EXPECT_EQ_U(p.stats().headerRejects, 1);
  }
  // A parser with a small buffer clamps maxPayload to what it can hold.
  std::vector<uint8_t> small(FRAME_OVERHEAD + 64);
  FrameParser sp(small.data(), small.size(), MAX_PAYLOAD, Direction::HostToDevice);
  std::vector<uint8_t> blitPayload(BLIT_HEADER_SIZE + 100, 0);
  const auto big = frameBytes(MsgType::Blit, 2, blitPayload);
  EXPECT_EQ_U(parseAll(sp, big, 0).size(), 0);
  EXPECT(sp.stats().headerRejects >= 1);
  // encodeFrame refuses what it cannot represent.
  std::vector<uint8_t> out(32);
  EXPECT_EQ_U(encodeFrame(out.data(), out.size(), MsgType::Ping, 0, nullptr, MAX_PAYLOAD + 1), 0);
  EXPECT_EQ_U(encodeFrame(out.data(), 10, MsgType::Ping, 0, out.data(), 4), 0);
}

// --- BLIT ---------------------------------------------------------------------

std::vector<uint8_t> blitPayload(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t fmt, uint8_t enc,
                                 const std::vector<uint8_t>& data) {
  std::vector<uint8_t> p(BLIT_HEADER_SIZE);
  wr16(&p[0], x);
  wr16(&p[2], y);
  wr16(&p[4], w);
  wr16(&p[6], h);
  p[8] = fmt;
  p[9] = enc;
  p.insert(p.end(), data.begin(), data.end());
  return p;
}

void testBlitValidation() {
  beginGroup("blit-validation");
  BlitHeader b{};
  auto check = [&](const std::vector<uint8_t>& p) { return parseBlit(p.data(), static_cast<uint32_t>(p.size()), b); };
  EXPECT(check(blitPayload(0, 0, 800, 480, 0, 0, std::vector<uint8_t>(48000))) == Status::Ok);
  EXPECT_EQ_U(b.decodedLen, 48000);
  EXPECT(check(blitPayload(0, 0, 800, 480, 1, 0, std::vector<uint8_t>(96000))) == Status::Ok);
  EXPECT_EQ_U(b.decodedLen, 96000);
  EXPECT(check(blitPayload(792, 479, 8, 1, 0, 0, {0xAA})) == Status::Ok);
  EXPECT(check(blitPayload(4, 0, 8, 1, 0, 0, {0})) == Status::BadRect);          // x not /8
  EXPECT(check(blitPayload(0, 0, 12, 1, 1, 0, {0, 0, 0})) == Status::BadRect);   // w not /8
  EXPECT(check(blitPayload(0, 0, 0, 1, 0, 0, {0})) == Status::BadRect);          // empty
  EXPECT(check(blitPayload(800, 0, 8, 1, 0, 0, {0})) == Status::BadRect);        // off the right
  EXPECT(check(blitPayload(0, 480, 8, 1, 0, 0, {0})) == Status::BadRect);        // off the bottom
  EXPECT(check(blitPayload(0, 400, 8, 81, 0, 0, std::vector<uint8_t>(81))) == Status::BadRect);
  EXPECT(check(blitPayload(0, 0, 65528, 1, 0, 0, {0})) == Status::BadRect);      // u16 overflow bait
  EXPECT(check(blitPayload(0, 0, 8, 2, 0, 0, {0})) == Status::BadLength);        // 1 of 2 bytes
  EXPECT(check(blitPayload(0, 0, 8, 1, 0, 0, {0, 0})) == Status::BadLength);     // 1 byte too many
  EXPECT(check(blitPayload(0, 0, 8, 1, 2, 0, {0})) == Status::BadFormat);
  EXPECT(check(blitPayload(0, 0, 8, 1, 0, 2, {0})) == Status::BadFormat);
  const std::vector<uint8_t> tiny = {0, 0, 0, 0, 8, 0, 1, 0, 0, 0, 0, 0};
  EXPECT(parseBlit(tiny.data(), static_cast<uint32_t>(tiny.size()), b) == Status::BadLength);
  // Deflate data is not length-checked against the decoded size here.
  EXPECT(check(blitPayload(0, 0, 800, 480, 0, 1, {1, 2, 3})) == Status::Ok);
}

// A stored (uncompressed) DEFLATE block: 1 final bit + type 00, LEN/NLEN.
std::vector<uint8_t> storedDeflate(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> out = {0x01};
  const auto len = static_cast<uint16_t>(data.size());
  out.push_back(static_cast<uint8_t>(len));
  out.push_back(static_cast<uint8_t>(len >> 8));
  out.push_back(static_cast<uint8_t>(~len));
  out.push_back(static_cast<uint8_t>(~len >> 8));
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

void testInflate() {
  beginGroup("inflate");
  InflateReader reader;
  std::vector<uint8_t> data(3000);
  for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i % 13 == 0 ? rnd() : i / 50);
  std::vector<uint8_t> dst(data.size() + 1);
  const auto stored = storedDeflate(data);
  EXPECT(inflateExact(reader, stored.data(), static_cast<uint32_t>(stored.size()), dst.data(),
                      static_cast<uint32_t>(data.size())));
  EXPECT(memcmp(dst.data(), data.data(), data.size()) == 0);
  // Stream longer than expected -> rejected.
  EXPECT(!inflateExact(reader, stored.data(), static_cast<uint32_t>(stored.size()), dst.data(),
                       static_cast<uint32_t>(data.size() - 1)));
  // Stream shorter than expected -> rejected.
  std::vector<uint8_t> big(data.size() + 2);
  EXPECT(!inflateExact(reader, stored.data(), static_cast<uint32_t>(stored.size()), big.data(),
                       static_cast<uint32_t>(data.size() + 1)));
  // Truncated input -> rejected (sticky eof), not zero-filled.
  EXPECT(!inflateExact(reader, stored.data(), static_cast<uint32_t>(stored.size() - 10), dst.data(),
                       static_cast<uint32_t>(data.size())));
  // Garbage -> rejected.
  std::vector<uint8_t> junk(64, 0xFF);
  EXPECT(!inflateExact(reader, junk.data(), static_cast<uint32_t>(junk.size()), dst.data(),
                       static_cast<uint32_t>(data.size())));
#ifdef PCLINK_TEST_ZLIB
  // Real raw-DEFLATE streams exactly as pclink.py produces them (wbits=-15).
  std::vector<uint8_t> frame(FB_2BPP_BYTES);
  for (size_t i = 0; i < frame.size(); ++i) frame[i] = static_cast<uint8_t>((i / 200) % 7 == 0 ? 0x1B : 0xFF);
  std::vector<uint8_t> comp(compressBound(static_cast<uLong>(frame.size())) + 64);
  z_stream zs{};
  deflateInit2(&zs, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
  zs.next_in = frame.data();
  zs.avail_in = static_cast<uInt>(frame.size());
  zs.next_out = comp.data();
  zs.avail_out = static_cast<uInt>(comp.size());
  EXPECT(deflate(&zs, Z_FINISH) == Z_STREAM_END);
  const auto compLen = static_cast<uint32_t>(zs.total_out);
  deflateEnd(&zs);
  std::vector<uint8_t> out(frame.size() + 1);
  EXPECT(inflateExact(reader, comp.data(), compLen, out.data(), static_cast<uint32_t>(frame.size())));
  EXPECT(memcmp(out.data(), frame.data(), frame.size()) == 0);
  EXPECT(compLen < 4000);  // the dashboard's win: 96 KB of mostly-white -> a few KB
  EXPECT(!inflateExact(reader, comp.data(), compLen, out.data(), static_cast<uint32_t>(frame.size() - 8)));
  EXPECT(!inflateExact(reader, comp.data(), compLen - 5, out.data(), static_cast<uint32_t>(frame.size())));
#endif
}

void testPixels() {
  beginGroup("pixels");
  std::vector<uint8_t> fb(FB_1BPP_BYTES), gray(FB_2BPP_BYTES);
  clearCanvases(true, fb.data(), gray.data());
  EXPECT(fb[0] == 0xFF && fb[FB_1BPP_BYTES - 1] == 0xFF && gray[FB_2BPP_BYTES - 1] == 0xFF);

  // 1bpp 16x2 at (8, 3).
  BlitHeader b{};
  const std::vector<uint8_t> px1 = {0x0F, 0xF0, 0x80, 0x01};
  auto p = blitPayload(8, 3, 16, 2, 0, 0, px1);
  EXPECT(parseBlit(p.data(), static_cast<uint32_t>(p.size()), b) == Status::Ok);
  applyBlit(b, b.data, fb.data(), gray.data());
  EXPECT_EQ_U(fb[3 * 100 + 1], 0x0F);
  EXPECT_EQ_U(fb[3 * 100 + 2], 0xF0);
  EXPECT_EQ_U(fb[4 * 100 + 1], 0x80);
  EXPECT_EQ_U(fb[4 * 100 + 2], 0x01);
  EXPECT_EQ_U(fb[3 * 100 + 0], 0xFF);  // untouched neighbour
  EXPECT_EQ_U(fb[3 * 100 + 3], 0xFF);
  // Mirror into gray: 0x0F -> levels 0,0,0,0,3,3,3,3 -> 0x00, 0xFF.
  EXPECT_EQ_U(gray[3 * 200 + 2], 0x00);
  EXPECT_EQ_U(gray[3 * 200 + 3], 0xFF);
  EXPECT_EQ_U(gray[4 * 200 + 2], 0xC0);  // 0x80 -> 3,0,0,0
  EXPECT_EQ_U(gray[4 * 200 + 5], 0x03);  // 0x01 -> ...,0,0,0,3

  // 2bpp 8x1 at (0, 0): levels 0,1,2,3,3,2,1,0.
  const std::vector<uint8_t> px2 = {0x1B, 0xE4};
  p = blitPayload(0, 0, 8, 1, 1, 0, px2);
  EXPECT(parseBlit(p.data(), static_cast<uint32_t>(p.size()), b) == Status::Ok);
  applyBlit(b, b.data, fb.data(), gray.data());
  EXPECT_EQ_U(gray[0], 0x1B);
  EXPECT_EQ_U(gray[1], 0xE4);
  EXPECT_EQ_U(fb[0], 0x18);  // white only where level 3: pixels 3 and 4

  std::vector<uint8_t> plane(FB_1BPP_BYTES);
  buildGrayPlane(gray.data(), plane.data(), false);
  EXPECT_EQ_U(plane[0], 0x42);  // LSB: level 1 at pixels 1 and 6
  buildGrayPlane(gray.data(), plane.data(), true);
  EXPECT_EQ_U(plane[0], 0x66);  // MSB: level 1 or 2 at pixels 1,2,5,6
  EXPECT_EQ_U(plane[1], 0x00);  // white is in neither plane
  std::vector<uint8_t> base(FB_1BPP_BYTES);
  buildGrayBase(gray.data(), base.data());
  EXPECT(base == fb);  // the base is exactly the thresholded framebuffer
  std::vector<uint8_t> gray2(FB_2BPP_BYTES);
  grayFromFramebuffer(fb.data(), gray2.data());
  EXPECT_EQ_U(gray2[0], 0x03);  // fb 0x18 -> levels 0,0,0,3 | 3,0,0,0
  EXPECT_EQ_U(gray2[1], 0xC0);
  EXPECT_EQ_U(gray2[3 * 200 + 3], 0xFF);
  std::vector<uint8_t> back(FB_1BPP_BYTES);
  buildGrayBase(gray2.data(), back.data());
  EXPECT(back == fb);  // fb -> gray -> base round-trips
  clearCanvases(false, fb.data(), gray.data());
  EXPECT(fb[123] == 0x00 && gray[456] == 0x00);
}

// --- flow control -------------------------------------------------------------

void testCreditMath() {
  beginGroup("credit-math");
  CreditReporter r(RX_WINDOW);
  EXPECT(!r.reportDue());
  r.consumed(RX_WINDOW / 4 - 1);
  EXPECT(!r.reportDue());
  r.consumed(1);
  EXPECT(r.reportDue());
  r.reported();
  EXPECT(!r.reportDue());
  EXPECT_EQ_U(r.total(), RX_WINDOW / 4);

  CreditWindow w;
  w.begin(RX_WINDOW, 1000);
  EXPECT_EQ_U(w.available(), RX_WINDOW);
  w.sent(RX_WINDOW);
  EXPECT_EQ_U(w.available(), 0);
  w.update(1000 + 1000);
  EXPECT_EQ_U(w.inFlight(), RX_WINDOW - 1000);
  EXPECT_EQ_U(w.available(), 1000);
  w.update(1000 + 500);  // stale report: ignored
  EXPECT_EQ_U(w.available(), 1000);
  w.update(1000 + RX_WINDOW + 50);  // over-report (duplicate HELLO): clamped
  EXPECT_EQ_U(w.inFlight(), 0);

  // Wrap: baseline just below 2^32.
  CreditWindow z;
  z.begin(RX_WINDOW, 0xFFFFFF00U);
  z.sent(3000);
  z.update(0xFFFFFF00U + 2000U);  // wraps past zero
  EXPECT_EQ_U(z.inFlight(), 1000);
  EXPECT_EQ_U(z.available(), RX_WINDOW - 1000);
}

// Simulates the whole flow: a host streaming a max-size BLIT through an
// HWCDC-like queue of RX_QUEUE_BYTES that the device drains only between
// long "waveform" stalls. The queue must never overflow and every byte must
// arrive intact.
void testCreditSimulation() {
  beginGroup("credit-simulation");
  std::vector<uint8_t> payload = blitPayload(0, 0, 800, 480, 1, 0, std::vector<uint8_t>(FB_2BPP_BYTES));
  for (size_t i = BLIT_HEADER_SIZE; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(rnd());
  std::vector<uint8_t> stream = frameBytes(MsgType::Blit, 42, payload);
  const auto ping = frameBytes(MsgType::Refresh, 43, {0, 0, 0, 0});
  stream.insert(stream.end(), ping.begin(), ping.end());

  const uint32_t baseline = 0xFFFFF000U;  // exercise wrap during the transfer
  CreditReporter dev(RX_WINDOW);
  dev.consumed(baseline);
  dev.reported();
  CreditWindow host;
  host.begin(RX_WINDOW, baseline);

  std::vector<uint8_t> queue;
  size_t maxQueue = 0, sent = 0;
  uint32_t lastCredit = baseline;
  bool overflow = false;
  FrameParser p(g_parseBuf.data(), g_parseBuf.size(), MAX_PAYLOAD, Direction::HostToDevice);
  std::vector<Collected> got;
  for (int tick = 0; tick < 200000 && (sent < stream.size() || !queue.empty()); ++tick) {
    // Host: push up to 64-byte USB packets while the window allows.
    size_t can = host.available();
    while (can > 0 && sent < stream.size()) {
      size_t n = 64;
      if (n > can) n = can;
      if (n > stream.size() - sent) n = stream.size() - sent;
      if (queue.size() + n > RX_QUEUE_BYTES) overflow = true;
      queue.insert(queue.end(), stream.begin() + static_cast<long>(sent), stream.begin() + static_cast<long>(sent + n));
      sent += n;
      host.sent(static_cast<uint32_t>(n));
      can -= n;
    }
    if (queue.size() > maxQueue) maxQueue = queue.size();
    // Device: stalls on 1 of 3 ticks (a blocking waveform), else drains a burst.
    if (rnd() % 3 == 0) continue;
    size_t n = 1 + rnd() % 2048;
    if (n > queue.size()) n = queue.size();
    size_t off = 0;
    while (off < n) {
      const size_t used = p.feed(queue.data() + off, n - off);
      off += used;
      while (p.hasFrame()) {
        got.push_back({p.frame().type, p.frame().seq, {}});
        if (p.frame().type == MsgType::Blit) {
          EXPECT(memcmp(p.frame().payload, payload.data(), payload.size()) == 0);
        }
        p.release();
      }
    }
    queue.erase(queue.begin(), queue.begin() + static_cast<long>(n));
    dev.consumed(static_cast<uint32_t>(n));
    // CREDIT goes out when due; drop every third report to prove cumulative
    // reports heal the loss.
    if (dev.reportDue()) {
      dev.reported();
      if (rnd() % 3 != 0) {
        lastCredit = dev.total();
        host.update(lastCredit);
      }
    }
    // Host PING/PONG fallback when stalled with nothing in the queue.
    if (host.available() == 0 && queue.empty()) host.update(dev.total());
  }
  EXPECT(!overflow);
  EXPECT(maxQueue <= RX_QUEUE_BYTES);
  EXPECT_EQ_U(sent, stream.size());
  EXPECT_EQ_U(got.size(), 2);
  EXPECT_EQ_U(p.stats().crcErrors, 0);
}

// --- wear policy --------------------------------------------------------------

void testConfig() {
  beginGroup("config");
  WearConfig cfg;
  EXPECT_EQ_U(cfg.minGapMs, 500);
  EXPECT_EQ_U(cfg.fullEveryN, 60);
  EXPECT_EQ_U(cfg.fullEverySec, 1800);
  uint8_t p[8];
  wr16(p + 0, 5);      // below the 20 floor
  wr16(p + 2, 60);     // below the 5 min floor
  wr16(p + 4, 100);    // below the 300 ms floor
  p[6] = 0x01;
  p[7] = 0;
  EXPECT(applyConfig(cfg, p));
  EXPECT_EQ_U(cfg.fullEveryN, FLOOR_FULL_EVERY_N);
  EXPECT_EQ_U(cfg.fullEverySec, FLOOR_FULL_EVERY_S);
  EXPECT_EQ_U(cfg.minGapMs, FLOOR_MIN_GAP_MS);
  EXPECT_EQ_U(cfg.inputMask, INPUT_MASK_BUTTONS);
  wr16(p + 0, 0);  // 0 = keep
  wr16(p + 2, 900);
  wr16(p + 4, 1000);
  p[6] = 0x03;
  EXPECT(!applyConfig(cfg, p));
  EXPECT_EQ_U(cfg.fullEveryN, FLOOR_FULL_EVERY_N);
  EXPECT_EQ_U(cfg.fullEverySec, 900);
  EXPECT_EQ_U(cfg.minGapMs, 1000);
  wr16(p + 0, 60000);  // above the ceiling
  p[6] = 0xFF;          // unknown mask bits
  EXPECT(applyConfig(cfg, p));
  EXPECT_EQ_U(cfg.fullEveryN, CEIL_FULL_EVERY_N);
  EXPECT_EQ_U(cfg.inputMask, 0x03);
  uint8_t out[8];
  encodeConfig(cfg, out);
  WearConfig round;
  EXPECT(!applyConfig(round, out));
  EXPECT(round.fullEveryN == cfg.fullEveryN && round.fullEverySec == cfg.fullEverySec &&
         round.minGapMs == cfg.minGapMs && round.inputMask == cfg.inputMask);
}

void testPolicyGapAndCoalescing() {
  beginGroup("policy-gap");
  RefreshPolicy pol;
  pol.begin(0);
  RefreshPolicy::Action a{};
  EXPECT(!pol.due(0, a));  // nothing requested: the device never refreshes on its own
  pol.request(RefreshMode::Fast, 1);
  EXPECT(pol.due(10, a));  // first waveform needs no gap
  EXPECT(a.mode == RefreshMode::Fast && a.seq == 1 && !a.coalesced && !a.promoted);
  pol.executed(a, 1000);
  EXPECT(!pol.pending());
  // Three requests inside the gap coalesce into one waveform.
  pol.request(RefreshMode::Fast, 2);
  pol.request(RefreshMode::Full, 3);
  pol.request(RefreshMode::Fast, 4);
  EXPECT(!pol.due(1000 + 499, a));
  EXPECT(pol.due(1000 + 500, a));
  EXPECT_EQ_U(a.seq, 4);              // latest wins
  EXPECT(a.mode == RefreshMode::Full);  // a coalesced FAST never downgrades FULL
  EXPECT(a.coalesced);
  pol.executed(a, 3000);
  // Nothing due until asked again, however much time passes.
  EXPECT(!pol.due(3000 + 10000000, a));
  // A custom gap from CONFIG is honoured.
  WearConfig cfg;
  cfg.minGapMs = 2000;
  pol.setConfig(cfg);
  pol.request(RefreshMode::Gray, 5);
  EXPECT(!pol.due(3000 + 1999, a));
  EXPECT(pol.due(3000 + 2000, a));
  EXPECT(a.mode == RefreshMode::Gray);
  // Millis wrap: gap arithmetic is modular.
  RefreshPolicy w;
  w.begin(0xFFFFFF00U);
  w.request(RefreshMode::Fast, 1);
  EXPECT(w.due(0xFFFFFF00U, a));
  w.executed(a, 0xFFFFFFF0U);
  w.request(RefreshMode::Fast, 2);
  EXPECT(!w.due(0x100U, a));  // 0x110 ms later
  EXPECT(w.due(0x1E4U, a));   // 0x1F4 = 500 ms later
}

void testPolicyScrub() {
  beginGroup("policy-scrub");
  RefreshPolicy pol;
  pol.begin(0);
  RefreshPolicy::Action a{};
  uint32_t t = 0;
  uint16_t seq = 0;
  auto runOne = [&](RefreshMode m) {
    pol.request(m, ++seq);
    t += 600;
    const bool ok = pol.due(t, a);
    pol.executed(a, t);
    return ok;
  };
  for (int i = 0; i < 60; ++i) {
    EXPECT(runOne(RefreshMode::Fast));
    EXPECT(a.mode == RefreshMode::Fast);
  }
  EXPECT_EQ_U(pol.fastSinceClean(), 60);
  EXPECT(runOne(RefreshMode::Fast));  // the 61st is promoted
  EXPECT(a.mode == RefreshMode::Full && a.promoted);
  EXPECT_EQ_U(pol.fastSinceClean(), 0);
  EXPECT_EQ_U(pol.scrubs(), 1);

  // Time rule: one FAST, then 30 min later the next FAST is promoted.
  EXPECT(runOne(RefreshMode::Fast));
  EXPECT(a.mode == RefreshMode::Fast);
  t += 30 * 60 * 1000;
  EXPECT(runOne(RefreshMode::Fast));
  EXPECT(a.mode == RefreshMode::Full && a.promoted);
  // ...but a long idle with no partials since the last clean does not scrub.
  t += 2 * 60 * 60 * 1000;
  EXPECT(runOne(RefreshMode::Fast));
  EXPECT(a.mode == RefreshMode::Fast && !a.promoted);

  // HALF and GRAY are clearing waveforms: they reset the counter.
  for (int i = 0; i < 50; ++i) runOne(RefreshMode::Fast);
  EXPECT(runOne(RefreshMode::Half));
  EXPECT_EQ_U(pol.fastSinceClean(), 0);
  for (int i = 0; i < 50; ++i) runOne(RefreshMode::Fast);
  EXPECT(runOne(RefreshMode::Gray));
  EXPECT(a.mode == RefreshMode::Gray && !a.promoted);
  EXPECT_EQ_U(pol.fastSinceClean(), 0);

  // The floor holds: the most aggressive CONFIG still scrubs every 20.
  WearConfig cfg;
  uint8_t p[8] = {1, 0, 0, 0, 0, 0, 3, 0};
  applyConfig(cfg, p);
  pol.setConfig(cfg);
  for (int i = 0; i < 20; ++i) {
    runOne(RefreshMode::Fast);
    EXPECT(!a.promoted);
  }
  runOne(RefreshMode::Fast);
  EXPECT(a.promoted);
}

}  // namespace

int main() {
  testCrc();
  testRoundTrip();
  testResync();
  testCrcRejection();
  testOversize();
  testBlitValidation();
  testInflate();
  testPixels();
  testCreditMath();
  testCreditSimulation();
  testConfig();
  testPolicyGapAndCoalescing();
  testPolicyScrub();
  printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
