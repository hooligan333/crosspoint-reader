#pragma once
#if defined(CROSSPOINT_RSS_SYNC) || defined(CROSSPOINT_FLASHCARDS)

#include <expat.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Per-env item cap. The default is unchanged at 100, so every env that does not
// define this builds exactly as it did before; only [env:x4-rss] overrides it.
//
// WHY IT IS OVERRIDABLE: the cap's cost is not the same on every device. The
// combo (S3 + 8 MB PSRAM) envs can afford 100. The original X4 is an ESP32-C3
// with ~380 KB RAM, no PSRAM and a heap a reading session leaves around 50 KB
// free and fragmented — and the fetch cascade costs roughly 484 B per item with
// typical field lengths (RssItem slot 100 + Row slot 88 + the title, URL, guid,
// pubDate and filename strings), so 100 items would ask for more than the whole
// free heap. See the comment on [env:x4-rss] in platformio.ini.
#ifndef CROSSPOINT_RSS_MAX_ITEMS
#define CROSSPOINT_RSS_MAX_ITEMS 100
#endif

/**
 * One <item> of an RSS 2.0 feed, reduced to the fields the sync screen needs.
 * Everything else in the document is ignored.
 */
struct RssItem {
  std::string title;         // <title>, UTF-8, bounded to MAX_TITLE_BYTES
  std::string guid;          // <guid>, empty when the feed omits it
  std::string pubDate;       // <pubDate>, kept raw (RFC-822)
  std::string enclosureUrl;  // <enclosure url="...">
  uint32_t enclosureLength = 0;

  // Feed identity: the <guid> when present, the enclosure URL otherwise.
  const std::string& identity() const { return guid.empty() ? enclosureUrl : guid; }
};

/**
 * Render an RFC-822 pubDate as a short "12 Aug" label.
 *
 * Writes at most `size` bytes and always NUL-terminates. `out` is left empty
 * when the string is not a recognisable RFC-822 date; the year is parsed past
 * but never displayed, so it is not returned.
 */
void rssShortDate(const std::string& pubDate, char* out, size_t size);

/**
 * Streaming RSS 2.0 parser, built on the same expat push model as OpdsParser:
 * the caller pushes HTTP body chunks through feed() as they arrive and calls
 * finish() at end of stream, so the document is never buffered whole.
 *
 * Malformed XML sets error(); the caller is expected to discard the partial
 * result rather than use it.
 *
 * Shared with CROSSPOINT_FLASHCARDS: the deck feed is the same RSS 2.0
 * document with a different <enclosure type> (DECK_SERVER_SPEC.md §2), and the
 * type attribute is advisory here — so FlashcardSyncActivity reuses this
 * parser verbatim instead of duplicating the handler set (~1.9 KB of IROM by
 * the RSS measurement in platformio.ini) for an identical grammar.
 *
 *   RssParser parser;
 *   HttpDownloader::fetchUrl(url, [&](const uint8_t* d, size_t n) { return parser.feed(d, n); });
 *   if (parser.finish() && !parser.error()) { ... parser.takeItems() ... }
 */
class RssParser final {
 public:
  // Feeds longer than this are truncated: the sync list is a hand-curated
  // selection, and 100 rows already costs a bounded ~25 KB of item strings.
  // Overridden per env through CROSSPOINT_RSS_MAX_ITEMS above.
  static constexpr size_t MAX_ITEMS = CROSSPOINT_RSS_MAX_ITEMS;

  RssParser();
  ~RssParser();
  RssParser(const RssParser&) = delete;
  RssParser& operator=(const RssParser&) = delete;

  // Push one body chunk. Returns false once the parse has failed, which also
  // aborts an HttpDownloader::DataCallback transfer.
  bool feed(const uint8_t* data, size_t length);
  // Close the document. Returns false on error.
  bool finish();

  bool error() const { return errorOccured; }
  // True when the feed carried more than MAX_ITEMS entries.
  bool truncated() const { return feedTruncated; }

  std::vector<RssItem> takeItems() { return std::move(items); }

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL endElement(void* userData, const XML_Char* name);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);

  static const char* findAttribute(const XML_Char** atts, const char* name);
  static void appendBounded(std::string& target, const char* value, size_t len, size_t maxLen);

  // Which text element the character-data handler is currently filling.
  enum class Field : uint8_t { None, Title, Guid, PubDate };

  XML_Parser parser = nullptr;
  std::vector<RssItem> items;
  RssItem current;
  std::string text;

  Field field = Field::None;
  bool inItem = false;
  bool collect = false;  // inside an item that still fits under MAX_ITEMS
  bool errorOccured = false;
  bool feedTruncated = false;
};

#endif  // CROSSPOINT_RSS_SYNC || CROSSPOINT_FLASHCARDS
