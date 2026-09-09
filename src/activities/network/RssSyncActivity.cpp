#include "RssSyncActivity.h"

#ifdef CROSSPOINT_RSS_SYNC

#include <Arduino.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#ifdef CROSSPOINT_SOFT_CLOCK
#include <HalClock.h>  // the soft clock's time-fix hook in startFetch()
#endif
#include <HalHeapGauge.h>  // gate*Heap() for the pre-reserve guard in fetchFeed()
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <RssParser.h>
#include <WiFi.h>
#include <strings.h>

#include <cstdio>
#include <cstring>
#include <utility>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/DestFolder.h"
#include "util/UrlUtils.h"

namespace fui = freeink::ui;

namespace {
// Downloads land here first and are renamed into place only once complete, so
// an interrupted transfer never leaves a half file that looks like a book.
constexpr const char* TMP_LEAF = "/.tmp-rss";
constexpr size_t MAX_FILENAME_CHARS = 80;
// Same repaint throttle OpdsBookBrowserActivity uses for its download bar:
// e-ink refreshes cost more than the progress they show.
constexpr int PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long PROGRESS_MIN_UPDATE_MS = 5000;

// THE TWO reserve() CALLS ON THE FETCH PATH CANNOT FAIL SOFTLY. Under
// -fno-exceptions an allocation failure inside std::vector::reserve() does not
// return an error, it goes __throw_bad_alloc -> __cxa_throw ->
// _Unwind_RaiseException with no handler in range -> std::terminate() ->
// abort(): a panic and a reboot, mid-sync. (Verified by disassembling the
// linked [env:default] image; the same hazard is already documented in-tree at
// src/util/DictZip.cpp:183.) Both are gated below the way DictZip guards its
// chunk table — a largest-free-block check against the exact block the reserve
// is about to ask for.
//
// Slack required above that block, matching DictZip's
// CHUNK_TABLE_HEAP_HEADROOM_BYTES.
constexpr size_t RESERVE_HEAP_HEADROOM_BYTES = 1024;
// Free heap the rest of the fetch needs behind those two reserves: expat's
// parser and buffers, the HTTP client's response headers, and the per-item
// title/url/guid/pubDate/filename strings. Deliberately a FLAT floor and not
// one scaled by the item cap: the cap is what bounds the string total (see
// CROSSPOINT_RSS_MAX_ITEMS in RssParser.h), and a cap-scaled floor would refuse
// a three-item feed on the 100-item combo envs for want of memory that a
// three-item feed never asks for.
constexpr size_t FETCH_MIN_FREE_HEAP = 24 * 1024;

bool startsWithNoCase(const std::string& value, const char* prefix) {
  return strncasecmp(value.c_str(), prefix, strlen(prefix)) == 0;
}

// Destination filename for an enclosure: last path segment, percent-decoded,
// sanitized for FAT32 by the same helper the rest of the firmware uses, with
// an .epub extension guaranteed.
std::string filenameFromUrl(const std::string& url) {
  std::string path = url.substr(0, url.find_first_of("?#"));
  const size_t slash = path.find_last_of('/');
  if (slash != std::string::npos) path.erase(0, slash + 1);
  path = FsHelpers::decodeUriEscapes(path);

  char sanitized[MAX_FILENAME_CHARS];
  FsHelpers::sanitizePathComponentForFat32(path.c_str(), sanitized, sizeof(sanitized));

  std::string name = sanitized;
  // A segment of only dots/dashes (or nothing at all) would name a directory
  // entry the card can't hold; fall back to a generic stem.
  if (name.find_first_not_of(".-") == std::string::npos) name = "feed";
  if (!FsHelpers::hasEpubExtension(name)) name += ".epub";
  return name;
}
}  // namespace

RssSyncActivity::RssSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("RssSync", renderer, mappedInput) {}

// --- Lifecycle ---

void RssSyncActivity::onEnter() {
  UiListActivity::onEnter();
  // Normalised HERE, not just in the settings editor: the web settings API and
  // a hand-edited settings.json write rssDestFolder verbatim, and this screen
  // deletes files under whatever it is handed.
  destFolder = normalizeDestFolder(SETTINGS.rssDestFolder, RSS_DEFAULT_FOLDER);
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void RssSyncActivity::onExit() {
  Activity::onExit();
  rows.clear();
  rowItems.clear();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void RssSyncActivity::shutdownWifi() {
  if (WiFi.getMode() == WIFI_MODE_NULL) return;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(30);
}

void RssSyncActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    // Leave the radio up; onExit's silent reboot handles teardown without
    // fragmenting (same reasoning as OpdsBookBrowserActivity).
    failWith(tr(STR_WIFI_CONN_FAILED));
    return;
  }
  startFetch();
}

void RssSyncActivity::failWith(std::string message) {
  {
    RenderLock lock(*this);
    state = State::ERROR;
    errorMessage = std::move(message);
  }
  requestUpdate();
}

// --- Fetch ---

void RssSyncActivity::startFetch() {
  {
    RenderLock lock(*this);
    state = State::FETCHING;
  }
  requestUpdateAndWait();

  if (!fetchFeed()) return;  // fetchFeed() set the error state

#ifdef CROSSPOINT_SOFT_CLOCK
  // The feed response's own Date header is the soft clock's primary time
  // source: it just arrived, it cost nothing, and it works on a LAN with no
  // route to an NTP server. NTP is the fallback for the server that sends no
  // usable Date — WiFi is up and the "Fetching..." screen is still on the
  // panel, the one moment in this flow where a few seconds cost the user
  // nothing, and the HAL backs it off so a routeless device does not pay twice.
  if (!halClock.applyServerDate(HttpDownloader::lastResponseDate())) halClock.maybeOpportunisticSync();
#endif

  {
    RenderLock lock(*this);
    rebuildRowItems();
    state = State::LIST;
    // Open on the first feed item, not on the Sync row: Confirm should toggle a
    // checkbox, never start a sync the user has not looked at yet.
    nav.reset(ACTION_ROW_COUNT);
  }
  requestUpdate();
}

bool RssSyncActivity::fetchFeed() {
  const std::string configured = SETTINGS.rssFeedUrl;
  if (configured.empty()) {
    failWith(tr(STR_RSS_NO_URL));
    return false;
  }
  // The feed lives on the user's own LAN server. TLS to a LAN host has no
  // verifiable certificate, so refuse https loudly instead of silently
  // downgrading or failing deep inside the client.
  if (startsWithNoCase(configured, "https://")) {
    failWith(tr(STR_RSS_HTTPS_UNSUPPORTED));
    return false;
  }
  // RssParser's constructor reserves the whole item vector in ONE block, up
  // front, whatever the feed turns out to hold — so the ask is known here.
  // reserve() aborts rather than fails; see RESERVE_HEAP_HEADROOM_BYTES.
  // Checked after the URL validation above so a misconfigured feed still gets
  // its own specific message.
  const size_t itemsBlock = RssParser::MAX_ITEMS * sizeof(RssItem) + RESERVE_HEAP_HEADROOM_BYTES;
  if (gateMaxAllocHeap() < itemsBlock || gateFreeHeap() < FETCH_MIN_FREE_HEAP) {
    LOG_ERR("RSS", "Low heap for a %u-item feed: %u free (need %u), %u max block (need %u)",
            (unsigned)RssParser::MAX_ITEMS, (unsigned)gateFreeHeap(), (unsigned)FETCH_MIN_FREE_HEAP,
            (unsigned)gateMaxAllocHeap(), (unsigned)itemsBlock);
    // "Not enough memory" — generic despite the DICT key name, and reused
    // deliberately: a new key would grow the string tables of all 30 languages
    // on every env for a message that should almost never be seen.
    failWith(tr(STR_DICT_LOW_MEMORY));
    return false;
  }

  const std::string url = UrlUtils::ensureProtocol(configured);
  LOG_DBG("RSS", "Fetching feed: %s", url.c_str());

  // Streamed straight into expat through the DataCallback overload: the feed
  // body is never buffered whole.
  RssParser parser;
  const bool transferred = HttpDownloader::fetchUrl(
      url, [&parser](const uint8_t* data, const size_t len) { return parser.feed(data, len); });
  if (!transferred || !parser.finish() || parser.error()) {
    // A partial parse is discarded wholesale: half a feed would silently ask
    // the mirror to delete every item the truncated document never mentioned.
    failWith(I18N.get(transferred ? StrId::STR_PARSE_FEED_FAILED : StrId::STR_FETCH_FEED_FAILED));
    return false;
  }
  if (parser.truncated()) LOG_INF("RSS", "Feed truncated to %u items", (unsigned)RssParser::MAX_ITEMS);

  auto items = parser.takeItems();
  if (items.empty()) {
    failWith(tr(STR_RSS_NO_ITEMS));
    return false;
  }
  return buildRows(std::move(items));
}

bool RssSyncActivity::buildRows(std::vector<RssItem>&& items) {
  // Second of the two aborting reserve() calls (see RESERVE_HEAP_HEADROOM_BYTES
  // above), gated against the count the feed actually delivered rather than the
  // cap. rowItems is NOT gated separately: rebuildRowItems() runs immediately
  // after this on the heap just checked here, its block is smaller than this
  // one, and clear() keeps the capacity so every later call is a no-op.
  const size_t rowsBlock = items.size() * sizeof(Row) + RESERVE_HEAP_HEADROOM_BYTES;
  if (gateMaxAllocHeap() < rowsBlock) {
    LOG_ERR("RSS", "Low heap for %u rows: %u max block (need %u)", (unsigned)items.size(), (unsigned)gateMaxAllocHeap(),
            (unsigned)rowsBlock);
    failWith(tr(STR_DICT_LOW_MEMORY));
    return false;
  }

  rows.clear();
  rows.reserve(items.size());

  for (auto& item : items) {
    Row row;
    // Moved, not copied: the parser's vector is released below and these are
    // the only two fields that outlive it.
    row.title = std::move(item.title);
    row.url = std::move(item.enclosureUrl);
    rssShortDate(item.pubDate, row.date, sizeof(row.date));
    // guid/pubDate have served their purpose (identity fallback + date label);
    // free them now rather than holding 100 of each until the vector dies.
    std::string().swap(item.guid);
    std::string().swap(item.pubDate);

    row.filename = filenameFromUrl(row.url);
    // Two items deriving the same leaf name cannot both be mirrored, and a
    // positional tie-break (a "-2" suffix) would be worse than refusing:
    // suffixes are assigned in feed order, so the same file changes identity
    // as soon as the feed rotates, and unticking a row would then delete a
    // DIFFERENT item's book. The server contract is one unique, stable leaf
    // filename per item, so a clash is a feed bug — refuse the whole sync and
    // name the offending pair rather than act on a guess.
    if (const Row* clash = rowWithFilename(row.filename)) {
      LOG_ERR("RSS", "Duplicate filename '%s': '%s' / '%s'", row.filename.c_str(), clash->title.c_str(),
              row.title.c_str());
      // The ERROR screen draws one unwrapped centred line, so the format caps
      // each title (titles are otherwise up to 120 bytes) at what fits the
      // panel; the full pair is in the log line above.
      char message[96];
      snprintf(message, sizeof(message), tr(STR_RSS_DUPLICATE_FILENAME_FORMAT), clash->title.c_str(),
               row.title.c_str());
      std::vector<Row>().swap(rows);  // nothing will use the partial list
      failWith(message);
      return false;
    }

    // Presence is resolved once, here, and read from the row thereafter.
    row.present = Storage.exists(pathFor(row).c_str());
    rows.push_back(std::move(row));
  }
  std::vector<RssItem>().swap(items);

  recountAndRelabel();
  return true;
}

std::string RssSyncActivity::pathFor(const Row& row) const { return destFolder + "/" + row.filename; }

// The earlier row in this feed that already claimed the leaf name, or nullptr.
const RssSyncActivity::Row* RssSyncActivity::rowWithFilename(const std::string& filename) const {
  for (const auto& row : rows) {
    if (row.filename == filename) return &row;
  }
  return nullptr;
}

// --- List state ---

int RssSyncActivity::listCount() const {
  if (state == State::SUMMARY) return 2;
  return ACTION_ROW_COUNT + static_cast<int>(rows.size());
}

// Recomputes the two consequence counts and the Sync row's label. Called
// wherever a checkbox changes, so the label always states what the button will
// actually do.
void RssSyncActivity::recountAndRelabel() {
  newCount = removeCount = 0;
  for (const auto& row : rows) {
    if (row.checked && !row.present) newCount++;
    if (!row.checked && row.present) removeCount++;
  }
  if (newCount == 0 && removeCount == 0) {
    syncLabel = tr(STR_RSS_UP_TO_DATE);
    return;
  }
  char buf[64];
  snprintf(buf, sizeof(buf), tr(STR_RSS_SYNC_ACTION_FORMAT), newCount, removeCount);
  syncLabel = buf;
}

void RssSyncActivity::rebuildRowItems() {
  rowItems.clear();
  rowItems.reserve(listCount());

  fui::ListItem action;
  action.label = tr(STR_RSS_SELECT_ALL);
  action.actionValue = ROW_SELECT_ALL;
  rowItems.push_back(action);
  action.label = tr(STR_RSS_UNSELECT_ALL);
  action.actionValue = ROW_UNSELECT_ALL;
  rowItems.push_back(action);
  action.label = syncLabel.c_str();
  action.actionValue = ROW_SYNC;
  rowItems.push_back(action);

  for (size_t i = 0; i < rows.size(); i++) {
    fui::ListItem item;
    item.label = rows[i].title.c_str();
    // The switch replaces the value slot on a toggle row, so the date rides
    // the subtitle line.
    if (rows[i].date[0]) item.subtitle = rows[i].date;
    item.toggle = true;
    item.toggleChecked = rows[i].checked;
    item.actionValue = static_cast<int16_t>(ACTION_ROW_COUNT + i);
    rowItems.push_back(item);
  }
}

void RssSyncActivity::activateIndex(const int index) {
  nav.selected = index;
  app.clearTapFlash();

  if (state == State::SUMMARY) {
    if (index == ROW_BROWSE) {
      shutdownWifi();
      activityManager.goToFileBrowser(destFolder);
    } else {
      onGoHome();
    }
    return;
  }

  if (state != State::LIST) return;
  // Outside the lock below: runSync() takes RenderLocks of its own, and this
  // one is not recursive.
  if (index == ROW_SYNC) {
    runSync();  // ends in SUMMARY, even for a no-op sync
    return;
  }

  {
    // syncLabel and rowItems are read by the render task mid-build (rowItems
    // hands list() a raw pointer), so every mutation of them happens with the
    // render lock held rather than racing a repaint.
    RenderLock lock(*this);
    if (index == ROW_SELECT_ALL || index == ROW_UNSELECT_ALL) {
      for (auto& row : rows) row.checked = index == ROW_SELECT_ALL;
    } else {
      const int itemIndex = index - ACTION_ROW_COUNT;
      if (itemIndex < 0 || itemIndex >= static_cast<int>(rows.size())) return;
      rows[itemIndex].checked = !rows[itemIndex].checked;
    }
    recountAndRelabel();
    rebuildRowItems();
  }
  requestUpdate();
}

void RssSyncActivity::onBackButton() {
  // Back leaves the feature entirely from either list; onExit tears the radio
  // down and reboots into home.
  onGoHome();
}

bool RssSyncActivity::handleCustomInput() {
  // LIST and SUMMARY are ordinary lists — let the base protocol (touch
  // routing, swipe scroll, button navigation, Back/Confirm) drive them.
  if (state == State::LIST || state == State::SUMMARY) return false;

  if (state == State::ERROR) {
    int tx = 0;
    int ty = 0;
    // A tap works too: this screen is a dead end, and a touch-only user should
    // not have to reach for the home gesture to leave it.
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(tx, ty)) {
      onGoHome();
    }
  }
  // WIFI_SELECTION: the child activity owns input. FETCHING/SYNCING block the
  // main loop; cancel is pumped from the download progress callback.
  return true;
}

// --- Sync ---

void RssSyncActivity::runSync() {
  {
    RenderLock lock(*this);
    state = State::SYNCING;
    downloadedCount = deletedCount = skippedCount = failedCount = remainingCount = 0;
    actionIndex = 0;
    actionTotal = newCount + removeCount;
    cancelRequested = false;
    goHomeRequested = false;
    cancelled = false;
    fileProgress = fileTotal = 0;
    activeTitle.clear();
  }
  requestUpdateAndWait();

  // One mkdir for the whole run. exists() first: mkdir's return-on-existing is
  // unconfirmed, and every other caller in the tree guards it the same way.
  if (newCount > 0 && !Storage.exists(destFolder.c_str()) && !Storage.mkdir(destFolder.c_str())) {
    LOG_ERR("RSS", "mkdir failed for %s", destFolder.c_str());
    failWith(tr(STR_ERROR_GENERAL_FAILURE));
    return;
  }

  for (auto& row : rows) {
    // Cancel is honoured BETWEEN items, so the item in flight always finishes
    // cleanly (a half-written epub is worse than a slower exit). Latched here
    // rather than off cancelRequested: a cancel during the LAST item leaves
    // nothing undone, and that run did complete.
    if (cancelRequested) {
      cancelled = true;
      break;
    }

    if (row.checked && row.present) {
      skippedCount++;
      continue;
    }
    if (!row.checked && !row.present) continue;  // nothing to do

    {
      RenderLock lock(*this);
      actionIndex++;
      activeTitle = row.title;
      fileProgress = fileTotal = 0;
    }
    requestUpdateAndWait();

    const bool ok = row.checked ? downloadRow(row) : deleteRow(row);
    if (!ok) {
      failedCount++;
      continue;
    }
    // Keep the row consistent with the card, so a later pass over the same
    // list (or the summary) never contradicts what happened.
    row.present = row.checked;
    row.checked ? downloadedCount++ : deletedCount++;
  }

  if (goHomeRequested) {
    onGoHome();
    return;
  }

  {
    RenderLock lock(*this);
    // Every row that needs work bumps actionIndex before it runs, so what is
    // left of actionTotal is exactly what a cancel never started.
    remainingCount = actionTotal - actionIndex;
    state = State::SUMMARY;
    nav.reset(ROW_BROWSE);
  }
  requestUpdate();
}

bool RssSyncActivity::downloadRow(const Row& row) {
  // Plain http is the server contract, and fetchFeed() already refuses an https
  // FEED url for it: a LAN host has no verifiable certificate. An https
  // enclosure is the same contract violation arriving one level down, so refuse
  // it here too rather than let it reach the client. It is also the one place
  // this feature can pull in the whole wolfSSL TLS 1.3 handshake, and it would
  // do so with the row list already resident — the worst moment for it on a C3.
  // Fails this ITEM (the caller counts it in failedCount), not the run.
  if (startsWithNoCase(row.url, "https://")) {
    LOG_ERR("RSS", "Refusing https enclosure: %s", row.url.c_str());
    return false;
  }

  const std::string tmpPath = destFolder + TMP_LEAF;
  const std::string destPath = pathFor(row);
  // Enclosure URLs come off the wire raw: a space or other unsafe character in
  // one makes esp_http_client reject the request outright. Same treatment every
  // other caller gives a URL it did not build itself.
  const std::string url = UrlUtils::encodeUnsafeUrlChars(row.url);

  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;
  const auto result = HttpDownloader::downloadToFile(
      url, tmpPath, [this, &lastRenderedPercent, &lastProgressUpdateMs](const size_t done, const size_t total) {
        fileProgress = done;
        fileTotal = total;
        // The activity loop is blocked for the whole transfer; pump input here
        // so Back can ask to stop after this item.
        mappedInput.update();
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelRequested = true;
        // This update() consumes the one-shot home event before the central
        // dispatch can see it, so honour it here.
        if (mappedInput.wasHomeGesture()) {
          cancelRequested = true;
          goHomeRequested = true;
        }
        // total == 0 is a response without Content-Length: there is no percent
        // to step, so the repaint falls back to the time-based branch below and
        // render() draws the indeterminate (bytes received) presentation.
        const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(done) * 100 / total) : 0;
        const unsigned long now = millis();
        if (percent >= 100 || lastRenderedPercent < 0 || percent >= lastRenderedPercent + PROGRESS_STEP_PERCENT ||
            now - lastProgressUpdateMs >= PROGRESS_MIN_UPDATE_MS) {
          lastRenderedPercent = percent;
          lastProgressUpdateMs = now;
          requestUpdate(true);
        }
      });

  if (result != HttpDownloader::OK) {
    LOG_ERR("RSS", "Download failed (%d): %s", static_cast<int>(result), row.url.c_str());
    Storage.remove(tmpPath.c_str());  // belt and braces; downloadToFile already unlinks
    return false;
  }
  // Atomic swap into place. present == false got us here, but a file appearing
  // underneath us would make rename() fail, so clear the way first.
  if (Storage.exists(destPath.c_str())) Storage.remove(destPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), destPath.c_str())) {
    LOG_ERR("RSS", "Rename failed: %s", destPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  // A reused filename may still have a stale reading cache behind it.
  clearBookCache(destPath);
  return true;
}

bool RssSyncActivity::deleteRow(const Row& row) {
  const std::string path = pathFor(row);
  // Same helper the file browser's delete uses, so progress files, thumbs and
  // the /.crosspoint cache directory go with the book.
  clearBookCache(path);
  if (Storage.remove(path.c_str())) return true;
  LOG_ERR("RSS", "Delete failed: %s", path.c_str());
  return false;
}

// --- Rendering ---

void RssSyncActivity::buildListScreen(UiScreen& screen) {
  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}

void RssSyncActivity::buildSummaryScreen(UiScreen& screen) {
  const auto& theme = screen.theme();
  fui::TextStyle centered = theme.bodyText;
  centered.align = fui::TextAlign::Center;
  const int16_t lh = screen.target().lineHeight(centered.font);

  // Only the counters that actually happened get a line. "Remaining" is the
  // work a cancel skipped, so it is zero (and hidden) on a full run.
  const StrId formats[5] = {StrId::STR_RSS_DOWNLOADED_FORMAT, StrId::STR_RSS_DELETED_FORMAT,
                            StrId::STR_RSS_SKIPPED_FORMAT, StrId::STR_RSS_FAILED_FORMAT,
                            StrId::STR_RSS_REMAINING_FORMAT};
  const int counts[5] = {downloadedCount, deletedCount, skippedCount, failedCount, remainingCount};
  char line[48];
  for (int i = 0; i < 5; i++) {
    if (counts[i] == 0) continue;
    snprintf(line, sizeof(line), I18N.get(formats[i]), counts[i]);
    screen.target().text(screen.takeTop(lh, theme.spaceSm), line, centered);
  }
  screen.spacer(theme.spaceMd);

  summaryRowItems[ROW_BROWSE].label = tr(STR_RSS_BROWSE_FOLDER);
  summaryRowItems[ROW_BROWSE].actionValue = ROW_BROWSE;
  summaryRowItems[ROW_DONE].label = tr(STR_DONE);
  summaryRowItems[ROW_DONE].actionValue = ROW_DONE;

  fui::ListProps props;
  props.items = summaryRowItems;
  props.count = 2;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

void RssSyncActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  state == State::SUMMARY ? buildSummaryScreen(screen) : buildListScreen(screen);
}

// SUMMARY must not claim a run finished when the user stopped it.
const char* RssSyncActivity::headerTitle() const {
  if (state != State::SUMMARY) return I18N.get(StrId::STR_RSS_SYNC);
  return I18N.get(cancelled ? StrId::STR_RSS_SUMMARY_CANCELLED : StrId::STR_RSS_SUMMARY);
}

void RssSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  drawChrome();

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto centerY = (pageHeight - lineHeight) / 2;
  MappedInputManager::Labels labels;

  switch (state) {
    case State::LIST:
    case State::SUMMARY:
      renderUi();
      // Mirrors UiListActivity::render's rebuild loop (UiListActivity.cpp:160):
      // list() reports the real layout back to ListNav, and a selection past
      // the drawn rows advances the viewport and asks for another build.
      // Without this pass, scrolling over a page boundary paints the stale
      // viewport — it reads as a dropped keypress. Bounded: top strictly
      // advances toward the selection each pass.
      for (int pass = 0; activeNav().consumeRebuildNeeded() && pass < 8; ++pass) {
        renderer.clearScreen();
        drawChrome();
        renderUi();
      }
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      break;
    case State::SYNCING: {
      char header[48];
      snprintf(header, sizeof(header), tr(STR_RSS_ITEM_PROGRESS_FORMAT), actionIndex, actionTotal);
      renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight * 2, header);
      renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, activeTitle.c_str());
      if (fileTotal > 0) {
        GUI.drawProgressBar(renderer,
                            Rect{metrics.contentSidePadding, centerY + metrics.verticalSpacing,
                                 pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
                            fileProgress, fileTotal);
      } else if (fileProgress > 0) {
        // A response without Content-Length has no percentage to show, and
        // drawProgressBar draws nothing at all for total == 0 — a bar frozen
        // empty for the whole transfer. Report the bytes that have arrived
        // instead. fileProgress stays 0 for a delete, which draws neither.
        char received[32];
        snprintf(received, sizeof(received), tr(STR_RSS_RECEIVED_KB_FORMAT), static_cast<int>(fileProgress / 1024));
        renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, received);
      }
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    }
    case State::ERROR:
      renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_ERROR_MSG), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, errorMessage.c_str());
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
    case State::FETCHING:
    case State::WIFI_SELECTION:
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_RSS_FETCHING));
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
  }

  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

#endif  // CROSSPOINT_RSS_SYNC
