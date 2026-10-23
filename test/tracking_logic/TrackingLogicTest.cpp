#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "TrackingConfig.h"
#include "TrackingFormat.h"

using namespace tracking;

namespace {

struct Parsed {
  std::unique_ptr<Config> config = std::make_unique<Config>();
  ParseError error = ParseError::None;
  uint32_t line = 0;
};

// Feeds in small, uneven chunks so chunk boundaries land inside tokens.
Parsed parse(const std::string& json, const size_t chunk = 7) {
  Parsed out;
  auto parser = std::make_unique<ConfigParser>(*out.config);
  for (size_t i = 0; i < json.size(); i += chunk) {
    parser->feed(json.data() + i, std::min(chunk, json.size() - i));
  }
  out.error = parser->finish();
  out.line = parser->errorLine();
  return out;
}

std::string itemsJson(const std::string& items) { return "{\"items\": [" + items + "]}"; }

Item numberItem(const bool integerOnly = false) {
  Item item{};
  item.type = ItemType::Number;
  item.integerOnly = integerOnly;
  return item;
}

std::string canon(const char* input, const Item& item, NumberCheck* result = nullptr) {
  char out[NUMBER_CANONICAL_BYTES];
  const NumberCheck check = checkNumber(input, item, out, sizeof(out));
  if (result) *result = check;
  return out;
}

std::string row(const char* ts, const char* key, const char* type, const char* value) {
  char out[ROW_BYTES];
  EXPECT_TRUE(formatCsvRow(out, sizeof(out), ts, key, type, value));
  return out;
}

}  // namespace

// --- Config: valid ---

TEST(TrackingConfig, ParsesAllEightTypesInOrder) {
  const auto p = parse(R"({ "items": [
    { "label": "Mood",      "type": "stars" },
    { "label": "Weight",    "type": "number", "min": 0, "max": 500, "integer": false },
    { "label": "Journal",   "type": "text" },
    { "label": "Woke up",   "type": "datetime" },
    { "label": "Started",   "type": "date" },
    { "label": "Workout",   "type": "list", "options": ["Run", "Lift", "Rest"], "id": "workout" },
    { "label": "Slept well","type": "boolean" },
    { "label": "Took meds", "type": "none" }
  ] })");
  ASSERT_EQ(p.error, ParseError::None);
  const Config& c = *p.config;
  ASSERT_EQ(c.itemCount, 8);
  EXPECT_EQ(c.skippedCount, 0);
  const ItemType expected[] = {ItemType::Stars, ItemType::Number, ItemType::Text,    ItemType::DateTime,
                               ItemType::Date,  ItemType::List,   ItemType::Boolean, ItemType::None};
  const char* labels[] = {"Mood", "Weight", "Journal", "Woke up", "Started", "Workout", "Slept well", "Took meds"};
  for (int i = 0; i < 8; i++) {
    EXPECT_EQ(c.items[i].type, expected[i]) << i;
    EXPECT_STREQ(c.items[i].label, labels[i]) << i;
  }
  const Item& weight = c.items[1];
  EXPECT_TRUE(weight.hasMin);
  EXPECT_TRUE(weight.hasMax);
  EXPECT_DOUBLE_EQ(weight.minValue, 0);
  EXPECT_DOUBLE_EQ(weight.maxValue, 500);
  EXPECT_FALSE(weight.integerOnly);
  EXPECT_STREQ(weight.key(), "Weight");

  const Item& workout = c.items[5];
  EXPECT_STREQ(workout.key(), "workout");
  ASSERT_EQ(workout.optionCount, 3);
  EXPECT_STREQ(c.option(workout, 0), "Run");
  EXPECT_STREQ(c.option(workout, 1), "Lift");
  EXPECT_STREQ(c.option(workout, 2), "Rest");
  EXPECT_STREQ(c.option(workout, 3), "");
}

TEST(TrackingConfig, NumberRulesAndUnknownKeys) {
  const auto p = parse(itemsJson(
      R"({"label":"Steps","type":"number","integer":true,"min":-1.5e1,"note":{"x":[1,2]},"max":null},
         {"type":"text","label":"Note","min":5,"integer":"yes","options":7})"));
  ASSERT_EQ(p.error, ParseError::None);
  ASSERT_EQ(p.config->itemCount, 2);
  const Item& steps = p.config->items[0];
  EXPECT_TRUE(steps.integerOnly);
  EXPECT_TRUE(steps.hasMin);
  EXPECT_DOUBLE_EQ(steps.minValue, -15.0);
  EXPECT_FALSE(steps.hasMax);
  // number-only keys are ignored on other types, whatever their shape
  const Item& note = p.config->items[1];
  EXPECT_FALSE(note.hasMin);
  EXPECT_FALSE(note.integerOnly);
  EXPECT_EQ(note.optionCount, 0);
}

TEST(TrackingConfig, TimestampOverrideOnEveryType) {
  const auto p = parse(itemsJson(R"(
    {"label":"a","type":"number","timestamp_override":true},
    {"label":"b","type":"text","timestamp_override":true},
    {"label":"c","type":"date","timestamp_override":true},
    {"label":"d","type":"datetime","timestamp_override":true},
    {"label":"e","type":"boolean","timestamp_override":true},
    {"label":"f","type":"stars","timestamp_override":true},
    {"label":"g","type":"list","options":["x"],"timestamp_override":true},
    {"label":"h","type":"none","timestamp_override":true},
    {"label":"off","type":"number","timestamp_override":false},
    {"label":"absent","type":"number","timestamp":true,"override":true},
    {"label":"twice","type":"text","timestamp_override":true,"timestamp_override":false}
  )"));
  ASSERT_EQ(p.error, ParseError::None);
  ASSERT_EQ(p.config->itemCount, 11);
  EXPECT_EQ(p.config->skippedCount, 0);
  for (int i = 0; i < 8; i++) EXPECT_TRUE(p.config->items[i].timestampOverride) << i;
  EXPECT_FALSE(p.config->items[8].timestampOverride);
  // Look-alike keys are unknown keys: ignored, and the default stays false.
  EXPECT_FALSE(p.config->items[9].timestampOverride);
  EXPECT_FALSE(p.config->items[10].timestampOverride);
  EXPECT_STREQ(p.config->option(p.config->items[6], 0), "x");
}

TEST(TrackingConfig, TimestampOverrideNonBooleanSkipsItem) {
  const auto p = parse(itemsJson(R"(
    {"label":"ok","type":"text","timestamp_override":true},
    {"label":"string","type":"text","timestamp_override":"true"},
    {"label":"number","type":"number","timestamp_override":1},
    {"label":"null","type":"none","timestamp_override":null},
    {"label":"object","type":"date","timestamp_override":{"on":true}},
    {"label":"array","type":"stars","timestamp_override":[true]},
    {"label":"list","type":"list","options":["a","b"],"timestamp_override":0},
    {"label":"also ok","type":"none"}
  )"));
  ASSERT_EQ(p.error, ParseError::None);
  ASSERT_EQ(p.config->itemCount, 2);
  EXPECT_STREQ(p.config->items[0].label, "ok");
  EXPECT_TRUE(p.config->items[0].timestampOverride);
  EXPECT_STREQ(p.config->items[1].label, "also ok");
  EXPECT_FALSE(p.config->items[1].timestampOverride);
  EXPECT_EQ(p.config->skippedCount, 6);
  // The broken list's options are rolled back with it.
  EXPECT_EQ(p.config->optionTotal, 0);
  EXPECT_EQ(p.config->optionPoolUsed, 0);
}

TEST(TrackingConfig, TimestampOverrideScopedToItsItem) {
  const auto p = parse(R"({"timestamp_override":1,"items":[
    {"label":"on","type":"number","timestamp_override":true},
    {"label":"next","type":"number"},
    {"label":"nested","type":"text","meta":{"timestamp_override":"x"}}
  ]})");
  ASSERT_EQ(p.error, ParseError::None);
  ASSERT_EQ(p.config->itemCount, 3);
  EXPECT_EQ(p.config->skippedCount, 0);
  EXPECT_TRUE(p.config->items[0].timestampOverride);
  // No carry-over from the previous item.
  EXPECT_STREQ(p.config->items[1].label, "next");
  EXPECT_FALSE(p.config->items[1].timestampOverride);
  // A same-named key in a nested object is not the item's key.
  EXPECT_STREQ(p.config->items[2].label, "nested");
  EXPECT_FALSE(p.config->items[2].timestampOverride);
}

TEST(TrackingConfig, TimestampOverrideAloneOrRepairedStillSkips) {
  const auto p = parse(itemsJson(R"(
    {"timestamp_override":true},
    {"label":"sticky","type":"text","timestamp_override":null,"timestamp_override":true},
    {"label":"ok","type":"none"}
  )"));
  ASSERT_EQ(p.error, ParseError::None);
  ASSERT_EQ(p.config->itemCount, 1);
  EXPECT_STREQ(p.config->items[0].label, "ok");
  EXPECT_FALSE(p.config->items[0].timestampOverride);
  // Missing label/type, and a later valid value does not unbreak an item.
  EXPECT_EQ(p.config->skippedCount, 2);
}

TEST(TrackingConfig, EscapesAndUnicodePassThrough) {
  const auto p = parse(itemsJson(R"({"label":"Café \"x\"","type":"list","options":["日本","a,b"]})"));
  ASSERT_EQ(p.error, ParseError::None);
  ASSERT_EQ(p.config->itemCount, 1);
  EXPECT_STREQ(p.config->items[0].label, "Café \"x\"");
  EXPECT_STREQ(p.config->option(p.config->items[0], 0), "日本");
  EXPECT_STREQ(p.config->option(p.config->items[0], 1), "a,b");
}

TEST(TrackingConfig, EmptyItemsArrayIsValid) {
  const auto p = parse(R"({"items": []})");
  EXPECT_EQ(p.error, ParseError::None);
  EXPECT_EQ(p.config->itemCount, 0);
}

TEST(TrackingConfig, ChunkingDoesNotMatter) {
  const std::string json = itemsJson(R"({"label":"Weight","type":"number","min":12.25,"max":99})");
  for (size_t chunk : {1u, 2u, 3u, 64u, 4096u}) {
    const auto p = parse(json, chunk);
    ASSERT_EQ(p.error, ParseError::None) << chunk;
    ASSERT_EQ(p.config->itemCount, 1) << chunk;
    EXPECT_DOUBLE_EQ(p.config->items[0].minValue, 12.25) << chunk;
  }
}

// --- Config: invalid items are skipped and counted ---

TEST(TrackingConfig, InvalidItemsAreSkippedAndCounted) {
  const auto p = parse(itemsJson(R"(
    {"label":"ok","type":"text"},
    {"label":"bad type","type":"slider"},
    {"type":"text"},
    {"label":"","type":"text"},
    {"label":"no type"},
    {"label":"list no options","type":"list"},
    {"label":"list empty","type":"list","options":[]},
    {"label":"list non-string","type":"list","options":["a",3]},
    {"label":"list not array","type":"list","options":"a"},
    {"label":"list empty option","type":"list","options":[""]},
    {"label":"min>max","type":"number","min":5,"max":1},
    {"label":"min string","type":"number","min":"5"},
    {"label":"integer string","type":"number","integer":"true"},
    {"label":7,"type":"text"},
    {"label":"type number","type":3},
    {"label":"id number","type":"text","id":5},
    "not an object",
    42,
    ["nested"],
    {"label":"also ok","type":"none"}
  )"));
  ASSERT_EQ(p.error, ParseError::None);
  ASSERT_EQ(p.config->itemCount, 2);
  EXPECT_STREQ(p.config->items[0].label, "ok");
  EXPECT_STREQ(p.config->items[1].label, "also ok");
  EXPECT_EQ(p.config->skippedCount, 18);
  // A skipped list leaves nothing behind in the option pool.
  EXPECT_EQ(p.config->optionTotal, 0);
  EXPECT_EQ(p.config->optionPoolUsed, 0);
}

TEST(TrackingConfig, RepeatedOptionsKeyReplaces) {
  const auto p = parse(itemsJson(R"({"label":"L","type":"list","options":["a","b"],"options":["c"]})"));
  ASSERT_EQ(p.config->itemCount, 1);
  ASSERT_EQ(p.config->items[0].optionCount, 1);
  EXPECT_STREQ(p.config->option(p.config->items[0], 0), "c");
  EXPECT_EQ(p.config->optionTotal, 1);
}

TEST(TrackingConfig, NonListOptionsDoNotConsumePool) {
  const auto p = parse(itemsJson(R"({"label":"T","type":"text","options":["a","b"]},
                                    {"label":"L","type":"list","options":["x"]})"));
  ASSERT_EQ(p.config->itemCount, 2);
  EXPECT_EQ(p.config->optionTotal, 1);
  EXPECT_STREQ(p.config->option(p.config->items[1], 0), "x");
}

// --- Config: caps ---

TEST(TrackingConfig, LabelIdAndOptionLengthCaps) {
  const std::string label63(MAX_LABEL_BYTES, 'a');
  const std::string label64(MAX_LABEL_BYTES + 1, 'a');
  const std::string id31(MAX_ID_BYTES, 'i');
  const std::string id32(MAX_ID_BYTES + 1, 'i');
  const std::string opt47(MAX_OPTION_BYTES, 'o');
  const std::string opt48(MAX_OPTION_BYTES + 1, 'o');
  const auto p =
      parse(itemsJson("{\"label\":\"" + label63 + "\",\"type\":\"text\",\"id\":\"" + id31 + "\"}," + "{\"label\":\"" +
                      label64 + "\",\"type\":\"text\"}," + "{\"label\":\"x\",\"type\":\"text\",\"id\":\"" + id32 +
                      "\"}," + "{\"label\":\"y\",\"type\":\"list\",\"options\":[\"" + opt47 + "\"]}," +
                      "{\"label\":\"z\",\"type\":\"list\",\"options\":[\"" + opt48 + "\"]}"));
  ASSERT_EQ(p.error, ParseError::None);
  ASSERT_EQ(p.config->itemCount, 2);
  EXPECT_EQ(std::string(p.config->items[0].label), label63);
  EXPECT_EQ(std::string(p.config->items[0].id), id31);
  EXPECT_EQ(std::string(p.config->option(p.config->items[1], 0)), opt47);
  EXPECT_EQ(p.config->skippedCount, 3);
}

TEST(TrackingConfig, OptionCountCap) {
  std::string ok;
  std::string over;
  for (size_t i = 0; i < MAX_OPTIONS; i++) ok += std::string(i ? "," : "") + "\"o" + std::to_string(i) + "\"";
  over = ok + ",\"extra\"";
  const auto p = parse(itemsJson("{\"label\":\"a\",\"type\":\"list\",\"options\":[" + ok + "]}," +
                                 "{\"label\":\"b\",\"type\":\"list\",\"options\":[" + over + "]}"));
  ASSERT_EQ(p.config->itemCount, 1);
  EXPECT_EQ(p.config->items[0].optionCount, MAX_OPTIONS);
  EXPECT_EQ(p.config->skippedCount, 1);
  EXPECT_EQ(p.config->optionTotal, MAX_OPTIONS);
}

TEST(TrackingConfig, ItemCountCap) {
  std::string items;
  for (size_t i = 0; i < MAX_ITEMS + 3; i++) {
    items += std::string(i ? "," : "") + "{\"label\":\"L" + std::to_string(i) + "\",\"type\":\"none\"}";
  }
  const auto p = parse(itemsJson(items));
  ASSERT_EQ(p.error, ParseError::None);
  EXPECT_EQ(p.config->itemCount, MAX_ITEMS);
  EXPECT_EQ(p.config->skippedCount, 3);
  EXPECT_STREQ(p.config->items[MAX_ITEMS - 1].label, ("L" + std::to_string(MAX_ITEMS - 1)).c_str());
}

TEST(TrackingConfig, OptionPoolAndTotalCaps) {
  // 8 lists x 20 options = 160 options > MAX_TOTAL_OPTIONS (128): the lists
  // that no longer fit are skipped whole, the earlier ones stay intact.
  std::string items;
  for (int l = 0; l < 8; l++) {
    std::string opts;
    for (size_t i = 0; i < MAX_OPTIONS; i++) opts += std::string(i ? "," : "") + "\"o\"";
    items += std::string(l ? "," : "") + "{\"label\":\"L\",\"type\":\"list\",\"options\":[" + opts + "]}";
  }
  const auto p = parse(itemsJson(items));
  ASSERT_EQ(p.error, ParseError::None);
  EXPECT_EQ(p.config->itemCount, MAX_TOTAL_OPTIONS / MAX_OPTIONS);
  EXPECT_EQ(p.config->skippedCount, 8 - MAX_TOTAL_OPTIONS / MAX_OPTIONS);
  EXPECT_LE(p.config->optionTotal, MAX_TOTAL_OPTIONS);

  // Pool bytes: long options exhaust OPTION_POOL_BYTES before the count cap.
  const std::string opt(MAX_OPTION_BYTES, 'x');
  items.clear();
  for (int l = 0; l < 4; l++) {
    std::string opts;
    for (size_t i = 0; i < MAX_OPTIONS; i++) opts += std::string(i ? "," : "") + "\"" + opt + "\"";
    items += std::string(l ? "," : "") + "{\"label\":\"L\",\"type\":\"list\",\"options\":[" + opts + "]}";
  }
  const auto q = parse(itemsJson(items));
  ASSERT_EQ(q.error, ParseError::None);
  EXPECT_EQ(q.config->itemCount, OPTION_POOL_BYTES / (MAX_OPTIONS * (MAX_OPTION_BYTES + 1)));
  EXPECT_LE(q.config->optionPoolUsed, OPTION_POOL_BYTES);
}

TEST(TrackingConfig, FileSizeCap) {
  std::string json = "{\"items\": [], \"pad\": \"" + std::string(MAX_CONFIG_BYTES, ' ') + "\"}";
  const auto p = parse(json, 1024);
  EXPECT_EQ(p.error, ParseError::TooLarge);
}

// --- Config: whole-file errors ---

TEST(TrackingConfig, WholeFileErrors) {
  EXPECT_EQ(parse("").error, ParseError::Empty);
  EXPECT_EQ(parse("  \n\t ").error, ParseError::Empty);
  EXPECT_EQ(parse("[1,2]").error, ParseError::RootNotObject);
  EXPECT_EQ(parse("hello").error, ParseError::RootNotObject);
  EXPECT_EQ(parse("{}").error, ParseError::NoItems);
  EXPECT_EQ(parse(R"({"item": []})").error, ParseError::NoItems);
  EXPECT_EQ(parse(R"({"items": {}})").error, ParseError::ItemsNotArray);
  EXPECT_EQ(parse(R"({"items": "x"})").error, ParseError::ItemsNotArray);
  EXPECT_EQ(parse(R"({"items": [})").error, ParseError::Mismatch);
  EXPECT_EQ(parse(R"({"items": []]})").error, ParseError::Mismatch);
  EXPECT_EQ(parse(R"({"items": [] }})").error, ParseError::Mismatch);
  EXPECT_EQ(parse(R"({"items": [{"label": }]})").error, ParseError::MissingValue);
  EXPECT_EQ(parse(R"({"items": [{"label" "type": "x"}]})").error, ParseError::MissingValue);
  EXPECT_EQ(parse(R"({"items": [)").error, ParseError::Truncated);
  EXPECT_EQ(parse(R"({"items": [{"label": "abc)").error, ParseError::Truncated);
  EXPECT_EQ(parse(R"({"items": []} {})").error, ParseError::TrailingContent);
  EXPECT_EQ(parse(R"({"items": [{"label": tru}]})").error, ParseError::Syntax);
  EXPECT_EQ(parse(R"({"items": [{"min": 1-2}]})").error, ParseError::Syntax);
  EXPECT_EQ(parse(R"({"items": [{"min": 01}]})").error, ParseError::Syntax);
  EXPECT_EQ(parse(R"({"items": [{"label": "\uZZZZ"}]})").error, ParseError::Syntax);
}

TEST(TrackingConfig, StrayByteWhereValueStartsIsSyntax) {
  // The tokenizer would skip the stray byte and read 5 or 10.
  for (const char* bad : {".5", "+5", "x10"}) {
    const auto p =
        parse(std::string("{\"items\": [\n{\"label\": \"W\", \"type\": \"number\",\n \"min\": ") + bad + "}]}");
    EXPECT_EQ(p.error, ParseError::Syntax) << bad;
    EXPECT_EQ(p.line, 3u) << bad;
  }
  EXPECT_EQ(parse(R"({"items": [{"label": "W", "type": "number", "min": [.5]}]})").error, ParseError::Syntax);
  EXPECT_EQ(parse(R"({"items": [1, .5]})").error, ParseError::Syntax);
  EXPECT_EQ(parse(R"({"items": x[]})").error, ParseError::Syntax);
  // Stray bytes inside strings, and every legal value start, still pass.
  const auto ok = parse(itemsJson(
      R"({"label":"a: .5, +5 [x]","type":"number","min":-0.5,"max":10,"integer":false,"id":null,"x":[true,{"y":[]}]})"));
  ASSERT_EQ(ok.error, ParseError::None);
  ASSERT_EQ(ok.config->itemCount, 1);
  EXPECT_STREQ(ok.config->items[0].label, "a: .5, +5 [x]");
  EXPECT_DOUBLE_EQ(ok.config->items[0].minValue, -0.5);
  // An escaped quote does not end the string early.
  EXPECT_EQ(parse(itemsJson(R"({"label":"q\":.5","type":"text"})")).error, ParseError::None);
}

TEST(TrackingConfig, StrayByteAfterValueOrAtKeyIsSyntax) {
  // The tokenizer would drop the stray bytes and read 2, 1, 5, 0 or "W".
  for (const char* bad : {R"("min": 2,5)", R"("max": 1 000)", R"("min": 5x)", R"("min": 0x10)", R"(label: "W")",
                          R"("label": "W" x)", R"("integer": truex)", R"("min" x: 1)"}) {
    const auto p = parse(std::string("{\"items\": [\n{\"label\": \"W\", \"type\": \"number\",\n ") + bad + "}]}");
    EXPECT_EQ(p.error, ParseError::Syntax) << bad;
    EXPECT_EQ(p.line, 3u) << bad;
  }
  // Malformed commas.
  EXPECT_EQ(parse(R"({"items": [,1]})").error, ParseError::Syntax);
  EXPECT_EQ(parse(R"({"items": [1,]})").error, ParseError::Syntax);
  EXPECT_EQ(parse(R"({"items": [], })").error, ParseError::Syntax);
  EXPECT_EQ(parse(R"({, "items": []})").error, ParseError::Syntax);
  EXPECT_EQ(parse(R"({"items": [], "a":, "b": 1})").error, ParseError::Syntax);
  // Every legal separator, with and without whitespace, still parses.
  const auto ok = parse(
      "{ \"items\" :\r\n[ {\"label\":\"a\",\"type\":\"number\",\"min\":-1.5e0 ,\"max\":2E+1,"
      "\"integer\":true}\t,{ \"label\" : \"b\\\\\" , \"type\" : \"list\" , \"options\" : "
      "[ \"x\" , \"\\u00e9\" ] } ] , \"z\" : [ null , false , [ ] , { } ] }");
  ASSERT_EQ(ok.error, ParseError::None);
  ASSERT_EQ(ok.config->itemCount, 2);
  EXPECT_DOUBLE_EQ(ok.config->items[0].minValue, -1.5);
  EXPECT_DOUBLE_EQ(ok.config->items[0].maxValue, 20.0);
  EXPECT_STREQ(ok.config->items[1].label, "b\\");
  EXPECT_STREQ(ok.config->option(ok.config->items[1], 1), "\xC3\xA9");
}

TEST(TrackingConfig, FailedReportsFirstError) {
  Config config;
  ConfigParser parser(config);
  EXPECT_FALSE(parser.failed());
  parser.feed("{\"items\": [}", 12);
  EXPECT_TRUE(parser.failed());
  EXPECT_EQ(parser.finish(), ParseError::Mismatch);
}

TEST(TrackingConfig, ErrorLineIsReported) {
  const auto p = parse("{\n  \"items\": [\n    {\"label\": \"a\", \"type\": \"text\"},\n    {\"label\": }\n  ]\n}\n");
  EXPECT_EQ(p.error, ParseError::MissingValue);
  EXPECT_EQ(p.line, 4u);
  const auto t = parse("{\n\"items\": [\n");
  EXPECT_EQ(t.error, ParseError::Truncated);
  EXPECT_EQ(t.line, 3u);
}

TEST(TrackingConfig, TypeNamesRoundTrip) {
  for (const char* name : {"number", "text", "date", "datetime", "boolean", "stars", "list", "none"}) {
    ItemType type;
    ASSERT_TRUE(parseTypeName(name, type)) << name;
    EXPECT_STREQ(typeName(type), name);
  }
  ItemType type;
  EXPECT_FALSE(parseTypeName("Number", type));
  EXPECT_FALSE(parseTypeName("", type));
}

// --- CSV ---

TEST(TrackingCsv, QuotingEdgeCases) {
  EXPECT_FALSE(csvNeedsQuoting("plain"));
  EXPECT_FALSE(csvNeedsQuoting(""));
  EXPECT_TRUE(csvNeedsQuoting("a,b"));
  EXPECT_TRUE(csvNeedsQuoting("say \"hi\""));
  EXPECT_TRUE(csvNeedsQuoting("line\nbreak"));
  EXPECT_TRUE(csvNeedsQuoting("cr\rhere"));

  char out[64];
  EXPECT_EQ(appendCsvField(out, sizeof(out), 0, "a,b"), 5u);
  EXPECT_STREQ(out, "\"a,b\"");
  EXPECT_EQ(appendCsvField(out, sizeof(out), 0, "say \"hi\""), 12u);
  EXPECT_STREQ(out, "\"say \"\"hi\"\"\"");
  EXPECT_EQ(appendCsvField(out, sizeof(out), 0, "\""), 4u);
  EXPECT_STREQ(out, "\"\"\"\"");
  EXPECT_EQ(appendCsvField(out, sizeof(out), 0, "two\nlines"), 11u);
  EXPECT_STREQ(out, "\"two\nlines\"");
  EXPECT_EQ(appendCsvField(out, sizeof(out), 0, "Café ☕ 日本"), strlen("Café ☕ 日本"));
  EXPECT_STREQ(out, "Café ☕ 日本");
}

TEST(TrackingCsv, FieldThatDoesNotFitLeavesBufferIntact) {
  char out[8];
  strcpy(out, "ab");
  EXPECT_EQ(appendCsvField(out, sizeof(out), 2, "\"\"\""), 0u);
  EXPECT_STREQ(out, "ab");
  EXPECT_EQ(appendCsvField(out, sizeof(out), 2, "cdefg"), 7u);
  EXPECT_STREQ(out, "abcdefg");
}

TEST(TrackingCsv, RowsForEveryType) {
  const char* ts = "2026-10-22T07:05:09";
  EXPECT_EQ(row(ts, "Weight", "number", "72.5"), "2026-10-22T07:05:09,Weight,number,72.5\n");
  EXPECT_EQ(row(ts, "Journal", "text", "Good day, mostly \"fine\""),
            "2026-10-22T07:05:09,Journal,text,\"Good day, mostly \"\"fine\"\"\"\n");
  EXPECT_EQ(row(ts, "Started", "date", "2024-02-29"), "2026-10-22T07:05:09,Started,date,2024-02-29\n");
  EXPECT_EQ(row(ts, "Woke up", "datetime", "2026-10-22T06:30"),
            "2026-10-22T07:05:09,Woke up,datetime,2026-10-22T06:30\n");
  EXPECT_EQ(row(ts, "Slept well", "boolean", booleanValue(true)), "2026-10-22T07:05:09,Slept well,boolean,true\n");
  EXPECT_EQ(row(ts, "Slept well", "boolean", booleanValue(false)), "2026-10-22T07:05:09,Slept well,boolean,false\n");
  EXPECT_EQ(row(ts, "Mood", "stars", "0"), "2026-10-22T07:05:09,Mood,stars,0\n");
  EXPECT_EQ(row(ts, "Mood", "stars", "5"), "2026-10-22T07:05:09,Mood,stars,5\n");
  EXPECT_EQ(row(ts, "workout", "list", "Run, easy"), "2026-10-22T07:05:09,workout,list,\"Run, easy\"\n");
  EXPECT_EQ(row(ts, "Took meds", "none", ""), "2026-10-22T07:05:09,Took meds,none,\n");
  EXPECT_EQ(row(ts, "a,\"b\"", "text", "x\ny"), "2026-10-22T07:05:09,\"a,\"\"b\"\"\",text,\"x\ny\"\n");
}

TEST(TrackingCsv, WorstCaseRowFits) {
  const std::string label(MAX_LABEL_BYTES, '"');
  const std::string text(MAX_TEXT_BYTES, '"');
  char out[ROW_BYTES];
  ASSERT_TRUE(formatCsvRow(out, sizeof(out), "2026-10-22T07:05:09", label.c_str(), "datetime", text.c_str()));
  EXPECT_EQ(strlen(out), 19u + 1 + (2 * MAX_LABEL_BYTES + 2) + 1 + 8 + 1 + (2 * MAX_TEXT_BYTES + 2) + 1);
  EXPECT_LT(strlen(out) + 1, ROW_BYTES);
  char small[16];
  EXPECT_FALSE(formatCsvRow(small, sizeof(small), "2026-10-22T07:05:09", "k", "t", "v"));
}

TEST(TrackingCsv, HeaderDetection) {
  const std::string h = std::string(CSV_HEADER);
  EXPECT_TRUE(startsWithCsvHeader((h + "\n").data(), h.size() + 1));
  EXPECT_TRUE(startsWithCsvHeader((h + "\nrow\n").data(), h.size() + 5));
  EXPECT_TRUE(startsWithCsvHeader((h + "\r\n").data(), h.size() + 2));
  const std::string bom = "\xEF\xBB\xBF" + h + "\n";
  EXPECT_TRUE(startsWithCsvHeader(bom.data(), bom.size()));
  EXPECT_FALSE(startsWithCsvHeader(h.data(), h.size()));       // no line end, more may follow
  EXPECT_TRUE(startsWithCsvHeader(h.data(), h.size(), true));  // the whole file is the header
  EXPECT_TRUE(startsWithCsvHeader((h + "\r").data(), h.size() + 1, true));
  EXPECT_FALSE(startsWithCsvHeader((h + "\r").data(), h.size() + 1));
  EXPECT_FALSE(startsWithCsvHeader((h + "\rx").data(), h.size() + 2, true));
  EXPECT_TRUE(startsWithCsvHeader(("\xEF\xBB\xBF" + h).data(), h.size() + 3, true));
  EXPECT_FALSE(startsWithCsvHeader(h.data(), h.size() - 1, true));
  const std::string extra = h + ",note\n";
  EXPECT_FALSE(startsWithCsvHeader(extra.data(), extra.size()));
  const std::string old = "datetime,millis,event\n";
  EXPECT_FALSE(startsWithCsvHeader(old.data(), old.size()));
  EXPECT_FALSE(startsWithCsvHeader("", 0));
}

TEST(TrackingCsv, LogHeadClassification) {
  const std::string h = std::string(CSV_HEADER);
  constexpr size_t probe = 64;
  const std::string withRows = h + "\n2026-10-22T07:05:09,Mood,stars,3\n" + std::string(100, 'x');
  EXPECT_EQ(classifyLogHead(withRows.data(), probe, probe, withRows.size()), LogHead::Ours);
  // Header only, no trailing newline: the whole file was read, so it is ours.
  EXPECT_EQ(classifyLogHead(h.data(), static_cast<int>(h.size()), probe, h.size()), LogHead::Ours);
  const std::string foreign = "datetime,millis,event\n";
  EXPECT_EQ(classifyLogHead(foreign.data(), static_cast<int>(foreign.size()), probe, foreign.size()), LogHead::Foreign);
  const std::string prefix = "times";  // whole file, a header prefix only
  EXPECT_EQ(classifyLogHead(prefix.data(), 5, probe, 5), LogHead::Foreign);
  EXPECT_EQ(classifyLogHead(nullptr, 0, probe, 0), LogHead::Empty);
}

TEST(TrackingCsv, ReadFailureNeverRotates) {
  const std::string foreign = "datetime,millis,event\n" + std::string(200, 'x');
  // Failed, empty and short reads of a non-empty file are Unreadable.
  EXPECT_EQ(classifyLogHead(foreign.data(), -1, 64, foreign.size()), LogHead::Unreadable);
  EXPECT_EQ(classifyLogHead(foreign.data(), 0, 64, foreign.size()), LogHead::Unreadable);
  EXPECT_EQ(classifyLogHead(foreign.data(), 10, 64, foreign.size()), LogHead::Unreadable);
  EXPECT_EQ(classifyLogHead(foreign.data(), 0, 64, 5), LogHead::Unreadable);
}

namespace {
struct TakenPaths {
  std::vector<std::string> paths;
  static bool taken(const char* path, void* ctx) {
    const auto& v = static_cast<TakenPaths*>(ctx)->paths;
    return std::find(v.begin(), v.end(), path) != v.end();
  }
};
}  // namespace

TEST(TrackingCsv, RotationNeverReusesASlot) {
  char path[ROTATED_LOG_PATH_BYTES];
  ASSERT_TRUE(rotatedLogPath(1, path, sizeof(path)));
  EXPECT_STREQ(path, "/tracking/log.old.csv");
  ASSERT_TRUE(rotatedLogPath(2, path, sizeof(path)));
  EXPECT_STREQ(path, "/tracking/log.old-2.csv");
  ASSERT_TRUE(rotatedLogPath(MAX_ROTATED_LOGS, path, sizeof(path)));
  EXPECT_STREQ(path, "/tracking/log.old-99.csv");
  EXPECT_FALSE(rotatedLogPath(0, path, sizeof(path)));
  EXPECT_FALSE(rotatedLogPath(MAX_ROTATED_LOGS + 1, path, sizeof(path)));
  EXPECT_FALSE(rotatedLogPath(2, path, 10));

  TakenPaths disk;
  EXPECT_EQ(findFreeRotatedLog(TakenPaths::taken, &disk, path, sizeof(path)), 1u);
  EXPECT_STREQ(path, "/tracking/log.old.csv");
  // The second foreign-header event goes beside the first, not over it.
  disk.paths.push_back("/tracking/log.old.csv");
  EXPECT_EQ(findFreeRotatedLog(TakenPaths::taken, &disk, path, sizeof(path)), 2u);
  EXPECT_STREQ(path, "/tracking/log.old-2.csv");
  disk.paths.push_back("/tracking/log.old-2.csv");
  disk.paths.push_back("/tracking/log.old-4.csv");
  EXPECT_EQ(findFreeRotatedLog(TakenPaths::taken, &disk, path, sizeof(path)), 3u);
  // Every slot taken: refuse.
  for (unsigned i = 3; i <= MAX_ROTATED_LOGS; i++) {
    ASSERT_TRUE(rotatedLogPath(i, path, sizeof(path)));
    disk.paths.push_back(path);
  }
  EXPECT_EQ(findFreeRotatedLog(TakenPaths::taken, &disk, path, sizeof(path)), 0u);
  EXPECT_STREQ(path, "");
}

// --- Numbers ---

TEST(TrackingNumber, CanonicalForm) {
  const Item item = numberItem();
  EXPECT_EQ(canon("72.5", item), "72.5");
  EXPECT_EQ(canon("72.50", item), "72.5");
  EXPECT_EQ(canon("5.0", item), "5");
  EXPECT_EQ(canon("5.", item), "5");
  EXPECT_EQ(canon(".5", item), "0.5");
  EXPECT_EQ(canon("+007.50", item), "7.5");
  EXPECT_EQ(canon("-0", item), "0");
  EXPECT_EQ(canon("-0.000", item), "0");
  EXPECT_EQ(canon("-3.25", item), "-3.25");
  EXPECT_EQ(canon("  12  ", item), "12");
  EXPECT_EQ(canon("000", item), "0");
  EXPECT_EQ(canon("100", item), "100");
}

TEST(TrackingNumber, RejectsGarbage) {
  const Item item = numberItem();
  NumberCheck r;
  for (const char* bad : {"abc", "1e3", "1,5", "1.2.3", "--1", "+", "-", ".", "1 2", "0x10", "12a", "∞", "NaN"}) {
    canon(bad, item, &r);
    EXPECT_EQ(r, NumberCheck::NotANumber) << bad;
  }
  canon("", item, &r);
  EXPECT_EQ(r, NumberCheck::Empty);
  canon("   ", item, &r);
  EXPECT_EQ(r, NumberCheck::Empty);
  const std::string tooLong(MAX_NUMBER_INPUT_BYTES + 1, '1');
  canon(tooLong.c_str(), item, &r);
  EXPECT_EQ(r, NumberCheck::NotANumber);
  const std::string longest(MAX_NUMBER_INPUT_BYTES, '1');
  EXPECT_EQ(canon(longest.c_str(), item, &r), longest);
  EXPECT_EQ(r, NumberCheck::Ok);
  // ".5"-style growth at the length cap still fits the canonical buffer
  const std::string frac = "." + std::string(MAX_NUMBER_INPUT_BYTES - 1, '5');
  EXPECT_EQ(canon(frac.c_str(), item, &r), "0" + frac);
  EXPECT_EQ(r, NumberCheck::Ok);
}

TEST(TrackingNumber, IntegerOnly) {
  const Item item = numberItem(true);
  NumberCheck r;
  EXPECT_EQ(canon("42", item, &r), "42");
  EXPECT_EQ(r, NumberCheck::Ok);
  EXPECT_EQ(canon("42.000", item, &r), "42");
  EXPECT_EQ(r, NumberCheck::Ok);
  canon("42.5", item, &r);
  EXPECT_EQ(r, NumberCheck::NotInteger);
  canon("-0.1", item, &r);
  EXPECT_EQ(r, NumberCheck::NotInteger);
}

TEST(TrackingNumber, MinMaxInclusive) {
  Item item = numberItem();
  item.hasMin = true;
  item.minValue = 0;
  item.hasMax = true;
  item.maxValue = 0.1;
  NumberCheck r;
  canon("0", item, &r);
  EXPECT_EQ(r, NumberCheck::Ok);
  canon("0.1", item, &r);
  EXPECT_EQ(r, NumberCheck::Ok);
  canon("-0.0001", item, &r);
  EXPECT_EQ(r, NumberCheck::BelowMin);
  canon("0.10001", item, &r);
  EXPECT_EQ(r, NumberCheck::AboveMax);
  // Only one bound set
  Item minOnly = numberItem();
  minOnly.hasMin = true;
  minOnly.minValue = -10;
  canon("1e9", minOnly, &r);
  EXPECT_EQ(r, NumberCheck::NotANumber);
  canon("99999999", minOnly, &r);
  EXPECT_EQ(r, NumberCheck::Ok);
  canon("-10.5", minOnly, &r);
  EXPECT_EQ(r, NumberCheck::BelowMin);
  // integer check comes before the bound check
  Item both = numberItem(true);
  both.hasMax = true;
  both.maxValue = 1;
  canon("5.5", both, &r);
  EXPECT_EQ(r, NumberCheck::NotInteger);
}

// --- Dates ---

TEST(TrackingDate, LeapYearsAndMonthLengths) {
  EXPECT_TRUE(isLeapYear(2024));
  EXPECT_FALSE(isLeapYear(2026));
  EXPECT_FALSE(isLeapYear(2100));
  EXPECT_TRUE(isLeapYear(2000));
  EXPECT_EQ(daysInMonth(2024, 2), 29);
  EXPECT_EQ(daysInMonth(2026, 2), 28);
  EXPECT_EQ(daysInMonth(2026, 4), 30);
  EXPECT_EQ(daysInMonth(2026, 12), 31);
  EXPECT_EQ(daysInMonth(2026, 0), 0);
  EXPECT_EQ(daysInMonth(2026, 13), 0);
}

TEST(TrackingDate, ClampKeepsDayInsideItsMonth) {
  DateTime dt{2024, 2, 29, 0, 0, 0};
  clampDateTime(dt);
  EXPECT_EQ(dt.day, 29);  // Feb 29 in a leap year stays
  dt.year = 2025;         // year moved off the leap year
  clampDateTime(dt);
  EXPECT_EQ(dt.day, 28);
  dt = DateTime{2026, 1, 31, 0, 0, 0};
  dt.month = 4;
  clampDateTime(dt);
  EXPECT_EQ(dt.day, 30);
  dt = DateTime{1999, 0, 0, 25, 61, 99};
  clampDateTime(dt);
  EXPECT_EQ(dt.year, YEAR_MIN);
  EXPECT_EQ(dt.month, 1);
  EXPECT_EQ(dt.day, 1);
  EXPECT_EQ(dt.hour, 23);
  EXPECT_EQ(dt.minute, 59);
  EXPECT_EQ(dt.second, 59);
  dt = DateTime{2200, 12, 31, 0, 0, 0};
  clampDateTime(dt);
  EXPECT_EQ(dt.year, YEAR_MAX);
}

TEST(TrackingDate, Formatting) {
  const DateTime dt{2024, 2, 9, 7, 5, 3};
  char out[TIMESTAMP_BYTES];
  formatTimestamp(dt, out, sizeof(out));
  EXPECT_STREQ(out, "2024-02-09T07:05:03");
  formatDate(dt, out, sizeof(out));
  EXPECT_STREQ(out, "2024-02-09");
  char dtOut[VALUE_DATE_BYTES];
  formatDateTime(dt, dtOut, sizeof(dtOut));
  EXPECT_STREQ(dtOut, "2024-02-09T07:05");
}

// --- Log rows ---

namespace {

bool parseRow(const std::string& line, CsvRow& out) { return parseCsvRow(line.data(), line.size(), out); }

struct TailLog {
  const std::string& log;
  std::vector<std::pair<size_t, size_t>> reads;  // (pos, len) of each read, in order
  int failAtRead = -1;                           // this read (0-based) fails
};

bool readTail(void* ctx, const size_t pos, char* buf, const size_t len) {
  auto& t = *static_cast<TailLog*>(ctx);
  if (static_cast<int>(t.reads.size()) == t.failAtRead) return false;
  t.reads.emplace_back(pos, len);
  EXPECT_LE(pos + len, t.log.size());
  memcpy(buf, t.log.data() + pos, len);
  return true;
}

struct TailScan {
  bool ok;
  LastEntry entry;
  std::vector<std::pair<size_t, size_t>> reads;
};

// Runs scanLogTail() over `log` as the store runs it over the file.
TailScan scanTail(const std::string& log, const char* key, const size_t chunk = 8 * LOG_READ_ALIGN,
                  const size_t cap = SIZE_MAX, const int failAtRead = -1) {
  TailLog t{log, {}, failAtRead};
  auto scanner = std::make_unique<LastEntryScanner>(key);
  std::vector<char> buf(chunk);
  const bool ok = scanLogTail(log.size(), cap, readTail, &t, *scanner, buf.data(), chunk);
  return {ok, scanner->result(), t.reads};
}

LastEntry scanLog(const std::string& log, const char* key, const size_t chunk = 8 * LOG_READ_ALIGN) {
  const TailScan r = scanTail(log, key, chunk);
  EXPECT_TRUE(r.ok);
  return r.entry;
}

std::string withHeader(const std::string& rows) { return std::string(CSV_HEADER) + "\n" + rows; }

// Chunk sizes 1, 2, 3, 5, 7, 13, 64, 511, 512, 513, 700, 1024 must all agree
// with the device's 4096; returns the 4096-byte result.
LastEntry scanAllChunkings(const std::string& log, const char* key) {
  const LastEntry ref = scanLog(log, key);
  for (size_t chunk : {1u, 2u, 3u, 5u, 7u, 13u, 64u, 511u, 512u, 513u, 700u, 1024u}) {
    const LastEntry e = scanLog(log, key, chunk);
    EXPECT_EQ(e.found, ref.found) << "chunk " << chunk;
    EXPECT_STREQ(e.timestamp, ref.timestamp) << "chunk " << chunk;
    EXPECT_STREQ(e.value, ref.value) << "chunk " << chunk;
  }
  return ref;
}

// Header, then filler rows for another key up to at least `bytes`.
std::string fillerLog(const size_t bytes) {
  std::string log = withHeader("");
  while (log.size() < bytes) log += row("2026-10-01T08:00:00", "Filler", "number", "1");
  return log;
}

}  // namespace

TEST(TrackingRow, PlainAndQuotedFields) {
  CsvRow r;
  ASSERT_TRUE(parseRow("2026-10-22T07:05:09,Weight,number,72.5", r));
  EXPECT_STREQ(r.timestamp, "2026-10-22T07:05:09");
  EXPECT_STREQ(r.key, "Weight");
  EXPECT_STREQ(r.type, "number");
  EXPECT_STREQ(r.value, "72.5");

  ASSERT_TRUE(parseRow("2026-10-22T07:05:09,\"a,\"\"b\"\"\",text,\"Good day, mostly \"\"fine\"\"\"", r));
  EXPECT_STREQ(r.key, "a,\"b\"");
  EXPECT_STREQ(r.value, "Good day, mostly \"fine\"");

  ASSERT_TRUE(parseRow("\"2026-10-22T07:05:09\",\"k\",\"list\",\"\"", r));
  EXPECT_STREQ(r.timestamp, "2026-10-22T07:05:09");
  EXPECT_STREQ(r.key, "k");
  EXPECT_STREQ(r.value, "");

  // A none row: empty last field.
  ASSERT_TRUE(parseRow("2026-10-22T07:05:09,Took meds,none,", r));
  EXPECT_STREQ(r.value, "");
  ASSERT_TRUE(parseRow(",,,", r));
  EXPECT_STREQ(r.timestamp, "");
  EXPECT_STREQ(r.key, "");

  ASSERT_TRUE(parseRow("t,k,text,\"\"\"\"", r));
  EXPECT_STREQ(r.value, "\"");
  ASSERT_TRUE(parseRow("t,k,text,Café ☕ 日本", r));
  EXPECT_STREQ(r.value, "Café ☕ 日本");
}

TEST(TrackingRow, RoundTripsEveryFormattedRow) {
  const char* ts = "2026-10-22T07:05:09";
  const std::string label(MAX_LABEL_BYTES, '"');
  const std::string text(MAX_TEXT_BYTES, '"');
  const std::string commas(MAX_TEXT_BYTES, ',');
  const char* cases[][3] = {
      {"Weight", "number", "72.5"},
      {"Journal", "text", "a \"b\", c"},
      {"Woke up", "datetime", "2026-10-22T06:30"},
      {"Took meds", "none", ""},
      {label.c_str(), "datetime", text.c_str()},
      {"x", "text", commas.c_str()},
      {"workout", "list", "Run, easy"},
  };
  for (const auto& c : cases) {
    std::string line = row(ts, c[0], c[1], c[2]);
    ASSERT_EQ(line.back(), '\n');
    line.pop_back();
    CsvRow r;
    ASSERT_TRUE(parseRow(line, r)) << line;
    EXPECT_STREQ(r.timestamp, ts);
    EXPECT_STREQ(r.key, c[0]);
    EXPECT_STREQ(r.type, c[1]);
    EXPECT_STREQ(r.value, c[2]);
  }
}

TEST(TrackingRow, RejectsMalformed) {
  CsvRow r;
  EXPECT_FALSE(parseRow("", r));
  EXPECT_FALSE(parseRow("a,b,c", r));
  EXPECT_FALSE(parseRow("a,b,c,d,e", r));
  EXPECT_FALSE(parseRow("a,b,c,d,", r));
  EXPECT_FALSE(parseRow("t,k,text,\"unterminated", r));
  EXPECT_FALSE(parseRow("t,\"k,text,v", r));
  EXPECT_FALSE(parseRow("t,\"k\"x,text,v", r));  // text after the closing quote
  EXPECT_FALSE(parseRow("t,k\"x,text,v", r));    // quote inside an unquoted field
  EXPECT_FALSE(parseRow("t,k,text,v\"", r));
  EXPECT_FALSE(parseRow("t,k,text,\"v\" ", r));
}

TEST(TrackingRow, FieldCaps) {
  CsvRow r;
  const std::string ts19 = "2026-10-22T07:05:09";
  const std::string key63(MAX_LABEL_BYTES, 'k');
  const std::string value128(MAX_TEXT_BYTES, 'v');
  ASSERT_TRUE(parseRow(ts19 + "," + key63 + ",datetime," + value128, r));
  EXPECT_EQ(std::string(r.key), key63);
  EXPECT_EQ(std::string(r.value), value128);
  // Quoting does not count against the cap; only the unquoted text does.
  const std::string quoted128 = "\"" + std::string(2 * MAX_TEXT_BYTES, '"') + "\"";
  ASSERT_TRUE(parseRow(ts19 + ",k,text," + quoted128, r));
  EXPECT_EQ(std::string(r.value), std::string(MAX_TEXT_BYTES, '"'));

  EXPECT_FALSE(parseRow(ts19 + "0,k,text,v", r));
  EXPECT_FALSE(parseRow(ts19 + "," + key63 + "k,text,v", r));
  EXPECT_FALSE(parseRow(ts19 + ",k,datetimes,v", r));
  EXPECT_FALSE(parseRow(ts19 + ",k,text," + value128 + "v", r));
  EXPECT_FALSE(parseRow(ts19 + ",k,text,\"" + value128 + "v\"", r));
}

TEST(TrackingLastEntry, MatchInLastLine) {
  const std::string log = withHeader(
      "2026-10-20T08:00:00,Mood,stars,2\n"
      "2026-10-21T08:00:00,Weight,number,71\n"
      "2026-10-22T08:00:00,Mood,stars,4\n");
  const LastEntry e = scanAllChunkings(log, "Mood");
  ASSERT_TRUE(e.found);
  EXPECT_STREQ(e.timestamp, "2026-10-22T08:00:00");
  EXPECT_STREQ(e.value, "4");
}

TEST(TrackingLastEntry, NewestMatchWinsOverEarlierOnes) {
  const std::string log = withHeader(
      "2026-10-19T08:00:00,Weight,number,73\n"
      "2026-10-20T08:00:00,Weight,number,72.5\n"
      "2026-10-21T08:00:00,Mood,stars,2\n"
      "2026-10-22T08:00:00,Journal,text,\"Weight, 70\"\n");
  const LastEntry e = scanAllChunkings(log, "Weight");
  ASSERT_TRUE(e.found);
  EXPECT_STREQ(e.timestamp, "2026-10-20T08:00:00");
  EXPECT_STREQ(e.value, "72.5");
}

TEST(TrackingLastEntry, KeyMatchesOnlyTheKeyColumnExactly) {
  const std::string log = withHeader(
      "2026-10-20T08:00:00,Weigh,number,1\n"
      "2026-10-21T08:00:00,Weight2,number,2\n"
      "2026-10-22T08:00:00,Note,text,Weight\n"
      "2026-10-22T09:00:00,Weight,text,x\n"
      "2026-10-23T08:00:00, Weight,number,3\n");
  EXPECT_FALSE(scanAllChunkings(log, "Wei").found);
  EXPECT_FALSE(scanAllChunkings(log, "Weights").found);
  EXPECT_FALSE(scanAllChunkings(log, "text").found);
  EXPECT_FALSE(scanAllChunkings(log, "2026-10-22T08:00:00").found);
  const LastEntry e = scanAllChunkings(log, "Weight");
  ASSERT_TRUE(e.found);
  EXPECT_STREQ(e.timestamp, "2026-10-22T09:00:00");
  EXPECT_STREQ(e.value, "x");
  const LastEntry quoted = scanAllChunkings(withHeader("2026-10-22T08:00:00,\"a,\"\"b\"\"\",text,v\n"), "a,\"b\"");
  ASSERT_TRUE(quoted.found);
  EXPECT_STREQ(quoted.value, "v");
}

TEST(TrackingLastEntry, EmptyHeaderOnlyAndNoMatch) {
  EXPECT_FALSE(scanAllChunkings("", "Mood").found);
  EXPECT_FALSE(scanAllChunkings(std::string(CSV_HEADER), "Mood").found);
  EXPECT_FALSE(scanAllChunkings(withHeader(""), "Mood").found);
  EXPECT_FALSE(scanAllChunkings(withHeader("2026-10-22T08:00:00,Weight,number,1\n"), "Mood").found);
  // The header is never taken for a row, even for an item keyed "label".
  EXPECT_FALSE(scanAllChunkings(withHeader(""), "label").found);
  EXPECT_FALSE(scanAllChunkings("\xEF\xBB\xBF" + withHeader(""), "label").found);
  EXPECT_FALSE(scanAllChunkings(std::string(CSV_HEADER) + "\r\n", "label").found);
  // A blank file is just blank lines.
  EXPECT_FALSE(scanAllChunkings("\n\n\r\n", "Mood").found);
}

TEST(TrackingLastEntry, NoneRowHasEmptyValue) {
  const LastEntry e = scanAllChunkings(
      withHeader("2026-10-22T08:00:00,Took meds,none,\n2026-10-22T09:00:00,Mood,stars,1\n"), "Took meds");
  ASSERT_TRUE(e.found);
  EXPECT_STREQ(e.timestamp, "2026-10-22T08:00:00");
  EXPECT_STREQ(e.value, "");
}

TEST(TrackingLastEntry, LineEndings) {
  // CRLF throughout, as a spreadsheet re-save leaves it.
  const std::string crlf =
      std::string(CSV_HEADER) + "\r\n2026-10-21T08:00:00,Mood,stars,2\r\n2026-10-22T08:00:00,Mood,stars,3\r\n";
  LastEntry e = scanAllChunkings(crlf, "Mood");
  ASSERT_TRUE(e.found);
  EXPECT_STREQ(e.value, "3");
  // A hand-edited last line without a line end.
  e = scanAllChunkings(withHeader("2026-10-21T08:00:00,Mood,stars,2\n2026-10-22T08:00:00,Mood,stars,5"), "Mood");
  ASSERT_TRUE(e.found);
  EXPECT_STREQ(e.value, "5");
  e = scanAllChunkings(withHeader("2026-10-21T08:00:00,Mood,stars,2\r\n2026-10-22T08:00:00,Mood,stars,5\r"), "Mood");
  ASSERT_TRUE(e.found);
  EXPECT_STREQ(e.value, "5");
  // Blank lines between and after rows.
  e = scanAllChunkings(withHeader("\n2026-10-21T08:00:00,Mood,stars,1\n\n\r\n\n"), "Mood");
  ASSERT_TRUE(e.found);
  EXPECT_STREQ(e.value, "1");
  // A first line that is a row (no header) still counts.
  e = scanAllChunkings("2026-10-21T08:00:00,Mood,stars,1", "Mood");
  ASSERT_TRUE(e.found);
  EXPECT_STREQ(e.value, "1");
  // Only a CR right before the LF is dropped; one mid-line is data.
  e = scanAllChunkings(withHeader("2026-10-21T08:00:00,Mood,stars,1\r2\r\n"), "Mood");
  ASSERT_TRUE(e.found);
  EXPECT_STREQ(e.value, "1\r2");
}

TEST(TrackingLastEntry, OverlongLineSkippedOthersStillFound) {
  const std::string tooLong = "2026-10-23T08:00:00,Mood,text," + std::string(ROW_BYTES, 'x');
  const std::string tooLongMatching = "2026-10-23T08:00:00,Mood,stars,9" + std::string(ROW_BYTES, ' ');
  const std::string log = withHeader("2026-10-20T08:00:00,Mood,stars,1\n" + tooLong + "\n" + tooLongMatching +
                                     "\n2026-10-24T08:00:00,Other,stars,2\n");
  LastEntry e = scanAllChunkings(log, "Mood");
  ASSERT_TRUE(e.found);
  EXPECT_STREQ(e.timestamp, "2026-10-20T08:00:00");
  EXPECT_STREQ(e.value, "1");
  // Overlong as the newline-less last line, and as the first line.
  e = scanAllChunkings(withHeader("2026-10-20T08:00:00,Mood,stars,1\n" + tooLong), "Mood");
  ASSERT_TRUE(e.found);
  EXPECT_STREQ(e.value, "1");
  EXPECT_FALSE(scanAllChunkings(tooLong + "\n2026-10-24T08:00:00,Other,stars,2\n", "Mood").found);
}

TEST(TrackingLastEntry, LongestRealRowIsKept) {
  const std::string label(MAX_LABEL_BYTES, '"');
  const std::string text(MAX_TEXT_BYTES, '"');
  const std::string worst = row("2026-10-22T08:00:00", label.c_str(), "datetime", text.c_str());
  const std::string log = withHeader(worst + "2026-10-23T08:00:00,Mood,stars,1\r\n");
  const LastEntry e = scanAllChunkings(log, label.c_str());
  ASSERT_TRUE(e.found);
  EXPECT_STREQ(e.timestamp, "2026-10-22T08:00:00");
  EXPECT_EQ(std::string(e.value), text);
}

TEST(TrackingLastEntry, QuotedNewlineSkippedEarlierRowsIntact) {
  // A PC edit put a line break inside a quoted value: its pieces fail to
  // parse and are skipped, and older rows are still found.
  const std::string log = withHeader(
      "2026-10-20T08:00:00,Journal,text,older\n"
      "2026-10-21T08:00:00,Journal,text,\"two\nlines, \"\"quoted\"\"\"\n"
      "2026-10-22T08:00:00,Mood,stars,3\n");
  const LastEntry e = scanAllChunkings(log, "Journal");
  ASSERT_TRUE(e.found);
  EXPECT_STREQ(e.timestamp, "2026-10-20T08:00:00");
  EXPECT_STREQ(e.value, "older");
  const LastEntry mood = scanAllChunkings(log, "Mood");
  ASSERT_TRUE(mood.found);
  EXPECT_STREQ(mood.value, "3");
}

TEST(TrackingLastEntry, StopsAtNewestMatch) {
  // Bytes before the match are never needed: garbage there is not reached.
  auto scanner = std::make_unique<LastEntryScanner>("Mood");
  const std::string tail = "\n2026-10-22T08:00:00,Mood,stars,4\n2026-10-23T08:00:00,Other,none,\n";
  EXPECT_TRUE(scanner->feedBackward(tail.data(), tail.size()));
  const std::string older = withHeader("2026-10-21T08:00:00,Mood,stars,1");
  EXPECT_TRUE(scanner->feedBackward(older.data(), older.size()));
  scanner->finishAtFileStart();
  EXPECT_STREQ(scanner->result().value, "4");
}

TEST(TrackingLastEntry, LargeLogAcrossManyChunks) {
  std::string rows;
  for (int i = 0; i < 2000; i++) {
    rows += row("2026-10-22T08:00:00", i % 3 == 0 ? "Mood" : "Weight", "number", std::to_string(i).c_str());
  }
  const LastEntry e = scanAllChunkings(withHeader(rows), "Mood");
  ASSERT_TRUE(e.found);
  EXPECT_STREQ(e.value, "1998");
  const LastEntry w = scanAllChunkings(withHeader(rows), "Weight");
  ASSERT_TRUE(w.found);
  EXPECT_STREQ(w.value, "1999");
}

TEST(TrackingLastEntry, TimestampMustBeExact) {
  const std::string older = "2026-10-20T08:00:00,Mood,stars,1\n";
  for (const char* ts : {"", "t", "2026-10-22 08:00:00", "2026-10-22T08:00", "2026/10/22T08:00:00",
                         "20a6-10-22T08:00:00", "2026-10-22T08:00:0x", "2026-10-22T08-00:00", "\"2026-10-22T08:00\""}) {
    const LastEntry e = scanAllChunkings(withHeader(older + ts + ",Mood,stars,5\n"), "Mood");
    ASSERT_TRUE(e.found) << ts;
    EXPECT_STREQ(e.value, "1") << ts;
  }
  // Quoting is not part of the timestamp.
  const LastEntry quoted = scanAllChunkings(withHeader(older + "\"2026-10-22T08:00:00\",Mood,stars,5\n"), "Mood");
  EXPECT_STREQ(quoted.value, "5");
}

TEST(TrackingLastEntry, UnsetClockSentinelRowStillShows) {
  // appendEntry writes 1970-01-01T00:00:00 when the clock is unset; the shape
  // check must not reject it.
  const LastEntry e = scanAllChunkings(withHeader("1970-01-01T00:00:00,Mood,stars,3\n"), "Mood");
  ASSERT_TRUE(e.found);
  EXPECT_STREQ(e.timestamp, "1970-01-01T00:00:00");
  EXPECT_STREQ(e.value, "3");
}

TEST(TrackingLastEntry, QuotedNewlineFragmentThatParsesIsSkipped) {
  // The middle piece "b,Journal,text,c" is a well-formed row but has no timestamp.
  const std::string log = withHeader(
      "2026-10-20T08:00:00,Journal,text,older\n"
      "2026-10-21T08:00:00,Journal,text,\"a\nb,Journal,text,c\n\"\n");
  const LastEntry e = scanAllChunkings(log, "Journal");
  ASSERT_TRUE(e.found);
  EXPECT_STREQ(e.value, "older");
}

TEST(TrackingLogTail, CapBoundary) {
  const std::string match = "2026-10-22T08:00:00,Mood,stars,4\n";
  const std::string head = withHeader("2026-10-21T08:00:00,Mood,stars,1\n");
  const std::string log = head + match + fillerLog(5000).substr(sizeof(CSV_HEADER));
  const size_t fromMatch = log.size() - head.size();  // bytes from the match's first byte to the end
  for (size_t chunk : {1u, 7u, 512u, 700u, 4096u}) {
    // The line end before the match is inside the cap: the whole line is seen.
    TailScan r = scanTail(log, "Mood", chunk, fromMatch + 1);
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.entry.found) << "chunk " << chunk;
    EXPECT_STREQ(r.entry.value, "4");
    // Only the match's bytes are inside: they could be the tail of a longer
    // line, so they are not taken, and nothing older is read.
    r = scanTail(log, "Mood", chunk, fromMatch);
    ASSERT_TRUE(r.ok);
    EXPECT_FALSE(r.entry.found) << "chunk " << chunk;
    r = scanTail(log, "Mood", chunk, fromMatch - 1);
    ASSERT_TRUE(r.ok);
    EXPECT_FALSE(r.entry.found) << "chunk " << chunk;
    size_t lowest = SIZE_MAX;
    for (const auto& read : r.reads) lowest = std::min(lowest, read.first);
    EXPECT_EQ(lowest, log.size() - (fromMatch - 1)) << "chunk " << chunk;
  }
}

TEST(TrackingLogTail, DeviceCapOnALargeLog) {
  const size_t cap = 256 * 1024;
  const std::string old = withHeader("2026-10-01T08:00:00,Mood,stars,1\n");
  const std::string filler = fillerLog(cap).substr(sizeof(CSV_HEADER));
  EXPECT_FALSE(scanTail(old + filler, "Mood", 4096, cap).entry.found);
  const TailScan r = scanTail(old + filler + "2026-10-02T08:00:00,Mood,stars,2\n", "Mood", 4096, cap);
  ASSERT_TRUE(r.ok);
  ASSERT_TRUE(r.entry.found);
  EXPECT_STREQ(r.entry.value, "2");
}

TEST(TrackingLogTail, FileOfExactlyOneAndTwoChunks) {
  for (size_t size : {size_t{4096}, size_t{8192}}) {
    // The match is the first line (no header), so only finishAtFileStart() finds it.
    std::string log = "2026-10-22T08:00:00,Mood,stars,3\n";
    while (log.size() + 200 < size) log += row("2026-10-01T08:00:00", "Filler", "number", "1");
    const std::string pad = "2026-10-01T08:00:00,Pad,text,";
    log += pad + std::string(size - log.size() - pad.size() - 1, 'x') + "\n";
    ASSERT_EQ(log.size(), size);
    const TailScan r = scanTail(log, "Mood");
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.entry.found) << size;
    EXPECT_STREQ(r.entry.value, "3");
    ASSERT_EQ(r.reads.size(), size / 4096);
    for (size_t i = 0; i < r.reads.size(); i++) {
      EXPECT_EQ(r.reads[i].first, size - (i + 1) * 4096);
      EXPECT_EQ(r.reads[i].second, 4096u);
    }
    EXPECT_FALSE(scanTail(log, "Other").entry.found);
  }
}

TEST(TrackingLogTail, FileSmallerThanOneChunk) {
  const std::string log = withHeader("2026-10-22T08:00:00,Mood,stars,3\n");
  const TailScan r = scanTail(log, "Mood");
  ASSERT_TRUE(r.ok);
  ASSERT_TRUE(r.entry.found);
  ASSERT_EQ(r.reads.size(), 1u);
  EXPECT_EQ(r.reads[0], std::make_pair(size_t{0}, log.size()));
  // Empty file: no read at all.
  const TailScan empty = scanTail("", "Mood");
  EXPECT_TRUE(empty.ok);
  EXPECT_FALSE(empty.entry.found);
  EXPECT_TRUE(empty.reads.empty());
}

TEST(TrackingLogTail, FailedReadGivesNoHint) {
  const std::string log = withHeader("2026-10-20T08:00:00,Mood,stars,1\n") +
                          fillerLog(20000).substr(sizeof(CSV_HEADER)) + "2026-10-22T08:00:00,Weight,number,70\n";
  TailScan r = scanTail(log, "Mood", 4096, SIZE_MAX, 0);
  EXPECT_FALSE(r.ok);
  EXPECT_FALSE(r.entry.found);
  r = scanTail(log, "Mood", 4096, SIZE_MAX, 2);  // fails before the match's chunk
  EXPECT_FALSE(r.ok);
  EXPECT_FALSE(r.entry.found);
  // A failure after the match is never reached.
  r = scanTail(log, "Weight", 4096, SIZE_MAX, 1);
  EXPECT_TRUE(r.ok);
  EXPECT_TRUE(r.entry.found);
}

TEST(TrackingLogTail, ReadsAreAlignedContiguousAndBounded) {
  const std::string log = fillerLog(30000) + "2026-10-22T08:00:00,Weight,number,70\n";
  ASSERT_NE(log.size() % LOG_READ_ALIGN, 0u);
  for (size_t cap : {SIZE_MAX, size_t{12345}, size_t{8192}}) {
    for (size_t chunk : {size_t{512}, size_t{1024}, size_t{4096}}) {
      const TailScan r = scanTail(log, "Mood", chunk, cap);
      ASSERT_TRUE(r.ok);
      const size_t scanStart = cap < log.size() ? log.size() - cap : 0;
      size_t end = log.size();
      for (const auto& read : r.reads) {
        EXPECT_EQ(read.first + read.second, end);
        EXPECT_LE(read.second, chunk);
        EXPECT_GT(read.second, 0u);
        if (read.first != scanStart) {
          EXPECT_EQ(read.first % LOG_READ_ALIGN, 0u) << read.first;
        }
        end = read.first;
      }
      EXPECT_EQ(end, scanStart) << "cap " << cap << " chunk " << chunk;
    }
  }
}

TEST(TrackingLogTail, AlignmentLosesNoRows) {
  // Shift every row across the 512-byte grid; every chunking finds the same rows.
  std::string rows;
  for (int i = 0; i < 300; i++) {
    rows += row("2026-10-22T08:00:00", i % 7 == 0 ? "Mood" : "Weight", "number", std::to_string(i).c_str());
  }
  for (size_t shift = 0; shift < 2 * LOG_READ_ALIGN; shift += 37) {
    const std::string log = withHeader(std::string(shift, '\n') + "2026-10-21T08:00:00,Rare,none,\n" + rows);
    EXPECT_STREQ(scanAllChunkings(log, "Mood").value, "294") << shift;
    EXPECT_TRUE(scanAllChunkings(log, "Rare").found) << shift;
    EXPECT_FALSE(scanAllChunkings(log, "Absent").found) << shift;
  }
}
