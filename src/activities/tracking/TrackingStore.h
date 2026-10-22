#pragma once
#ifdef CROSSPOINT_TRACKING

#include <TrackingConfig.h>
#include <TrackingFormat.h>

#include <cstdint>

/**
 * SD card and clock side of Tracking: reads /tracking/config.json into a
 * tracking::Config and appends confirmed entries to /tracking/log.csv. The
 * parsing and CSV rules themselves live in lib/Tracking (host-tested).
 */
namespace tracking {

constexpr char CONFIG_PATH[] = "/tracking/config.json";
constexpr char LOG_PATH[] = "/tracking/log.csv";

enum class LoadStatus : uint8_t { Ok, Missing, ReadFailed, OutOfMemory, ParseFailed };

/** Re-reads the config from the card. On ParseFailed, `error`/`errorLine` say why. */
LoadStatus loadConfig(Config& config, ParseError& error, uint32_t& errorLine);

/**
 * Local wall time per the clock's timezone (the status-bar clock's path);
 * false if the clock is unset or reads a year before any plausible setting.
 */
bool localNow(DateTime& out);

enum class AppendResult : uint8_t {
  Ok,
  NotWritten,      // failed before any byte reached the log, or a short write was cut back off
  MaybeWritten,    // the row may be on the card: close failed, or a short write could not be cut back
  NoRotationSlot,  // a foreign log needs moving but every log.old*.csv slot is taken; nothing written
};

/**
 * Appends one row (timestamp, item key, type, value) and closes the file.
 * Writes the header first when the file is new, and moves a file whose first
 * line is not the expected header to a free log.old*.csv slot. Anything but Ok
 * is an SD failure, including an unreadable existing log, which is never
 * rotated.
 */
AppendResult appendEntry(const Item& item, const char* value);

}  // namespace tracking

#endif  // CROSSPOINT_TRACKING
