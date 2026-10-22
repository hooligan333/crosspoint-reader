#pragma once

// Tracking (CROSSPOINT_TRACKING): CSV row formatting, number validation and
// date-field arithmetic for /tracking/log.csv. Pure functions over caller
// buffers; see TrackingConfig.h for why the .cpp is flag-wrapped.

#include <cstddef>
#include <cstdint>

#include "TrackingConfig.h"

namespace tracking {

// First line of log.csv, without its newline.
constexpr char CSV_HEADER[] = "timestamp,label,type,value";

// Keyboard caps: text matches the dictionary word editor; numbers leave room
// for any sane decimal.
constexpr size_t MAX_TEXT_BYTES = 128;
constexpr size_t MAX_NUMBER_INPUT_BYTES = 32;
// Canonical form can gain one byte (".5" -> "0.5").
constexpr size_t NUMBER_CANONICAL_BYTES = MAX_NUMBER_INPUT_BYTES + 2;
constexpr size_t TIMESTAMP_BYTES = sizeof("YYYY-MM-DDTHH:MM:SS");
constexpr size_t VALUE_DATE_BYTES = sizeof("YYYY-MM-DDTHH:MM");
// Quoting at most doubles a field plus two quotes, so the worst row is 418
// bytes: timestamp 19, a 63-byte label of quotes 128, type 8, a 128-byte text
// of quotes 258, three commas, newline, NUL.
constexpr size_t ROW_BYTES = 512;

// The date/datetime pickers' year range, and the year they open on when the
// clock is unset.
constexpr uint16_t YEAR_MIN = 2000;
constexpr uint16_t YEAR_MAX = 2099;
constexpr uint16_t FALLBACK_YEAR = 2026;

struct DateTime {
  uint16_t year;
  uint8_t month;  // 1..12
  uint8_t day;    // 1..daysInMonth
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
};

bool isLeapYear(uint16_t year);
/** 28..31; 0 for a month outside 1..12. */
uint8_t daysInMonth(uint16_t year, uint8_t month);
/** Pulls every field into range: year into YEAR_MIN..YEAR_MAX, day into its month. */
void clampDateTime(DateTime& dt);

void formatTimestamp(const DateTime& dt, char* out, size_t outBytes);  // YYYY-MM-DDTHH:MM:SS
void formatDate(const DateTime& dt, char* out, size_t outBytes);       // YYYY-MM-DD
void formatDateTime(const DateTime& dt, char* out, size_t outBytes);   // YYYY-MM-DDTHH:MM

enum class NumberCheck : uint8_t { Ok, Empty, NotANumber, NotInteger, BelowMin, AboveMax };

/**
 * Validates keyboard input for a number item and writes its canonical text:
 * optional sign, digits, optional fraction, no exponent; leading zeros, a
 * trailing ".0..." and the sign of zero are dropped ("+007.50" -> "7.5").
 * Done on the text, not a double, so the logged value is exactly what was
 * typed. `canonical` needs NUMBER_CANONICAL_BYTES.
 */
NumberCheck checkNumber(const char* input, const Item& item, char* canonical, size_t canonicalBytes);

/** True when the field must be wrapped in quotes (comma, quote, CR or LF). */
bool csvNeedsQuoting(const char* field);

/**
 * Appends one RFC 4180 field at out[used]; returns the new length, or 0 if it
 * does not fit (out is left NUL-terminated at `used`).
 */
size_t appendCsvField(char* out, size_t outBytes, size_t used, const char* field);

/** One log row with its trailing "\n"; false if it does not fit. */
bool formatCsvRow(char* out, size_t outBytes, const char* timestamp, const char* key, const char* type,
                  const char* value);

/**
 * True when `data` (the start of a file) begins with CSV_HEADER as a whole
 * line. A UTF-8 BOM and a CRLF ending, both of which a spreadsheet re-save can
 * add, are accepted. With `wholeFile`, `data` is the entire file and a header
 * that ends the file without a line end also counts.
 */
bool startsWithCsvHeader(const char* data, size_t len, bool wholeFile = false);

enum class LogHead : uint8_t { Empty, Ours, Foreign, Unreadable };

/**
 * Classifies an existing log.csv from its first bytes. `got` is the read's
 * result for a request of `requested` bytes on a `fileSize`-byte file. A
 * failed or short read is Unreadable, never Foreign: only content that was
 * actually read and does not match may cause a rotation.
 */
LogHead classifyLogHead(const char* data, int got, size_t requested, size_t fileSize);

// A log with a foreign header moves to slot 1 (log.old.csv), or to the first
// free slot 2..MAX_ROTATED_LOGS (log.old-N.csv). Rotated files are never
// deleted or overwritten.
constexpr unsigned MAX_ROTATED_LOGS = 99;
constexpr size_t ROTATED_LOG_PATH_BYTES = sizeof("/tracking/log.old-99.csv");

/** Writes slot `index`'s path; false when `index` is outside 1..MAX_ROTATED_LOGS or `out` is too small. */
bool rotatedLogPath(unsigned index, char* out, size_t outBytes);

/**
 * Writes the first slot whose path `taken` reports free and returns its index;
 * 0 when every slot is taken (the caller must then not rotate).
 */
unsigned findFreeRotatedLog(bool (*taken)(const char* path, void* ctx), void* ctx, char* out, size_t outBytes);

const char* booleanValue(bool value);

}  // namespace tracking
