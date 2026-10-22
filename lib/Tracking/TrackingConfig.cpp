#include "TrackingConfig.h"

#ifdef CROSSPOINT_TRACKING

#include <cstdlib>
#include <cstring>

namespace tracking {

namespace {

struct TypeEntry {
  const char* name;
  ItemType type;
};

constexpr TypeEntry TYPES[] = {
    {"number", ItemType::Number},     {"text", ItemType::Text},       {"date", ItemType::Date},
    {"datetime", ItemType::DateTime}, {"boolean", ItemType::Boolean}, {"stars", ItemType::Stars},
    {"list", ItemType::List},         {"none", ItemType::None},
};

bool isDigit(const char c) { return c >= '0' && c <= '9'; }

// RFC 8259 number grammar. The tokenizer collects any run of [0-9.+-eE], so
// "1-2" or "1.2.3" reach the callback and must be refused here.
bool isJsonNumber(const char* s, const size_t len) {
  size_t i = 0;
  if (i < len && s[i] == '-') i++;
  if (i >= len) return false;
  if (s[i] == '0') {
    i++;
  } else if (isDigit(s[i])) {
    while (i < len && isDigit(s[i])) i++;
  } else {
    return false;
  }
  if (i < len && s[i] == '.') {
    i++;
    if (i >= len || !isDigit(s[i])) return false;
    while (i < len && isDigit(s[i])) i++;
  }
  if (i < len && (s[i] == 'e' || s[i] == 'E')) {
    i++;
    if (i < len && (s[i] == '+' || s[i] == '-')) i++;
    if (i >= len || !isDigit(s[i])) return false;
    while (i < len && isDigit(s[i])) i++;
  }
  return i == len;
}

void copyBounded(char* dst, const size_t dstBytes, const char* src, const size_t len) {
  const size_t n = len < dstBytes - 1 ? len : dstBytes - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

}  // namespace

const char* typeName(const ItemType type) {
  for (const auto& entry : TYPES) {
    if (entry.type == type) return entry.name;
  }
  return "";
}

bool parseTypeName(const char* name, ItemType& out) {
  for (const auto& entry : TYPES) {
    if (strcmp(entry.name, name) == 0) {
      out = entry.type;
      return true;
    }
  }
  return false;
}

void Config::clear() {
  itemCount = 0;
  skippedCount = 0;
  optionPoolUsed = 0;
  optionTotal = 0;
}

const char* Config::option(const Item& item, const int index) const {
  if (index < 0 || index >= item.optionCount) return "";
  const int slot = item.firstOption + index;
  if (slot >= optionTotal) return "";
  return optionPool + optionStart[slot];
}

ConfigParser::ConfigParser(Config& out)
    : config(out),
      parser(JsonCallbacks{this, &ConfigParser::sOnKey, &ConfigParser::sOnString, &ConfigParser::sOnNumber,
                           &ConfigParser::sOnBool, &ConfigParser::sOnNull, &ConfigParser::sOnObjectStart,
                           &ConfigParser::sOnObjectEnd, &ConfigParser::sOnArrayStart, &ConfigParser::sOnArrayEnd}) {
  config.clear();
}

void ConfigParser::feed(const char* data, const size_t len) {
  // One byte at a time so every callback knows the line it fired on.
  for (size_t i = 0; i < len && error == ParseError::None; i++) {
    if (bytesFed >= MAX_CONFIG_BYTES) {
      fail(ParseError::TooLarge);
      return;
    }
    if (!scanRawByte(data[i])) {
      fail(ParseError::Syntax);
      return;
    }
    parser.feed(data + i, 1);
    bytesFed++;
    if (parser.hasError()) fail(ParseError::Syntax);
    const char c = data[i];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') sawContent = true;
    if (c == '\n') line++;
  }
}

// The tokenizer silently skips any byte it cannot place, so ".5", "2,5",
// "1 000", "5x" or "0x10" would read as 5, 2, 1, 5 or 0. This tracks the JSON
// grammar byte by byte inside the root object: a value may only start where
// one is expected, a key only where a key is expected, and a finished value
// must be followed by ',', '}' or ']'. A closer the structure checks classify
// (Mismatch, MissingValue) is let through so they still report it; a trailing
// comma is refused here.
bool ConfigParser::scanRawByte(const char c) {
  switch (raw) {
    case Raw::String:
      if (rawEscaped) {
        rawEscaped = false;
      } else if (c == '\\') {
        rawEscaped = true;
      } else if (c == '"') {
        raw = rawStringIsKey ? Raw::AfterKey : (rawDepth == 0 ? Raw::Free : Raw::AfterValue);
      }
      return true;
    case Raw::Literal:
      // The tokenizer rejects a wrong letter; only the length is tracked.
      if (--rawLiteralLeft == 0) raw = Raw::AfterValue;
      return true;
    case Raw::Number:
      if (isDigit(c) || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') return true;
      raw = Raw::AfterValue;  // this byte ends the number; check it below
      break;
    default:
      break;
  }
  if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return true;

  const bool afterComma = rawLastStruct == ',';
  switch (raw) {
    case Raw::Value:
      if (c == '-' || isDigit(c)) {
        raw = Raw::Number;
        return true;
      }
      if (c == 't' || c == 'n' || c == 'f') {
        rawLiteralLeft = c == 'f' ? 4 : 3;
        raw = Raw::Literal;
        return true;
      }
      if (!(c == '"' || c == '{' || c == '[' || c == '}' || (c == ']' && !afterComma))) return false;
      break;
    case Raw::Key:
      if (!(c == '"' || c == ']' || (c == '}' && !afterComma))) return false;
      break;
    case Raw::AfterKey:
      if (!(c == ':' || c == '"' || c == ',' || c == '}' || c == ']')) return false;
      break;
    case Raw::AfterValue:
      if (!(c == ',' || c == '}' || c == ']')) return false;
      break;
    default:  // Free
      break;
  }

  switch (c) {
    case '"':
      rawStringIsKey = raw == Raw::Key || raw == Raw::AfterKey;
      raw = Raw::String;
      break;
    case '{':
    case '[':
      if (rawDepth >= 32) return false;  // the tokenizer's nesting cap
      if (c == '[') {
        rawArrayBits |= 1u << rawDepth;
      } else {
        rawArrayBits &= ~(1u << rawDepth);
      }
      rawDepth++;
      raw = c == '[' ? Raw::Value : Raw::Key;
      rawLastStruct = c;
      break;
    case '}':
    case ']':
      if (rawDepth > 0) rawDepth--;
      raw = rawDepth == 0 ? Raw::Free : Raw::AfterValue;
      break;
    case ':':
      if (raw != Raw::Free) {
        raw = Raw::Value;
        rawLastStruct = c;
      }
      break;
    case ',':
      if (raw != Raw::Free) {
        raw = rawDepth > 0 && (rawArrayBits >> (rawDepth - 1) & 1u) != 0 ? Raw::Value : Raw::Key;
        rawLastStruct = c;
      }
      break;
    default:
      break;
  }
  return true;
}

ParseError ConfigParser::finish() {
  if (error != ParseError::None) return error;
  // A number is only emitted when the next byte ends it; a trailing space
  // flushes one that ends the file (it can only be a top-level value).
  parser.feed(" ", 1);
  if (error != ParseError::None) return error;
  if (parser.hasError()) {
    fail(ParseError::Syntax);
    return error;
  }
  if (depth > 0) {
    fail(ParseError::Truncated);
    return error;
  }
  if (!rootSeen) {
    // Text that never opened a value (a bare word or string) is not an
    // empty file.
    fail(sawContent ? ParseError::RootNotObject : ParseError::Empty);
    return error;
  }
  if (!itemsSeen) {
    error = ParseError::NoItems;
    errLine = 0;
  }
  return error;
}

void ConfigParser::fail(const ParseError err) {
  if (error != ParseError::None) return;
  error = err;
  errLine = line;
}

ConfigParser::Context ConfigParser::topContext() const {
  if (depth == 0 || depth > TRACKED_LEVELS) return Context::Other;
  return contexts[depth - 1];
}

void ConfigParser::push(const bool isArray, const Context context) {
  if (depth >= 32) {
    fail(ParseError::Syntax);
    return;
  }
  if (isArray) {
    arrayBits |= 1u << depth;
  } else {
    arrayBits &= ~(1u << depth);
  }
  if (depth < TRACKED_LEVELS) contexts[depth] = context;
  depth++;
}

// --- SAX trampolines ---

void ConfigParser::sOnKey(void* ctx, const char* key, size_t) {
  auto* self = static_cast<ConfigParser*>(ctx);
  if (self->error != ParseError::None || self->depth == 0 || self->topIsArray()) return;
  if (self->awaitingValue) {
    self->fail(ParseError::MissingValue);
    return;
  }
  self->awaitingValue = true;
  self->pendingKey = Key::Other;
  switch (self->topContext()) {
    case Context::Root:
      if (strcmp(key, "items") == 0) self->pendingKey = Key::Items;
      break;
    case Context::Item:
      if (strcmp(key, "label") == 0) {
        self->pendingKey = Key::Label;
      } else if (strcmp(key, "type") == 0) {
        self->pendingKey = Key::Type;
      } else if (strcmp(key, "id") == 0) {
        self->pendingKey = Key::Id;
      } else if (strcmp(key, "options") == 0) {
        self->pendingKey = Key::Options;
      } else if (strcmp(key, "min") == 0) {
        self->pendingKey = Key::Min;
      } else if (strcmp(key, "max") == 0) {
        self->pendingKey = Key::Max;
      } else if (strcmp(key, "integer") == 0) {
        self->pendingKey = Key::Integer;
      }
      break;
    default:
      break;
  }
}

void ConfigParser::sOnString(void* ctx, const char* value, const size_t len) {
  static_cast<ConfigParser*>(ctx)->onValue(ValueKind::String, value, len, false);
}

void ConfigParser::sOnNumber(void* ctx, const char* value, const size_t len) {
  auto* self = static_cast<ConfigParser*>(ctx);
  if (!isJsonNumber(value, len)) {
    self->fail(ParseError::Syntax);
    return;
  }
  self->onValue(ValueKind::Number, value, len, false);
}

void ConfigParser::sOnBool(void* ctx, const bool value) {
  static_cast<ConfigParser*>(ctx)->onValue(ValueKind::Bool, nullptr, 0, value);
}

void ConfigParser::sOnNull(void* ctx) { static_cast<ConfigParser*>(ctx)->onValue(ValueKind::Null, nullptr, 0, false); }

void ConfigParser::sOnObjectStart(void* ctx) {
  static_cast<ConfigParser*>(ctx)->onValue(ValueKind::Object, nullptr, 0, false);
}

void ConfigParser::sOnObjectEnd(void* ctx) { static_cast<ConfigParser*>(ctx)->onClose(false); }

void ConfigParser::sOnArrayStart(void* ctx) {
  static_cast<ConfigParser*>(ctx)->onValue(ValueKind::Array, nullptr, 0, false);
}

void ConfigParser::sOnArrayEnd(void* ctx) { static_cast<ConfigParser*>(ctx)->onClose(true); }

// --- Structure ---

void ConfigParser::onValue(const ValueKind kind, const char* text, const size_t len, const bool flag) {
  if (error != ParseError::None) return;
  const bool container = kind == ValueKind::Object || kind == ValueKind::Array;
  const bool isArray = kind == ValueKind::Array;

  if (depth == 0) {
    if (rootSeen) {
      fail(ParseError::TrailingContent);
      return;
    }
    rootSeen = true;
    if (kind != ValueKind::Object) {
      fail(ParseError::RootNotObject);
      return;
    }
    push(false, Context::Root);
    return;
  }

  const Context context = topContext();

  if (topIsArray()) {
    if (context == Context::ItemsArray) {
      if (kind == ValueKind::Object) {
        beginItem();
        push(false, Context::Item);
        return;
      }
      if (config.skippedCount < UINT8_MAX) config.skippedCount++;
    } else if (context == Context::Options) {
      if (kind == ValueKind::String) {
        addOption(text, len);
      } else {
        optionsBroken = true;
      }
    }
    if (container) push(isArray, Context::Other);
    return;
  }

  // Inside an object: a value must answer a key.
  if (!awaitingValue) {
    fail(ParseError::Syntax);
    return;
  }
  awaitingValue = false;
  const Key key = pendingKey;

  if (context == Context::Root && key == Key::Items) {
    if (!isArray) {
      fail(ParseError::ItemsNotArray);
      return;
    }
    itemsSeen = true;
    push(true, Context::ItemsArray);
    return;
  }

  if (context == Context::Item) {
    switch (key) {
      case Key::Label:
        if (kind != ValueKind::String || len == 0 || len > MAX_LABEL_BYTES) {
          itemBroken = true;
        } else {
          copyBounded(current.label, sizeof(current.label), text, len);
        }
        break;
      case Key::Type:
        if (kind != ValueKind::String || !parseTypeName(text, current.type)) {
          itemBroken = true;
        } else {
          typeSeen = true;
        }
        break;
      case Key::Id:
        if (kind == ValueKind::Null) {
          current.id[0] = '\0';
        } else if (kind != ValueKind::String || len > MAX_ID_BYTES) {
          itemBroken = true;
        } else {
          copyBounded(current.id, sizeof(current.id), text, len);
        }
        break;
      case Key::Options:
        // A repeated key replaces the earlier list rather than appending to it.
        discardItemOptions();
        optionsBroken = false;
        if (isArray) {
          push(true, Context::Options);
          return;
        }
        if (kind != ValueKind::Null) optionsBroken = true;
        break;
      case Key::Min:
      case Key::Max: {
        if (kind == ValueKind::Null) break;
        if (kind != ValueKind::Number) {
          numberRuleBroken = true;
          break;
        }
        const double value = strtod(text, nullptr);
        if (key == Key::Min) {
          current.hasMin = true;
          current.minValue = value;
        } else {
          current.hasMax = true;
          current.maxValue = value;
        }
        break;
      }
      case Key::Integer:
        if (kind == ValueKind::Bool) {
          current.integerOnly = flag;
        } else if (kind != ValueKind::Null) {
          numberRuleBroken = true;
        }
        break;
      case Key::Items:
      case Key::Other:
        break;
    }
  }

  if (container) push(isArray, Context::Other);
}

void ConfigParser::onClose(const bool isArray) {
  if (error != ParseError::None) return;
  if (depth == 0 || topIsArray() != isArray) {
    fail(ParseError::Mismatch);
    return;
  }
  if (!isArray && awaitingValue) {
    fail(ParseError::MissingValue);
    return;
  }
  const Context context = topContext();
  depth--;
  if (context == Context::Item) endItem();
}

// --- Items ---

void ConfigParser::beginItem() {
  current = Item{};
  itemBroken = false;
  typeSeen = false;
  optionsBroken = false;
  numberRuleBroken = false;
  itemPoolMark = config.optionPoolUsed;
  itemOptionMark = config.optionTotal;
  current.firstOption = itemOptionMark;
}

void ConfigParser::discardItemOptions() {
  config.optionPoolUsed = itemPoolMark;
  config.optionTotal = itemOptionMark;
  current.firstOption = itemOptionMark;
  current.optionCount = 0;
}

void ConfigParser::addOption(const char* text, const size_t len) {
  if (optionsBroken) return;
  if (len == 0 || len > MAX_OPTION_BYTES || current.optionCount >= MAX_OPTIONS ||
      config.optionTotal >= MAX_TOTAL_OPTIONS || config.optionPoolUsed + len + 1 > OPTION_POOL_BYTES) {
    optionsBroken = true;
    return;
  }
  config.optionStart[config.optionTotal++] = config.optionPoolUsed;
  memcpy(config.optionPool + config.optionPoolUsed, text, len);
  config.optionPool[config.optionPoolUsed + len] = '\0';
  config.optionPoolUsed = static_cast<uint16_t>(config.optionPoolUsed + len + 1);
  current.optionCount++;
}

void ConfigParser::endItem() {
  bool valid = !itemBroken && current.label[0] != '\0' && typeSeen;
  if (valid && current.type == ItemType::List) valid = !optionsBroken && current.optionCount > 0;
  if (valid && current.type == ItemType::Number) {
    valid = !numberRuleBroken && !(current.hasMin && current.hasMax && current.minValue > current.maxValue);
  }
  if (!valid || config.itemCount >= MAX_ITEMS) {
    discardItemOptions();
    if (config.skippedCount < UINT8_MAX) config.skippedCount++;
    return;
  }
  if (current.type != ItemType::List) discardItemOptions();
  if (current.type != ItemType::Number) {
    current.hasMin = current.hasMax = current.integerOnly = false;
  }
  config.items[config.itemCount++] = current;
}

}  // namespace tracking

#endif  // CROSSPOINT_TRACKING
