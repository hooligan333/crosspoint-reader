#include "FlashcardSyncActivity.h"

#ifdef CROSSPOINT_FLASHCARDS

#include <Arduino.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
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
#include "flashcards/DeckPaths.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/DeckGuid.h"
#include "util/DestFolder.h"
#include "util/UrlUtils.h"

namespace fui = freeink::ui;

namespace {
// Downloads land here first and are renamed into place only once complete and
// header-checked, so an interrupted or wrong-format transfer never leaves a
// half file that the deck picker would offer.
constexpr const char* TMP_LEAF = "/.tmp-deck";
// Scheduling state lives beside the decks, one file per deck leaf
// (FLASHCARD_SPEC.md §3). Created by the study session, deleted with the deck;
// both spellings of the path come from flashcards::statePathFor so this screen
// and the study session cannot drift apart (util/../flashcards/DeckPaths.h).
constexpr const char* DECK_EXTENSION = flashcards::DECK_EXTENSION;
constexpr size_t MAX_FILENAME_CHARS = 80;
// Same repaint throttle RssSyncActivity uses for its download bar: e-ink
// refreshes cost more than the progress they show.
constexpr int PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long PROGRESS_MIN_UPDATE_MS = 5000;

// CPDK v1 header, from DECK_SERVER_SPEC.md §3.1. Only the fields this screen
// checks are named; the rest of the 64 bytes is the reader's business.
constexpr size_t CPDK_HEADER_BYTES = 64;
constexpr size_t CPDK_PARAMS_BYTES = 84;  // f32 w[21], present iff flags bit 0
constexpr size_t CPDK_INDEX_ENTRY_BYTES = 20;
constexpr size_t CPDK_CONTENT_HASH_OFFSET = 12;
constexpr uint16_t CPDK_FORMAT_VERSION = 1;
constexpr uint16_t CPDK_FLAG_FSRS_PARAMS = 0x0001;
constexpr uint32_t CPDK_MAX_CARDS = 40000;

// Every CPDK multi-byte field is little-endian on the wire and is memcpy'd
// straight into a local below, so the bytes only land in the right order on a
// little-endian host. That is true of every ESP32 target (and of the host the
// unit tests run on), but it is an assumption worth failing the build over
// rather than discovering as a garbled hash (FLASHCARD_SPEC.md §2.1).
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "CPDK fields are little-endian; this host is not");

bool startsWithNoCase(const std::string& value, const char* prefix) {
  return strncasecmp(value.c_str(), prefix, strlen(prefix)) == 0;
}

// FAT is case-insensitive, so two leaf names that differ only in case name the
// SAME file on the card. Every leaf comparison in this screen goes through
// here: the delete-on-untick identity guarantee depends on "Biology.deck" and
// "biology.deck" being recognised as one deck, not two.
bool leafEquals(const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) == 0; }

// Destination filename for an enclosure: last path segment, percent-decoded,
// sanitized for FAT32 by the same helper the rest of the firmware uses, with a
// .deck extension guaranteed. The leaf IS the deck's identity — presence
// detection, delete-on-untick and the state file all key on it.
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
  if (name.find_first_not_of(".-") == std::string::npos) name = "deck";
  if (!FsHelpers::checkFileExtension(name, DECK_EXTENSION)) name += DECK_EXTENSION;
  return name;
}

/**
 * Cheap CPDK v1 header check on a freshly downloaded file.
 *
 * Deliberately NOT a deck validation: it reads the 64-byte header and stops.
 * Its job is to keep a file that this build cannot possibly read — wrong
 * magic, a future format_version, an unknown flags bit, an impossible card
 * count, or a body that is shorter than its own index — from being renamed
 * into the deck folder, where it would read as "present" and be skipped by
 * every later sync. The index/blob/FSRS-parameter validation belongs to the
 * deck reader (FLASHCARD_SPEC.md §2.1).
 */
bool deckHeaderIsReadable(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("DECK", path, file)) return false;

  const size_t fileSize = file.fileSize();
  if (fileSize < CPDK_HEADER_BYTES) {
    LOG_ERR("DECK", "Short file (%u B): %s", static_cast<unsigned>(fileSize), path.c_str());
    return false;
  }
  uint8_t header[CPDK_HEADER_BYTES];
  if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    LOG_ERR("DECK", "Header read failed: %s", path.c_str());
    return false;
  }

  if (memcmp(header, "CPDK", 4) != 0) {
    LOG_ERR("DECK", "Not a CPDK file: %s", path.c_str());
    return false;
  }
  // Nothing in CPDK is naturally aligned (DECK_SERVER_SPEC.md §3.6.1), so every
  // field is memcpy'd into a local instead of being read through a cast pointer.
  uint16_t formatVersion = 0;
  uint16_t flags = 0;
  uint32_t cardCount = 0;
  memcpy(&formatVersion, header + 4, sizeof(formatVersion));
  memcpy(&flags, header + 6, sizeof(flags));
  memcpy(&cardCount, header + 8, sizeof(cardCount));

  if (formatVersion != CPDK_FORMAT_VERSION) {
    LOG_ERR("DECK", "Unsupported format version %u: %s", static_cast<unsigned>(formatVersion), path.c_str());
    return false;
  }
  // Unknown flags bits are the format's version escape hatch: refuse rather
  // than guess what the block after the header means.
  if ((flags & static_cast<uint16_t>(~CPDK_FLAG_FSRS_PARAMS)) != 0) {
    LOG_ERR("DECK", "Unknown header flags 0x%04x: %s", static_cast<unsigned>(flags), path.c_str());
    return false;
  }
  // Zero is refused with the over-cap case: an empty deck has nothing to study,
  // and letting one land would leave a file the picker offers and the reader
  // cannot open (FLASHCARD_SPEC.md §2.1 lists it among the sync-layer refusals).
  if (cardCount == 0 || cardCount > CPDK_MAX_CARDS) {
    LOG_ERR("DECK", "Card count %u out of range: %s", static_cast<unsigned>(cardCount), path.c_str());
    return false;
  }
  // A truncated download is the realistic failure mode (DECK_SERVER_SPEC.md
  // §3.6.4); the header alone says how long the file must be before its first
  // byte of text. cardCount is capped above, so this cannot overflow.
  const size_t blobStart = CPDK_HEADER_BYTES + ((flags & CPDK_FLAG_FSRS_PARAMS) != 0 ? CPDK_PARAMS_BYTES : 0) +
                           static_cast<size_t>(cardCount) * CPDK_INDEX_ENTRY_BYTES;
  if (fileSize < blobStart) {
    LOG_ERR("DECK", "Truncated: %u B < index end %u: %s", static_cast<unsigned>(fileSize),
            static_cast<unsigned>(blobStart), path.c_str());
    return false;
  }
  return true;
}

/**
 * The local half of the staleness comparison (FLASHCARD_SPEC.md §1): the file's
 * size and the content_hash out of its CPDK header.
 *
 * Deliberately as cheap as it looks — one open and one 64-byte read per checked
 * row that is already present, and nothing at all during the feed parse. A file
 * that will not open, is shorter than a header, or does not say "CPDK" comes
 * back not-readable, which deckIsStale() treats as stale: re-downloading is the
 * repair for exactly that state.
 */
LocalDeckStamp readLocalDeckStamp(const std::string& path) {
  LocalDeckStamp stamp;
  HalFile file;
  if (!Storage.openFileForRead("DECK", path, file)) {
    LOG_DBG("DECK", "Stale check: cannot open %s", path.c_str());
    return stamp;
  }
  const size_t fileSize = file.fileSize();
  if (fileSize < CPDK_HEADER_BYTES) return stamp;

  uint8_t header[CPDK_HEADER_BYTES];
  if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) return stamp;
  if (memcmp(header, "CPDK", 4) != 0) return stamp;

  // Unaligned by construction (offset 12), so memcpy into a local rather than
  // dereferencing a cast pointer; see the endianness static_assert above.
  memcpy(&stamp.contentHash, header + CPDK_CONTENT_HASH_OFFSET, sizeof(stamp.contentHash));
  stamp.sizeBytes = static_cast<uint32_t>(fileSize);
  stamp.readable = true;
  return stamp;
}
}  // namespace

FlashcardSyncActivity::FlashcardSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("FlashcardSync", renderer, mappedInput) {}

// --- Lifecycle ---

void FlashcardSyncActivity::onEnter() {
  UiListActivity::onEnter();
  // Normalised HERE, not just in the settings editor: the web settings API and
  // a hand-edited settings.json write flashcardDestFolder verbatim, and this
  // screen deletes files under whatever it is handed.
  destFolder = normalizeDestFolder(SETTINGS.flashcardDestFolder, DECK_DEFAULT_FOLDER);
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void FlashcardSyncActivity::onExit() {
  Activity::onExit();
  rows.clear();
  rowItems.clear();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void FlashcardSyncActivity::shutdownWifi() {
  if (WiFi.getMode() == WIFI_MODE_NULL) return;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(30);
}

void FlashcardSyncActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    // Leave the radio up; onExit's silent reboot handles teardown without
    // fragmenting (same reasoning as RssSyncActivity).
    failWith(tr(STR_WIFI_CONN_FAILED));
    return;
  }
  startFetch();
}

void FlashcardSyncActivity::failWith(std::string message) {
  {
    RenderLock lock(*this);
    state = State::ERROR;
    errorMessage = std::move(message);
  }
  requestUpdate();
}

// --- Fetch ---

void FlashcardSyncActivity::startFetch() {
  {
    RenderLock lock(*this);
    state = State::FETCHING;
  }
  requestUpdateAndWait();

  if (!fetchFeed()) return;  // fetchFeed() set the error state

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

bool FlashcardSyncActivity::fetchFeed() {
  const std::string configured = SETTINGS.flashcardFeedUrl;
  if (configured.empty()) {
    failWith(tr(STR_DECK_NO_URL));
    return false;
  }
  // This guard covers the FEED URL only — the one URL the user types. TLS to a
  // LAN host has no verifiable certificate, so refuse https here loudly instead
  // of silently downgrading or failing deep inside the client. The enclosure
  // URLs the feed then hands back are followed under the same LAN-trust model
  // the RSS mirror uses; they are not re-checked.
  if (startsWithNoCase(configured, "https://")) {
    failWith(tr(STR_RSS_HTTPS_UNSUPPORTED));
    return false;
  }
  const std::string url = UrlUtils::ensureProtocol(configured);
  LOG_DBG("DECK", "Fetching feed: %s", url.c_str());

  // Same document grammar as the ePUB feed, so the same streaming parser: the
  // body goes straight into expat and is never buffered whole.
  RssParser parser;
  const bool transferred = HttpDownloader::fetchUrl(
      url, [&parser](const uint8_t* data, const size_t len) { return parser.feed(data, len); });
  if (!transferred || !parser.finish() || parser.error()) {
    // A partial parse is discarded wholesale: half a feed would silently ask
    // the mirror to delete every deck the truncated document never mentioned.
    failWith(I18N.get(transferred ? StrId::STR_PARSE_FEED_FAILED : StrId::STR_FETCH_FEED_FAILED));
    return false;
  }
  if (parser.truncated()) LOG_INF("DECK", "Feed truncated to %u items", (unsigned)RssParser::MAX_ITEMS);

  auto items = parser.takeItems();
  if (items.empty()) {
    failWith(tr(STR_DECK_NO_ITEMS));
    return false;
  }
  return buildRows(std::move(items));
}

bool FlashcardSyncActivity::buildRows(std::vector<RssItem>&& items) {
  rows.clear();
  rows.reserve(items.size());

  for (auto& item : items) {
    Row row;
    // Moved, not copied: the parser's vector is released below and these are
    // the only fields that outlive it.
    row.title = std::move(item.title);
    row.url = std::move(item.enclosureUrl);
    row.feed.sizeBytes = item.enclosureLength;
    rssShortDate(item.pubDate, row.date, sizeof(row.date));
    // The guid carries the served file's content hash as an "@<16 hex>" suffix
    // (FLASHCARD_SPEC.md §1). Parsing is pure string work — the comparison
    // against the local header happens in the sync pass, not here, so a 100-item
    // feed costs no reads at all until the user presses Sync.
    row.feed.hasContentHash = parseDeckGuidHash(item.guid, row.feed.contentHash);
    // guid/pubDate have served their purpose (content hash + date label); free
    // them now rather than holding 100 of each until the vector dies.
    std::string().swap(item.guid);
    std::string().swap(item.pubDate);

    row.filename = filenameFromUrl(row.url);
    // Two items deriving the same leaf name cannot both be mirrored, and a
    // positional tie-break (a "-2" suffix) would be worse than refusing:
    // suffixes are assigned in feed order, so the same file changes identity as
    // soon as the feed rotates, and unticking a row would then delete a
    // DIFFERENT deck — along with the scheduling state of every card in it. The
    // server contract is one unique, stable leaf filename per item
    // (DECK_SERVER_SPEC.md §2), so a clash is a feed bug: refuse the whole sync
    // and name the offending pair rather than act on a guess.
    if (const Row* clash = rowWithFilename(row.filename)) {
      LOG_ERR("DECK", "Duplicate filename '%s': '%s' / '%s'", row.filename.c_str(), clash->title.c_str(),
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

std::string FlashcardSyncActivity::pathFor(const Row& row) const { return destFolder + "/" + row.filename; }

// The deck's scheduling state, which follows the deck file's own leaf name.
std::string FlashcardSyncActivity::statePathFor(const Row& row) const {
  return flashcards::statePathFor(destFolder, row.filename);
}

// The earlier row in this feed that already claimed the leaf name, or nullptr.
// Case-INSENSITIVE (see leafEquals): the destination is FAT, so "Biology.deck"
// and "biology.deck" are one file, and treating them as two rows would break
// the delete-on-untick identity guarantee this refusal exists to protect.
const FlashcardSyncActivity::Row* FlashcardSyncActivity::rowWithFilename(const std::string& filename) const {
  for (const auto& row : rows) {
    if (leafEquals(row.filename, filename)) return &row;
  }
  return nullptr;
}

// --- List state ---

int FlashcardSyncActivity::listCount() const {
  if (state == State::SUMMARY) return 2;
  return ACTION_ROW_COUNT + static_cast<int>(rows.size());
}

// Recomputes the two consequence counts and the Sync row's label. Called
// wherever a checkbox changes, so the label always states what the button will
// actually do.
void FlashcardSyncActivity::recountAndRelabel() {
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

void FlashcardSyncActivity::rebuildRowItems() {
  rowItems.clear();
  // Sized from what this function actually pushes, not from listCount(), which
  // reports SUMMARY's two rows once the run ends.
  rowItems.reserve(ACTION_ROW_COUNT + rows.size());

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
    // The switch replaces the value slot on a toggle row, so the date rides the
    // subtitle line.
    if (rows[i].date[0]) item.subtitle = rows[i].date;
    item.toggle = true;
    item.toggleChecked = rows[i].checked;
    item.actionValue = static_cast<int16_t>(ACTION_ROW_COUNT + i);
    rowItems.push_back(item);
  }
}

void FlashcardSyncActivity::activateIndex(const int index) {
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

void FlashcardSyncActivity::onBackButton() {
  // Back leaves the feature entirely from either list; onExit tears the radio
  // down and reboots into home.
  onGoHome();
}

bool FlashcardSyncActivity::handleCustomInput() {
  // LIST and SUMMARY are ordinary lists — let the base protocol (touch routing,
  // swipe scroll, button navigation, Back/Confirm) drive them.
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

void FlashcardSyncActivity::runSync() {
  {
    RenderLock lock(*this);
    state = State::SYNCING;
    downloadedCount = updatedCount = deletedCount = skippedCount = failedCount = invalidCount = remainingCount = 0;
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
    LOG_ERR("DECK", "mkdir failed for %s", destFolder.c_str());
    failWith(tr(STR_ERROR_GENERAL_FAILURE));
    return;
  }

  for (auto& row : rows) {
    // Cancel is honoured BETWEEN items, so the item in flight always finishes
    // cleanly (a half-written deck is worse than a slower exit). Latched here
    // rather than off cancelRequested: a cancel during the LAST item leaves
    // nothing undone, and that run did complete.
    if (cancelRequested) {
      cancelled = true;
      break;
    }

    // A checked row whose file is already on the card is the interesting case:
    // decks are MUTABLE, so "present" does not mean "current". One 64-byte
    // header read decides it (FLASHCARD_SPEC.md §1). Done here, in the sync
    // pass, rather than while the list is built: the user may never press Sync,
    // and 100 opens to draw a list would be felt.
    bool replacing = false;
    if (row.checked && row.present) {
      if (!deckIsStale(row.feed, readLocalDeckStamp(pathFor(row)))) {
        skippedCount++;
        continue;
      }
      replacing = true;
    }
    if (!row.checked && !row.present) continue;  // nothing to do

    {
      RenderLock lock(*this);
      // A stale row is work the pre-sync counts could not know about (they were
      // computed without touching the card), so widen the denominator as it is
      // discovered — "item x of y" and the cancelled remainder both read off it.
      if (replacing) actionTotal++;
      actionIndex++;
      activeTitle = row.title;
      fileProgress = fileTotal = 0;
    }
    requestUpdateAndWait();

    if (row.checked) {
      // A failed or rejected replacement leaves the existing deck untouched:
      // the transfer lands in the temp file and the header check runs before
      // the rename, so the user keeps a studiable deck either way.
      switch (downloadRow(row)) {
        case ItemResult::Ok:
          // Keep the row consistent with the card, so a later pass over the
          // same list (or the summary) never contradicts what happened.
          row.present = true;
          if (replacing) {
            updatedCount++;
          } else {
            downloadedCount++;
          }
          break;
        case ItemResult::Invalid:
          invalidCount++;
          break;
        case ItemResult::Failed:
          failedCount++;
          break;
      }
      continue;
    }

    if (!deleteRow(row)) {
      failedCount++;
      continue;
    }
    row.present = false;
    deletedCount++;
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

FlashcardSyncActivity::ItemResult FlashcardSyncActivity::downloadRow(const Row& row) {
  // Plain http is the server contract (DECK_SERVER_SPEC.md), and fetchFeed()
  // already refuses an https FEED url for it: a LAN host has no verifiable
  // certificate. An https enclosure is the same contract violation arriving one
  // level down, so refuse it here too rather than let it reach the client. It is
  // also the one place this feature can pull in the whole wolfSSL TLS 1.3
  // handshake, and it would do so with the row list already resident. Fails this
  // ITEM (the caller counts it in failedCount), not the run.
  if (startsWithNoCase(row.url, "https://")) {
    LOG_ERR("DECK", "Refusing https enclosure: %s", row.url.c_str());
    return ItemResult::Failed;
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
      url, tmpPath, [this, &row, &lastRenderedPercent, &lastProgressUpdateMs](const size_t done, const size_t total) {
        fileProgress = done;
        // A response without Content-Length still has a length the user can see:
        // the feed's <enclosure length> is contractually the real byte size
        // (DECK_SERVER_SPEC.md §2), so the bar keeps working where the RSS
        // screen falls back to a bytes-received line. But it is a CLAIM, not a
        // measurement: once more bytes than that have arrived it is provably
        // wrong, so drop back to the byte counter rather than drive a bar past
        // its own end (GUI.drawProgressBar does not clamp) and repaint the
        // whole panel on every kilobyte of the overrun.
        fileTotal = total > 0 ? total : (done <= row.feed.sizeBytes ? row.feed.sizeBytes : 0);
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
        int percent = fileTotal > 0 ? static_cast<int>(static_cast<uint64_t>(done) * 100 / fileTotal) : 0;
        // A server that sends more than it announced must not paint 137%.
        if (percent > 100) percent = 100;
        const unsigned long now = millis();
        // The 100% case forces one repaint so the bar visibly fills, but only
        // the FIRST time: every later callback at 100% is the same picture.
        if ((percent >= 100 && lastRenderedPercent < 100) || lastRenderedPercent < 0 ||
            percent >= lastRenderedPercent + PROGRESS_STEP_PERCENT ||
            now - lastProgressUpdateMs >= PROGRESS_MIN_UPDATE_MS) {
          lastRenderedPercent = percent;
          lastProgressUpdateMs = now;
          requestUpdate(true);
        }
      });

  if (result != HttpDownloader::OK) {
    LOG_ERR("DECK", "Download failed (%d): %s", static_cast<int>(result), row.url.c_str());
    Storage.remove(tmpPath.c_str());  // belt and braces; downloadToFile already unlinks
    return ItemResult::Failed;
  }
  // Checked BEFORE the rename, so a file this build cannot read never reaches
  // the deck folder: there it would read as "present" and be skipped by every
  // later sync instead of being retried.
  if (!deckHeaderIsReadable(tmpPath)) {
    Storage.remove(tmpPath.c_str());
    return ItemResult::Invalid;
  }
  // Swap into place. There may well be a file here already — a stale deck being
  // replaced is the whole point of the staleness check — and rename() over an
  // existing name is not guaranteed, so clear the way first. Everything that
  // could reject this download has already run, so the window between the two
  // is as short as it can be made.
  if (Storage.exists(destPath.c_str())) Storage.remove(destPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), destPath.c_str())) {
    LOG_ERR("DECK", "Rename failed: %s", destPath.c_str());
    Storage.remove(tmpPath.c_str());
    return ItemResult::Failed;
  }
  // Scheduling state is deliberately left alone, and that is load-bearing for a
  // stale replacement: the state file records the deck's content hash, and the
  // study session merges it against the new one on open (FLASHCARD_SPEC.md §3).
  // Unticking a deck remains the only thing that deletes state.
  return ItemResult::Ok;
}

bool FlashcardSyncActivity::deleteRow(const Row& row) {
  const std::string path = pathFor(row);
  // No clearBookCache here: a deck is not a book, so it has no progress file,
  // no thumbnail and no /.crosspoint cache entry. What it does own is its
  // scheduling state, which is worthless without the deck and would otherwise
  // be silently adopted by a later deck that happened to reuse the leaf name.
  //
  // Deck FIRST, state second. The other order loses the schedule for a deck
  // that is still there: if the deck remove fails after the state is gone, the
  // user is left with a full deck whose every card has been reset to new, and
  // nothing to undo it with.
  if (!Storage.remove(path.c_str())) {
    LOG_ERR("DECK", "Delete failed: %s", path.c_str());
    return false;
  }
  const std::string statePath = statePathFor(row);
  if (Storage.exists(statePath.c_str()) && !Storage.remove(statePath.c_str())) {
    // Not fatal: the deck itself is what the user asked to remove, and a stale
    // state file is rejected on content-hash mismatch at deck open.
    LOG_ERR("DECK", "State delete failed: %s", statePath.c_str());
  }
  // The merge temp counts as state and must go with it: StateStore::open()
  // ADOPTS an orphaned `.tmp` (a merge that was interrupted between remove and
  // rename left the only complete copy there, FLASHCARD_SPEC.md §3), so one
  // left behind here would be picked up by whatever deck next takes this leaf
  // name — the very thing the state delete above exists to prevent.
  const std::string stateTemp = statePath + ".tmp";
  if (Storage.exists(stateTemp.c_str()) && !Storage.remove(stateTemp.c_str())) {
    LOG_ERR("DECK", "State temp delete failed: %s", stateTemp.c_str());
  }
  return true;
}

// --- Rendering ---

void FlashcardSyncActivity::buildListScreen(UiScreen& screen) {
  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}

void FlashcardSyncActivity::buildSummaryScreen(UiScreen& screen) {
  const auto& theme = screen.theme();
  fui::TextStyle centered = theme.bodyText;
  centered.align = fui::TextAlign::Center;
  const int16_t lh = screen.target().lineHeight(centered.font);

  // Only the counters that actually happened get a line. "Remaining" is the
  // work a cancel skipped, so it is zero (and hidden) on a full run.
  // "Updated" is its own line rather than part of "Downloaded": a replaced deck
  // kept its scheduling state, and the user is entitled to see the difference.
  constexpr int COUNTER_COUNT = 7;
  const StrId formats[COUNTER_COUNT] = {StrId::STR_RSS_DOWNLOADED_FORMAT, StrId::STR_DECK_UPDATED_FORMAT,
                                        StrId::STR_RSS_DELETED_FORMAT,    StrId::STR_RSS_SKIPPED_FORMAT,
                                        StrId::STR_RSS_FAILED_FORMAT,     StrId::STR_DECK_INVALID_FORMAT,
                                        StrId::STR_RSS_REMAINING_FORMAT};
  const int counts[COUNTER_COUNT] = {downloadedCount, updatedCount, deletedCount,  skippedCount,
                                     failedCount,     invalidCount, remainingCount};
  char line[48];
  for (int i = 0; i < COUNTER_COUNT; i++) {
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

void FlashcardSyncActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  state == State::SUMMARY ? buildSummaryScreen(screen) : buildListScreen(screen);
}

// SUMMARY must not claim a run finished when the user stopped it.
const char* FlashcardSyncActivity::headerTitle() const {
  if (state != State::SUMMARY) return I18N.get(StrId::STR_DECK_SYNC);
  return I18N.get(cancelled ? StrId::STR_RSS_SUMMARY_CANCELLED : StrId::STR_RSS_SUMMARY);
}

void FlashcardSyncActivity::render(RenderLock&&) {
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
        // Neither a Content-Length nor an <enclosure length>: there is no
        // percentage to draw, so report the bytes that have arrived instead.
        // fileProgress stays 0 for a delete, which draws neither.
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

#endif  // CROSSPOINT_FLASHCARDS
