#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
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
