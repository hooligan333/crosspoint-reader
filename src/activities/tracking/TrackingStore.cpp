#include "TrackingStore.h"

#ifdef CROSSPOINT_TRACKING

#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>
#include <ctime>

#include "RtcClock.h"

namespace tracking {

namespace {

constexpr char LOG_DIR[] = "/tracking";
// Holds a UTF-8 BOM, the header and a CRLF with room to spare.
constexpr size_t HEADER_PROBE_BYTES = 64;
// What may precede a row in the same write: the header line, or the newline
// a hand-edited file lacks (never both). sizeof counts the header's '\n'.
constexpr size_t ROW_PREFIX_BYTES = sizeof(CSV_HEADER);

struct LogState {
  bool needHeader = true;    // file absent, empty, or just rotated away
  bool needNewline = false;  // file does not end in '\n' (hand-edited on a PC)
};

// One append's buffers, heap-held for the call (~560 B is too much stack).
// The row is formatted at `text + ROW_PREFIX_BYTES` so any prefix can be
// copied in front of it and both go out in one write.
struct AppendScratch {
  char timestamp[TIMESTAMP_BYTES];
  char text[ROW_PREFIX_BYTES + ROW_BYTES];
};

bool pathTaken(const char* path, void*) { return Storage.exists(path); }

// Moves the log to the first free rotation slot. Never deletes or overwrites:
// with every slot taken it refuses, and the append fails.
AppendResult rotateLog() {
  char target[ROTATED_LOG_PATH_BYTES];
  if (findFreeRotatedLog(pathTaken, nullptr, target, sizeof(target)) == 0) {
    LOG_ERR("TRK", "No free rotation slot for %s; not rotating", LOG_PATH);
    return AppendResult::NoRotationSlot;
  }
  if (!Storage.rename(LOG_PATH, target)) {
    LOG_ERR("TRK", "Cannot rotate %s with unexpected header to %s", LOG_PATH, target);
    return AppendResult::NotWritten;
  }
  LOG_INF("TRK", "Moved %s with unexpected header to %s", LOG_PATH, target);
  return AppendResult::Ok;
}

// Rotates a file with a foreign first line out of the way, so new rows never
// land under someone else's columns. Not Ok if the log cannot be read (it is
// then left alone) or the rotation failed.
AppendResult inspectLog(LogState& state) {
  HalFile file = Storage.open(LOG_PATH, O_RDONLY);
  if (!file) {
    if (!Storage.exists(LOG_PATH)) return AppendResult::Ok;
    LOG_ERR("TRK", "Cannot open existing %s", LOG_PATH);
    return AppendResult::NotWritten;
  }
  const size_t size = file.fileSize();
  char head[HEADER_PROBE_BYTES];
  const int got = size == 0 ? 0 : file.read(head, sizeof(head));
  const LogHead kind = classifyLogHead(head, got, sizeof(head), size);
  bool tailOk = true;
  if (kind == LogHead::Ours) {
    char last = '\n';
    tailOk = file.seek(size - 1) && file.read(&last, 1) == 1;
    state.needNewline = last != '\n';
    state.needHeader = false;
  }
  file.close();

  switch (kind) {
    case LogHead::Empty:
      return AppendResult::Ok;
    case LogHead::Ours:
      if (!tailOk) LOG_ERR("TRK", "Cannot read the end of %s", LOG_PATH);
      return tailOk ? AppendResult::Ok : AppendResult::NotWritten;
    case LogHead::Unreadable:
      LOG_ERR("TRK", "Cannot read the start of %s; leaving it alone", LOG_PATH);
      return AppendResult::NotWritten;
    case LogHead::Foreign:
      break;
  }
  return rotateLog();
}

}  // namespace

LoadStatus loadConfig(Config& config, ParseError& error, uint32_t& errorLine) {
  config.clear();
  error = ParseError::None;
  errorLine = 0;
  if (!Storage.exists(CONFIG_PATH)) return LoadStatus::Missing;

  HalFile file;
  if (!Storage.openFileForRead("TRK", CONFIG_PATH, file)) return LoadStatus::ReadFailed;
  if (file.fileSize() > MAX_CONFIG_BYTES) {
    LOG_ERR("TRK", "%s is %u bytes; the limit is %u", CONFIG_PATH, static_cast<unsigned>(file.fileSize()),
            static_cast<unsigned>(MAX_CONFIG_BYTES));
    file.close();
    error = ParseError::TooLarge;
    return LoadStatus::ParseFailed;
  }
  // Parser lifetime is this call; its 2 KB token buffer stays off the stack.
  auto parser = makeUniqueNoThrow<ConfigParser>(config);
  if (!parser) {
    LOG_ERR("TRK", "OOM: config parser (%u bytes)", static_cast<unsigned>(sizeof(ConfigParser)));
    file.close();
    return LoadStatus::OutOfMemory;
  }
  char chunk[256];
  while (!parser->failed()) {
    const int got = file.read(chunk, sizeof(chunk));
    if (got < 0) {
      LOG_ERR("TRK", "Read failed: %s", CONFIG_PATH);
      file.close();
      return LoadStatus::ReadFailed;
    }
    if (got == 0) break;
    parser->feed(chunk, static_cast<size_t>(got));
  }
  file.close();

  error = parser->finish();
  errorLine = parser->errorLine();
  if (error != ParseError::None) {
    LOG_ERR("TRK", "Config parse failed (%u) at line %u", static_cast<unsigned>(error),
            static_cast<unsigned>(errorLine));
    return LoadStatus::ParseFailed;
  }
  return LoadStatus::Ok;
}

// The seconds can be up to HalClock::CLOCK_POLL_MS stale (its poll cache);
// fine for a habit log.
bool localNow(DateTime& out) {
  struct tm local{};
  if (!halClock.localTime(local)) return false;
  // The RTC backend answers whenever the chip exists; one that lost power
  // reads its reset date (~2000), which must count as unset.
  if (local.tm_year + 1900 < rtcclock::EARLIEST_PLAUSIBLE_YEAR) return false;
  out.year = static_cast<uint16_t>(local.tm_year + 1900);
  out.month = static_cast<uint8_t>(local.tm_mon + 1);
  out.day = static_cast<uint8_t>(local.tm_mday);
  out.hour = static_cast<uint8_t>(local.tm_hour);
  out.minute = static_cast<uint8_t>(local.tm_min);
  out.second = static_cast<uint8_t>(local.tm_sec);
  return true;
}

AppendResult appendEntry(const Item& item, const char* value, const DateTime* overrideTimestamp) {
  auto scratch = makeUniqueNoThrow<AppendScratch>();
  if (!scratch) {
    LOG_ERR("TRK", "OOM: append buffers (%u bytes)", static_cast<unsigned>(sizeof(AppendScratch)));
    return AppendResult::NotWritten;
  }
  DateTime now{};
  if (overrideTimestamp) {
    now = *overrideTimestamp;
    clampDateTime(now);  // the timestamp column must stay a valid date
  } else if (!localNow(now)) {
    // The entry is still logged: the user's input is never dropped, and this
    // stamp is easy to find and fix on the PC.
    LOG_ERR("TRK", "Clock unset; logging with the epoch timestamp");
    now = DateTime{1970, 1, 1, 0, 0, 0};
  }
  formatTimestamp(now, scratch->timestamp, sizeof(scratch->timestamp));

  char* const row = scratch->text + ROW_PREFIX_BYTES;
  if (!formatCsvRow(row, ROW_BYTES, scratch->timestamp, item.key(), typeName(item.type), value)) {
    LOG_ERR("TRK", "Row too long for %s", item.key());
    return AppendResult::NotWritten;
  }

  if (!Storage.exists(LOG_DIR) && !Storage.mkdir(LOG_DIR)) {
    LOG_ERR("TRK", "Cannot create %s", LOG_DIR);
    return AppendResult::NotWritten;
  }
  LogState state;
  const AppendResult inspected = inspectLog(state);
  if (inspected != AppendResult::Ok) return inspected;

  char* start = row;
  if (state.needHeader) {
    start -= ROW_PREFIX_BYTES;
    memcpy(start, CSV_HEADER, ROW_PREFIX_BYTES - 1);
    start[ROW_PREFIX_BYTES - 1] = '\n';
  } else if (state.needNewline) {
    *--start = '\n';
  }
  const size_t len = static_cast<size_t>(row - start) + strlen(row);

  HalFile file = Storage.open(LOG_PATH, O_WRITE | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("TRK", "Cannot open %s for append", LOG_PATH);
    return AppendResult::NotWritten;
  }
  const size_t before = file.fileSize();
  AppendResult result = AppendResult::Ok;
  if (file.write(start, len) != len) {
    // Cut a partial row back off, so the next append starts on a clean line.
    LOG_ERR("TRK", "Short write to %s", LOG_PATH);
    if (file.truncate(before)) {
      result = AppendResult::NotWritten;
    } else {
      LOG_ERR("TRK", "Cannot truncate %s back to %u bytes", LOG_PATH, static_cast<unsigned>(before));
      result = AppendResult::MaybeWritten;
    }
  }
  file.flush();
  if (!file.close()) {
    // Whatever was written (or cut back) may or may not have reached the card.
    LOG_ERR("TRK", "Close failed: %s", LOG_PATH);
    result = AppendResult::MaybeWritten;
  }
  return result;
}

}  // namespace tracking

#endif  // CROSSPOINT_TRACKING
