#include "RssParser.h"

#ifdef CROSSPOINT_RSS_SYNC

#include <Logging.h>
#include <Utf8.h>
#include <XmlParserUtils.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
// Per-field byte caps. Titles are display-only and truncated to a whole UTF-8
// codepoint when the cap lands mid-sequence.
constexpr size_t MAX_TITLE_BYTES = 120;
constexpr size_t MAX_GUID_BYTES = 128;
constexpr size_t MAX_DATE_BYTES = 40;
constexpr size_t MAX_URL_BYTES = 384;
// Same push size OpdsParser uses; keeps XML_GetBuffer's request bounded
// regardless of the HTTP read chunk that arrives.
constexpr size_t PARSE_CHUNK = 1024;
}  // namespace

void rssShortDate(const std::string& pubDate, char* out, const size_t size) {
  if (size == 0) return;
  out[0] = '\0';

  // RFC-822: "[Wed, ]12 Aug 2026 10:30:00 +0000". The day name is optional, so
  // start after the comma when there is one.
  const char* p = pubDate.c_str();
  if (const char* comma = strchr(p, ',')) p = comma + 1;
  while (*p == ' ') ++p;

  int day = 0;
  int digits = 0;
  while (digits < 2 && *p >= '0' && *p <= '9') {
    day = day * 10 + (*p - '0');
    ++p;
    ++digits;
  }
  if (digits == 0 || day < 1 || day > 31) return;

  while (*p == ' ') ++p;
  for (int i = 0; i < 3; ++i) {
    if (isalpha(static_cast<unsigned char>(p[i])) == 0) return;
  }
  snprintf(out, size, "%d %.3s", day, p);
}

RssParser::RssParser() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    errorOccured = true;
    LOG_ERR("RSS", "Couldn't allocate memory for parser");
    return;
  }
  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
}

RssParser::~RssParser() { destroyXmlParser(parser); }

bool RssParser::feed(const uint8_t* data, const size_t length) {
  if (errorOccured || !parser) return false;

  const char* pos = reinterpret_cast<const char*>(data);
  size_t remaining = length;
  while (remaining > 0) {
    const size_t toRead = remaining < PARSE_CHUNK ? remaining : PARSE_CHUNK;
    void* const buf = XML_GetBuffer(parser, toRead);
    if (!buf) {
      errorOccured = true;
      LOG_ERR("RSS", "Couldn't allocate memory for buffer");
      destroyXmlParser(parser);
      return false;
    }
    memcpy(buf, pos, toRead);
    if (XML_ParseBuffer(parser, static_cast<int>(toRead), 0) == XML_STATUS_ERROR) {
      errorOccured = true;
      LOG_ERR("RSS", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      return false;
    }
    pos += toRead;
    remaining -= toRead;
  }
  return true;
}

bool RssParser::finish() {
  if (errorOccured || !parser) return false;
  if (XML_Parse(parser, nullptr, 0, XML_TRUE) != XML_STATUS_OK) {
    errorOccured = true;
    LOG_ERR("RSS", "Incomplete document: %s", XML_ErrorString(XML_GetErrorCode(parser)));
    destroyXmlParser(parser);
    return false;
  }
  return true;
}

const char* RssParser::findAttribute(const XML_Char** atts, const char* name) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

void RssParser::appendBounded(std::string& target, const char* value, const size_t len, const size_t maxLen) {
  if (target.size() >= maxLen) return;
  const size_t remaining = maxLen - target.size();
  target.append(value, len < remaining ? len : remaining);
}

void XMLCALL RssParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<RssParser*>(userData);

  if (xmlLocalNameEquals(name, "item")) {
    self->inItem = true;
    self->collect = self->items.size() < MAX_ITEMS;
    if (!self->collect && !self->feedTruncated) {
      LOG_INF("RSS", "Feed has more than %u items; ignoring the rest", (unsigned)MAX_ITEMS);
    }
    self->feedTruncated = self->feedTruncated || !self->collect;
    self->current = RssItem{};
    self->text.clear();
    self->field = Field::None;
    return;
  }

  if (!self->inItem || !self->collect) return;

  if (xmlLocalNameEquals(name, "enclosure")) {
    // First enclosure with a url wins; the server contract is one ready-made
    // .epub per item, and the type attribute is advisory.
    if (self->current.enclosureUrl.empty()) {
      if (const char* url = findAttribute(atts, "url")) {
        appendBounded(self->current.enclosureUrl, url, strnlen(url, MAX_URL_BYTES), MAX_URL_BYTES);
      }
      if (const char* length = findAttribute(atts, "length")) {
        self->current.enclosureLength = static_cast<uint32_t>(strtoul(length, nullptr, 10));
      }
    }
    return;
  }

  if (xmlLocalNameEquals(name, "title")) {
    self->field = Field::Title;
  } else if (xmlLocalNameEquals(name, "guid")) {
    self->field = Field::Guid;
  } else if (xmlLocalNameEquals(name, "pubDate")) {
    self->field = Field::PubDate;
  } else {
    return;
  }
  self->text.clear();
}

void XMLCALL RssParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<RssParser*>(userData);

  if (xmlLocalNameEquals(name, "item")) {
    // An item without a title or without an enclosure has nothing to show or
    // download; drop it rather than render a blank row.
    if (self->collect && !self->current.title.empty() && !self->current.enclosureUrl.empty()) {
      self->items.push_back(std::move(self->current));
    }
    self->current = RssItem{};
    self->inItem = false;
    self->collect = false;
    self->field = Field::None;
    return;
  }

  if (!self->inItem || !self->collect || self->field == Field::None) return;

  switch (self->field) {
    case Field::Title:
      if (!xmlLocalNameEquals(name, "title")) return;
      // The byte cap can land inside a multi-byte codepoint; drop the partial
      // sequence so the row renders without a replacement glyph.
      self->text.resize(
          static_cast<size_t>(utf8SafeTruncateBuffer(self->text.data(), static_cast<int>(self->text.size()))));
      self->current.title = self->text;
      break;
    case Field::Guid:
      if (!xmlLocalNameEquals(name, "guid")) return;
      self->current.guid = self->text;
      break;
    case Field::PubDate:
      if (!xmlLocalNameEquals(name, "pubDate")) return;
      self->current.pubDate = self->text;
      break;
    case Field::None:
      return;
  }
  self->field = Field::None;
}

void XMLCALL RssParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<RssParser*>(userData);
  if (!self->collect) return;
  switch (self->field) {
    case Field::Title:
      appendBounded(self->text, s, len, MAX_TITLE_BYTES);
      break;
    case Field::Guid:
      appendBounded(self->text, s, len, MAX_GUID_BYTES);
      break;
    case Field::PubDate:
      appendBounded(self->text, s, len, MAX_DATE_BYTES);
      break;
    case Field::None:
      break;
  }
}

#endif  // CROSSPOINT_RSS_SYNC
