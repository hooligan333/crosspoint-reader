#pragma once
#ifdef CROSSPOINT_TRACKING

#include <I18n.h>
#include <TrackingConfig.h>
#include <TrackingFormat.h>

#include <cstdint>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

/**
 * Value entry for one tracked item, pushed over TrackingMenuActivity.
 *
 * One list per type, always ending in Confirm and Go Back rows, so every
 * control is reachable with Up/Down + Confirm and by touch:
 *   number, text  a Value row that opens the keyboard
 *   date          Year, Month, Day rows (+ Hour, Minute for datetime), each
 *                 opening the shared slider dialog; pre-filled with local now
 *   boolean       True / False checkbox rows, radio-style, none preset
 *   list          one checkbox row per option, radio-style, none preset
 *   stars         five stars drawn above the list; they take selection
 *                 indices 0..4 ahead of the rows, so button navigation walks
 *                 the stars first
 *   none          a prompt line above the two rows
 * An item with timestamp_override adds, before Confirm, a Timestamp row that
 * toggles Now / Custom and Stamp year..minute rows for the custom stamp. Now
 * stamps at Confirm and its rows show Not set; until a stamp field has been
 * picked, entering Custom or opening a field re-fills them with local now.
 * Picking a field switches to Custom.
 * Nothing is logged until Confirm validates the input and the CSV row is on
 * the card; then the activity finishes with a non-cancelled result. Problems
 * are reported in a popup and the screen stays open.
 */
class TrackingEntryActivity final : public UiListActivity {
 public:
  TrackingEntryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const tracking::Config& config,
                        const tracking::Item& item);

  void render(RenderLock&& lock) override;

 private:
  enum class RowKind : uint8_t {
    Value,
    Year,
    Month,
    Day,
    Hour,
    Minute,
    True,
    False,
    Option,
    Star,
    TsMode,
    TsYear,
    TsMonth,
    TsDay,
    TsHour,
    TsMinute,
    Confirm,
    GoBack
  };

  static constexpr int STAR_COUNT = 5;
  static constexpr int TIMESTAMP_ROWS = 6;  // TsMode + TsYear..TsMinute
  // Worst case: a full list item with timestamp_override.
  static constexpr int MAX_ROWS = static_cast<int>(tracking::MAX_OPTIONS) + TIMESTAMP_ROWS + 2;
  // addRow() drops rows past MAX_ROWS: each type's own rows (list MAX_OPTIONS,
  // datetime 5, stars STAR_COUNT) + stamp rows + Confirm/Go Back must fit.
  static_assert(MAX_ROWS >= static_cast<int>(tracking::MAX_OPTIONS) + TIMESTAMP_ROWS + 2 &&
                    MAX_ROWS >= 5 + TIMESTAMP_ROWS + 2 && MAX_ROWS >= STAR_COUNT + TIMESTAMP_ROWS + 2,
                "MAX_ROWS cannot hold every item type with timestamp_override");

  int listCount() const override { return rowCount; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  void onBackButton() override;
  const char* headerTitle() const override { return item.label; }

  void addRow(RowKind kind, StrId label, int16_t arg = 0);
  void refreshRowValues();
  void drawStars(UiScreen& screen);

  void editValue();
  // `field` is Year..Minute; `stamp` edits tsDateTime instead of dateTime.
  void editDateField(RowKind field, bool stamp);
  // Opens the stamp picker for `field` (Year..Minute).
  void editStampField(RowKind field);
  // Sets tsDateTime to local now, or FALLBACK_YEAR-01-01 00:00 when unset.
  void prefillStamp();
  void confirm();
  void cancel();
  void showProblem(const char* message);

  const tracking::Config& config;
  const tracking::Item& item;

  // Rows [0, leadingRows) are drawn outside the list (the stars); the list
  // shows the rest. Built once in the constructor; only values/toggles change after.
  RowKind kinds[MAX_ROWS] = {};
  int16_t args[MAX_ROWS] = {};
  freeink::ui::ListItem rows[MAX_ROWS]{};
  int rowCount = 0;
  int leadingRows = 0;

  // Entered state, one field per type family.
  char entered[tracking::MAX_TEXT_BYTES + 1] = {};  // number / text
  tracking::DateTime dateTime{};
  char dateText[5][6] = {};   // Year..Minute row values
  int8_t booleanChoice = -1;  // -1 none, 0 false, 1 true
  int16_t optionChoice = -1;
  uint8_t stars = 0;
  char starsText[8] = {};

  // timestamp_override: a date/datetime item shows both groups of date rows,
  // so the stamp has its own text buffers.
  tracking::DateTime tsDateTime{};
  char tsText[5][6] = {};  // TsYear..TsMinute row values
  bool tsCustom = false;   // false: stamp with local now at Confirm
  bool tsEdited = false;   // a stamp field was picked; no more re-fills

  OptionPopup problemPopup;
};

#endif  // CROSSPOINT_TRACKING
