#pragma once

// Tracking (CROSSPOINT_TRACKING): the item list the owner keeps in
// /tracking/config.json, parsed into one fixed-size Config.
//
// Device-independent so test/tracking_logic exercises the exact parser the
// firmware runs. The .cpp is wrapped in CROSSPOINT_TRACKING: PlatformIO
// compiles all of lib/ into every environment, and flag-off images must not
// change.
//
// Schema: { "items": [ { "label": "...", "type": "...", ... }, ... ] }
//   label    required string, 1..MAX_LABEL_BYTES bytes
//   type     required: number text date datetime boolean stars list none
//   id       optional string, 1..MAX_ID_BYTES bytes; logged instead of label
//   options  list only, required: 1..MAX_OPTIONS strings, each
//            1..MAX_OPTION_BYTES bytes
//   min/max  number only, optional numbers; min must not exceed max
//   integer  number only, optional bool
//   timestamp_override
//            optional bool, any type; true lets the entry screen set the
//            row's timestamp column instead of taking the clock's now.
//            Any other value, null included, breaks the item
// Unknown keys are ignored. An item that breaks a rule, or arrives after
// MAX_ITEMS valid ones, is skipped and counted in Config::skippedCount; the
// file as a whole fails only on broken JSON or a missing "items" array.

#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>

namespace tracking {

constexpr size_t MAX_ITEMS = 32;
constexpr size_t MAX_LABEL_BYTES = 63;
constexpr size_t MAX_ID_BYTES = 31;
constexpr size_t MAX_OPTIONS = 20;
constexpr size_t MAX_OPTION_BYTES = 47;
// Shared by every list item's option text (NUL-terminated).
constexpr size_t OPTION_POOL_BYTES = 2048;
constexpr size_t MAX_TOTAL_OPTIONS = 128;
// Parse time stays bounded on a pathological file.
constexpr size_t MAX_CONFIG_BYTES = 32 * 1024;

enum class ItemType : uint8_t { Number, Text, Date, DateTime, Boolean, Stars, List, None };

/** The schema / CSV spelling of a type. */
const char* typeName(ItemType type);
bool parseTypeName(const char* name, ItemType& out);

struct Item {
  char label[MAX_LABEL_BYTES + 1];
  char id[MAX_ID_BYTES + 1];
  double minValue;
  double maxValue;
  uint16_t firstOption;  // index into Config::optionStart
  uint8_t optionCount;
  ItemType type;
  bool hasMin;
  bool hasMax;
  bool integerOnly;
  bool timestampOverride;

  /** The CSV `label` column: the stable id when given, else the label. */
  const char* key() const { return id[0] != '\0' ? id : label; }
};

struct Config {
  Item items[MAX_ITEMS];
  char optionPool[OPTION_POOL_BYTES];
  uint16_t optionStart[MAX_TOTAL_OPTIONS];  // offsets into optionPool
  uint16_t optionPoolUsed;
  uint16_t optionTotal;
  uint8_t itemCount;
  uint8_t skippedCount;

  void clear();
  const char* option(const Item& item, int index) const;
};

enum class ParseError : uint8_t {
  None,
  Empty,            // no JSON value at all
  Syntax,           // the tokenizer rejected the text
  Mismatch,         // ] closing { or } closing [
  MissingValue,     // "key": followed by no value
  Truncated,        // file ends inside an object or array
  RootNotObject,    // top level is not { ... }
  TrailingContent,  // a second top-level value
  NoItems,          // no "items" key in the top-level object
  ItemsNotArray,    // "items" is not [ ... ]
  TooLarge,         // more than MAX_CONFIG_BYTES
};

/**
 * Streams /tracking/config.json into a Config.
 *
 * Feed the file in any chunking, then call finish(). The first structural
 * error wins and keeps the 1-based line it was found on. Holds a
 * StreamingJsonParser (~2.1 KB token buffer), so device callers heap-allocate
 * it for the duration of one parse.
 */
class ConfigParser {
 public:
  explicit ConfigParser(Config& out);
  ConfigParser(const ConfigParser&) = delete;
  ConfigParser& operator=(const ConfigParser&) = delete;

  void feed(const char* data, size_t len);
  ParseError finish();
  uint32_t errorLine() const { return errLine; }
  /** True once any error is recorded; later feed() calls are no-ops. */
  bool failed() const { return error != ParseError::None; }

 private:
  enum class Key : uint8_t { Other, Items, Label, Type, Id, Options, Min, Max, Integer, TimestampOverride };
  enum class Context : uint8_t { Other, Root, ItemsArray, Item, Options };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  enum class ValueKind : uint8_t { String, Number, Bool, Null, Object, Array };
  // Every value event funnels through here; `text` is set for String/Number,
  // `flag` for Bool.
  void onValue(ValueKind kind, const char* text, size_t len, bool flag);
  void onClose(bool isArray);
  void fail(ParseError error);
  bool scanRawByte(char c);
  void push(bool isArray, Context context);
  bool topIsArray() const { return depth > 0 && (arrayBits >> (depth - 1) & 1u) != 0; }
  Context topContext() const;

  void beginItem();
  void endItem();
  void discardItemOptions();
  void addOption(const char* text, size_t len);

  Config& config;
  StreamingJsonParser parser;

  uint32_t line = 1;
  uint32_t errLine = 0;
  uint32_t bytesFed = 0;
  ParseError error = ParseError::None;

  // Container stack: one bit per level (1 = array), plus the context of the
  // first four levels, which are the only ones the schema gives meaning to.
  uint32_t arrayBits = 0;
  uint8_t depth = 0;
  static constexpr uint8_t TRACKED_LEVELS = 4;
  Context contexts[TRACKED_LEVELS] = {};

  bool sawContent = false;  // any non-whitespace byte
  bool rootSeen = false;
  bool itemsSeen = false;
  bool awaitingValue = false;  // inside an object, a key has no value yet

  // Raw-byte grammar for scanRawByte(), independent of the tokenizer's state
  // (the tokenizer silently drops bytes it cannot place).
  enum class Raw : uint8_t {
    Free,        // outside the root object: no checks
    Value,       // a value must start (after ':', '[' or an array's ',')
    Key,         // a key must start (after '{' or an object's ',')
    AfterKey,    // a key's string closed; ':' comes next
    AfterValue,  // a value ended; ',', '}' or ']' comes next
    Number,      // inside a run of [0-9.eE+-]
    Literal,     // inside true / false / null
    String,      // inside a string
  };
  Raw raw = Raw::Free;
  char rawLastStruct = 0;  // the ':', '[', '{' or ',' that set Value / Key
  bool rawStringIsKey = false;
  bool rawEscaped = false;
  uint8_t rawLiteralLeft = 0;  // bytes of true / false / null still to come
  uint8_t rawDepth = 0;
  uint32_t rawArrayBits = 0;  // one bit per level, 1 = array
  Key pendingKey = Key::Other;

  // The item being built; committed or rolled back at its closing brace.
  Item current{};
  bool itemBroken = false;
  bool typeSeen = false;
  bool optionsBroken = false;  // non-array options, a non-string or over-cap entry
  bool numberRuleBroken = false;
  uint16_t itemPoolMark = 0;
  uint16_t itemOptionMark = 0;
};

}  // namespace tracking
