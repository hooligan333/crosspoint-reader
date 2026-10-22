#include "TrackingEntryActivity.h"

#ifdef CROSSPOINT_TRACKING

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "MappedInputManager.h"
#include "TrackingStore.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {

// A five-point star as ten vertices, outer and inner alternating clockwise
// from the top, in thousandths of the outer radius (inner radius 0.382, the
// regular star). Integer constants keep trig off the render path.
constexpr int16_t STAR_X[10] = {0, 225, 951, 363, 588, 0, -588, -363, -951, -225};
constexpr int16_t STAR_Y[10] = {-1000, -309, -309, 118, 809, 382, 809, 118, -309, -309};
// The star spans -1000..809 vertically; this shift centres it in its cell.
constexpr int STAR_CENTRE_SHIFT = 95;
constexpr int16_t MAX_STAR_CELL = 96;

}  // namespace

TrackingEntryActivity::TrackingEntryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const tracking::Config& config, const tracking::Item& item)
    : UiListActivity("TrackingEntry", renderer, mappedInput), config(config), item(item) {
  // Built here, not in onEnter(): onEnter() runs without the render lock, so
  // a frame drawn meanwhile could see half-built rows.
  switch (item.type) {
    case tracking::ItemType::Number:
    case tracking::ItemType::Text:
      addRow(RowKind::Value, StrId::STR_TRACKING_VALUE);
      break;
    case tracking::ItemType::Date:
    case tracking::ItemType::DateTime:
      if (!tracking::localNow(dateTime)) dateTime = tracking::DateTime{tracking::FALLBACK_YEAR, 1, 1, 0, 0, 0};
      tracking::clampDateTime(dateTime);
      addRow(RowKind::Year, StrId::STR_TRACKING_YEAR);
      addRow(RowKind::Month, StrId::STR_TRACKING_MONTH);
      addRow(RowKind::Day, StrId::STR_TRACKING_DAY);
      if (item.type == tracking::ItemType::DateTime) {
        addRow(RowKind::Hour, StrId::STR_TRACKING_HOUR);
        addRow(RowKind::Minute, StrId::STR_TRACKING_MINUTE);
      }
      break;
    case tracking::ItemType::Boolean:
      addRow(RowKind::True, StrId::STR_TRACKING_TRUE);
      addRow(RowKind::False, StrId::STR_TRACKING_FALSE);
      break;
    case tracking::ItemType::List:
      for (int i = 0; i < item.optionCount; i++) addRow(RowKind::Option, StrId::STR_NONE_OPT, static_cast<int16_t>(i));
      break;
    case tracking::ItemType::Stars:
      for (int k = 0; k < STAR_COUNT; k++) addRow(RowKind::Star, StrId::STR_NONE_OPT, static_cast<int16_t>(k));
      leadingRows = STAR_COUNT;
      break;
    case tracking::ItemType::None:
      break;
  }
  if (item.timestampOverride) {
    prefillStamp();
    addRow(RowKind::TsMode, StrId::STR_TRACKING_TIMESTAMP);
    addRow(RowKind::TsYear, StrId::STR_TRACKING_TS_YEAR);
    addRow(RowKind::TsMonth, StrId::STR_TRACKING_TS_MONTH);
    addRow(RowKind::TsDay, StrId::STR_TRACKING_TS_DAY);
    addRow(RowKind::TsHour, StrId::STR_TRACKING_TS_HOUR);
    addRow(RowKind::TsMinute, StrId::STR_TRACKING_TS_MINUTE);
  }
  addRow(RowKind::Confirm, StrId::STR_CONFIRM);
  addRow(RowKind::GoBack, StrId::STR_GO_BACK);
  refreshRowValues();
}

void TrackingEntryActivity::addRow(const RowKind kind, const StrId label, const int16_t arg) {
  if (rowCount >= MAX_ROWS) return;
  kinds[rowCount] = kind;
  args[rowCount] = arg;
  rows[rowCount] = fui::ListItem{};
  rows[rowCount].label = kind == RowKind::Option ? config.option(item, arg) : I18N.get(label);
  rows[rowCount].actionValue = static_cast<int16_t>(rowCount);
  rowCount++;
}

// Caller holds the RenderLock once the screen is up: row values are read by
// the render task.
void TrackingEntryActivity::refreshRowValues() {
  for (int i = 0; i < rowCount; i++) {
    fui::ListItem& row = rows[i];
    switch (kinds[i]) {
      case RowKind::Value:
        row.value = entered[0] != '\0' ? entered : tr(STR_NOT_SET);
        break;
      case RowKind::Year:
        snprintf(dateText[0], sizeof(dateText[0]), "%u", static_cast<unsigned>(dateTime.year));
        row.value = dateText[0];
        break;
      case RowKind::Month:
        snprintf(dateText[1], sizeof(dateText[1]), "%02u", static_cast<unsigned>(dateTime.month));
        row.value = dateText[1];
        break;
      case RowKind::Day:
        snprintf(dateText[2], sizeof(dateText[2]), "%02u", static_cast<unsigned>(dateTime.day));
        row.value = dateText[2];
        break;
      case RowKind::Hour:
        snprintf(dateText[3], sizeof(dateText[3]), "%02u", static_cast<unsigned>(dateTime.hour));
        row.value = dateText[3];
        break;
      case RowKind::Minute:
        snprintf(dateText[4], sizeof(dateText[4]), "%02u", static_cast<unsigned>(dateTime.minute));
        row.value = dateText[4];
        break;
      case RowKind::True:
        GUI.setCheckboxRow(row, booleanChoice == 1);
        break;
      case RowKind::False:
        GUI.setCheckboxRow(row, booleanChoice == 0);
        break;
      case RowKind::Option:
        GUI.setCheckboxRow(row, optionChoice == args[i]);
        break;
      case RowKind::TsMode:
        row.value = tsCustom ? tr(STR_TRACKING_TS_CUSTOM) : tr(STR_TRACKING_TS_NOW);
        break;
      // In Now mode the stamp is taken at Confirm; digits here would read as a
      // stale clock, so the fields show Not set until Custom.
      case RowKind::TsYear:
        snprintf(tsText[0], sizeof(tsText[0]), "%u", static_cast<unsigned>(tsDateTime.year));
        row.value = tsCustom ? tsText[0] : tr(STR_NOT_SET);
        break;
      case RowKind::TsMonth:
        snprintf(tsText[1], sizeof(tsText[1]), "%02u", static_cast<unsigned>(tsDateTime.month));
        row.value = tsCustom ? tsText[1] : tr(STR_NOT_SET);
        break;
      case RowKind::TsDay:
        snprintf(tsText[2], sizeof(tsText[2]), "%02u", static_cast<unsigned>(tsDateTime.day));
        row.value = tsCustom ? tsText[2] : tr(STR_NOT_SET);
        break;
      case RowKind::TsHour:
        snprintf(tsText[3], sizeof(tsText[3]), "%02u", static_cast<unsigned>(tsDateTime.hour));
        row.value = tsCustom ? tsText[3] : tr(STR_NOT_SET);
        break;
      case RowKind::TsMinute:
        snprintf(tsText[4], sizeof(tsText[4]), "%02u", static_cast<unsigned>(tsDateTime.minute));
        row.value = tsCustom ? tsText[4] : tr(STR_NOT_SET);
        break;
      case RowKind::Star:
      case RowKind::Confirm:
      case RowKind::GoBack:
        break;
    }
  }
  snprintf(starsText, sizeof(starsText), "%u / %d", static_cast<unsigned>(stars), STAR_COUNT);
}

// --- Rendering ---

void TrackingEntryActivity::render(RenderLock&& lock) {
  // The popup paints over the last frame and flips the buffer itself.
  if (problemPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

void TrackingEntryActivity::drawStars(UiScreen& screen) {
  const fui::Rect body = screen.body();
  const int16_t pad = screen.theme().spaceLg;
  const int16_t usable = static_cast<int16_t>(body.width - 2 * pad);
  const int16_t cellW = static_cast<int16_t>(usable / STAR_COUNT);
  const int16_t cellH = std::min<int16_t>(cellW, MAX_STAR_CELL);
  fui::Rect band = screen.takeTop(cellH, screen.theme().spaceSm);
  band.x = static_cast<int16_t>(band.x + pad + (usable - cellW * STAR_COUNT) / 2);

  const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
  const int selected = nav.selected.load();
  const int radius = std::min<int>(cellW, cellH) * 42 / 100;
  for (int k = 0; k < STAR_COUNT; k++) {
    const fui::Rect cell{static_cast<int16_t>(band.x + k * cellW), band.y, cellW, cellH};
    screen.frame().hit(cell, ACTION_ROW, static_cast<int16_t>(k), fui::InputTouch);

    const int cx = cell.x + cell.width / 2;
    const int cy = cell.y + cell.height / 2 + radius * STAR_CENTRE_SHIFT / 1000;
    fui::Point points[10];
    for (int v = 0; v < 10; v++) {
      points[v] = fui::Point{static_cast<int16_t>(cx + STAR_X[v] * radius / 1000),
                             static_cast<int16_t>(cy + STAR_Y[v] * radius / 1000)};
    }
    if (k < stars) {
      const fui::Point centre{static_cast<int16_t>(cx), static_cast<int16_t>(cy)};
      for (int v = 0; v < 10; v++) screen.target().triangle(centre, points[v], points[(v + 1) % 10], ink);
    }
    for (int v = 0; v < 10; v++) screen.target().line(points[v], points[(v + 1) % 10], 2, ink);
    // Button users need to see which star Confirm will act on.
    if (selected == k) {
      screen.target().stroke(fui::Rect{static_cast<int16_t>(cell.x + 2), static_cast<int16_t>(cell.y + 1),
                                       static_cast<int16_t>(cell.width - 4), static_cast<int16_t>(cell.height - 2)},
                             ink, 2, screen.theme().listRowRadius);
    }
  }

  auto readout = screen.theme().bodyText;
  readout.align = fui::TextAlign::Center;
  const int16_t lineH = screen.target().lineHeight(readout.font);
  screen.target().text(screen.takeTop(lineH, screen.theme().spaceMd), starsText, readout);
}

void TrackingEntryActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (item.type == tracking::ItemType::None) {
    auto prompt = screen.theme().bodyText;
    prompt.align = fui::TextAlign::Center;
    prompt.maxLines = 2;
    const int16_t lineH = screen.target().lineHeight(prompt.font);
    screen.target().text(screen.takeTop(static_cast<int16_t>(lineH * 2), screen.theme().spaceMd),
                         tr(STR_TRACKING_NONE_PROMPT), prompt);
  }
  if (leadingRows > 0) drawStars(screen);

  fui::ListProps props;
  props.items = rows + leadingRows;
  props.count = static_cast<uint16_t>(rowCount - leadingRows);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  if (leadingRows == 0) {
    syncListViewport(screen, props);
  } else {
    // The stars own selection indices 0..leadingRows-1; the list only shows
    // the highlight once the selection reaches its rows.
    props.toggleCheckbox = true;
    props.partialTrailingRow = true;
    screen.syncListViewport(nav, props, rowCount - leadingRows, leadingRows);
  }
  screen.list(props);
}

// --- Input ---

bool TrackingEntryActivity::handleCustomInput() {
  return problemPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void TrackingEntryActivity::onBackButton() { cancel(); }

void TrackingEntryActivity::activateIndex(const int index) {
  if (index < 0 || index >= rowCount) return;
  nav.selected = index;
  switch (kinds[index]) {
    case RowKind::Value:
      app.clearTapFlash();  // the keyboard covers this screen
      editValue();
      return;
    case RowKind::Year:
    case RowKind::Month:
    case RowKind::Day:
    case RowKind::Hour:
    case RowKind::Minute:
      app.clearTapFlash();
      editDateField(kinds[index], false);
      return;
    case RowKind::TsYear:
      app.clearTapFlash();
      editStampField(RowKind::Year);
      return;
    case RowKind::TsMonth:
      app.clearTapFlash();
      editStampField(RowKind::Month);
      return;
    case RowKind::TsDay:
      app.clearTapFlash();
      editStampField(RowKind::Day);
      return;
    case RowKind::TsHour:
      app.clearTapFlash();
      editStampField(RowKind::Hour);
      return;
    case RowKind::TsMinute:
      app.clearTapFlash();
      editStampField(RowKind::Minute);
      return;
    case RowKind::Confirm:
      confirm();
      return;
    case RowKind::GoBack:
      cancel();
      return;
    case RowKind::True:
    case RowKind::False:
    case RowKind::Option:
    case RowKind::Star:
    case RowKind::TsMode:
      break;
  }
  {
    RenderLock lock(*this);
    switch (kinds[index]) {
      case RowKind::True:
        booleanChoice = 1;
        break;
      case RowKind::False:
        booleanChoice = 0;
        break;
      case RowKind::Option:
        optionChoice = args[index];
        break;
      case RowKind::Star: {
        // Tapping the star that is already the rating clears it to zero.
        const uint8_t value = static_cast<uint8_t>(args[index] + 1);
        stars = stars == value ? 0 : value;
        break;
      }
      case RowKind::TsMode:
        tsCustom = !tsCustom;
        // Untouched fields would otherwise show the time the screen opened.
        if (tsCustom && !tsEdited) prefillStamp();
        break;
      default:
        break;
    }
    refreshRowValues();
  }
  requestUpdate();
}

void TrackingEntryActivity::editValue() {
  const bool number = item.type == tracking::ItemType::Number;
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(
      renderer, mappedInput, item.label, entered, number ? tracking::MAX_NUMBER_INPUT_BYTES : tracking::MAX_TEXT_BYTES,
      InputType::Text);
  if (!keyboard) {
    LOG_ERR("TRK", "OOM: keyboard (%u bytes)", static_cast<unsigned>(sizeof(KeyboardEntryActivity)));
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    const auto* typed = std::get_if<KeyboardResult>(&result.data);
    if (result.isCancelled || !typed) return;
    RenderLock lock(*this);
    snprintf(entered, sizeof(entered), "%s", typed->text.c_str());
    refreshRowValues();
  });
}

// Caller holds the RenderLock once the screen is up.
void TrackingEntryActivity::prefillStamp() {
  if (!tracking::localNow(tsDateTime)) tsDateTime = tracking::DateTime{tracking::FALLBACK_YEAR, 1, 1, 0, 0, 0};
  tracking::clampDateTime(tsDateTime);
}

void TrackingEntryActivity::editStampField(const RowKind field) {
  if (!tsCustom && !tsEdited) {
    // The picker starts from now, not from when the screen opened.
    RenderLock lock(*this);
    prefillStamp();
    refreshRowValues();
  }
  editDateField(field, true);
}

void TrackingEntryActivity::editDateField(const RowKind field, const bool stamp) {
  const tracking::DateTime& current = stamp ? tsDateTime : dateTime;
  StrId title = stamp ? StrId::STR_TRACKING_TS_YEAR : StrId::STR_TRACKING_YEAR;
  int value = current.year;
  int minValue = tracking::YEAR_MIN;
  int maxValue = tracking::YEAR_MAX;
  int largeStep = 10;
  switch (field) {
    case RowKind::Month:
      title = stamp ? StrId::STR_TRACKING_TS_MONTH : StrId::STR_TRACKING_MONTH;
      value = current.month;
      minValue = 1;
      maxValue = 12;
      largeStep = 3;
      break;
    case RowKind::Day:
      title = stamp ? StrId::STR_TRACKING_TS_DAY : StrId::STR_TRACKING_DAY;
      value = current.day;
      minValue = 1;
      maxValue = tracking::daysInMonth(current.year, current.month);
      largeStep = 7;
      break;
    case RowKind::Hour:
      title = stamp ? StrId::STR_TRACKING_TS_HOUR : StrId::STR_TRACKING_HOUR;
      value = current.hour;
      minValue = 0;
      maxValue = 23;
      largeStep = 6;
      break;
    case RowKind::Minute:
      title = stamp ? StrId::STR_TRACKING_TS_MINUTE : StrId::STR_TRACKING_MINUTE;
      value = current.minute;
      minValue = 0;
      maxValue = 59;
      largeStep = 10;
      break;
    default:
      break;
  }
  auto picker = makeUniqueNoThrow<IntervalSelectionActivity>(renderer, mappedInput, "TrackingField", title, value,
                                                             minValue, maxValue, 1, largeStep);
  if (!picker) {
    LOG_ERR("TRK", "OOM: field picker (%u bytes)", static_cast<unsigned>(sizeof(IntervalSelectionActivity)));
    return;
  }
  startActivityForResult(std::move(picker), [this, field, stamp](const ActivityResult& result) {
    const auto* picked = std::get_if<IntervalResult>(&result.data);
    if (result.isCancelled || !picked) return;
    RenderLock lock(*this);
    tracking::DateTime& target = stamp ? tsDateTime : dateTime;
    const auto v = static_cast<uint8_t>(picked->value);
    switch (field) {
      case RowKind::Year:
        target.year = static_cast<uint16_t>(picked->value);
        break;
      case RowKind::Month:
        target.month = v;
        break;
      case RowKind::Day:
        target.day = v;
        break;
      case RowKind::Hour:
        target.hour = v;
        break;
      case RowKind::Minute:
        target.minute = v;
        break;
      default:
        break;
    }
    // A year or month change can leave the day past the new month's end.
    tracking::clampDateTime(target);
    // Editing a stamp field asks for that stamp; Now would drop the edit.
    if (stamp) {
      tsCustom = true;
      tsEdited = true;  // later Custom toggles keep the picked stamp
    }
    refreshRowValues();
  });
}

void TrackingEntryActivity::showProblem(const char* message) {
  const char* options[] = {tr(STR_OK_BUTTON)};
  problemPopup.showMessage("", message, options, 1, 0, nullptr);
  requestUpdate();
}

void TrackingEntryActivity::confirm() {
  // ~225 B, off the stack for this call. OOM reports as a failed write.
  struct Scratch {
    char value[tracking::MAX_TEXT_BYTES + 1];  // large enough for every value form; text is the longest
    char problem[96];
  };
  auto scratch = makeUniqueNoThrow<Scratch>();
  if (!scratch) {
    LOG_ERR("TRK", "OOM: confirm buffers (%u bytes)", static_cast<unsigned>(sizeof(Scratch)));
    showProblem(tr(STR_TRACKING_WRITE_FAILED));
    return;
  }
  char* const value = scratch->value;
  const size_t valueBytes = sizeof(scratch->value);
  value[0] = '\0';
  switch (item.type) {
    case tracking::ItemType::Number:
      switch (const tracking::NumberCheck check = tracking::checkNumber(entered, item, value, valueBytes)) {
        case tracking::NumberCheck::Ok:
          break;
        case tracking::NumberCheck::Empty:
          showProblem(tr(STR_TRACKING_NEED_VALUE));
          return;
        case tracking::NumberCheck::NotANumber:
          showProblem(tr(STR_TRACKING_NOT_NUMBER));
          return;
        case tracking::NumberCheck::NotInteger:
          showProblem(tr(STR_TRACKING_NOT_INTEGER));
          return;
        case tracking::NumberCheck::BelowMin:
        case tracking::NumberCheck::AboveMax: {
          const bool below = check == tracking::NumberCheck::BelowMin;
          // %.15g round-trips any bound written with up to 15 significant digits.
          char bound[32];
          snprintf(bound, sizeof(bound), "%.15g", below ? item.minValue : item.maxValue);
          snprintf(scratch->problem, sizeof(scratch->problem),
                   below ? tr(STR_TRACKING_MIN_FORMAT) : tr(STR_TRACKING_MAX_FORMAT), bound);
          showProblem(scratch->problem);
          return;
        }
      }
      break;
    case tracking::ItemType::Text:
      if (entered[strspn(entered, " \t")] == '\0') {
        showProblem(tr(STR_TRACKING_NEED_VALUE));
        return;
      }
      snprintf(value, valueBytes, "%s", entered);
      break;
    case tracking::ItemType::Date:
      tracking::formatDate(dateTime, value, valueBytes);
      break;
    case tracking::ItemType::DateTime:
      tracking::formatDateTime(dateTime, value, valueBytes);
      break;
    case tracking::ItemType::Boolean:
      if (booleanChoice < 0) {
        showProblem(tr(STR_TRACKING_NEED_CHOICE));
        return;
      }
      snprintf(value, valueBytes, "%s", tracking::booleanValue(booleanChoice == 1));
      break;
    case tracking::ItemType::Stars:
      snprintf(value, valueBytes, "%u", static_cast<unsigned>(stars));
      break;
    case tracking::ItemType::List:
      if (optionChoice < 0) {
        showProblem(tr(STR_TRACKING_NEED_CHOICE));
        return;
      }
      snprintf(value, valueBytes, "%s", config.option(item, optionChoice));
      break;
    case tracking::ItemType::None:
      break;
  }

  tracking::DateTime stampTime{};
  const tracking::DateTime* overrideTimestamp = nullptr;
  if (tsCustom) {
    stampTime = tsDateTime;
    stampTime.second = 0;  // the pickers stop at minutes
    tracking::clampDateTime(stampTime);
    overrideTimestamp = &stampTime;
  }
  switch (tracking::appendEntry(item, value, overrideTimestamp)) {
    case tracking::AppendResult::Ok:
      break;
    case tracking::AppendResult::NotWritten:
      showProblem(tr(STR_TRACKING_WRITE_FAILED));
      return;
    case tracking::AppendResult::MaybeWritten:
      showProblem(tr(STR_TRACKING_WRITE_UNCERTAIN));
      return;
    case tracking::AppendResult::NoRotationSlot:
      showProblem(tr(STR_TRACKING_NO_ROTATION_SLOT));
      return;
  }
  app.clearTapFlash();
  setResult(ActivityResult{});
  finish();
}

void TrackingEntryActivity::cancel() {
  app.clearTapFlash();
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

#endif  // CROSSPOINT_TRACKING
