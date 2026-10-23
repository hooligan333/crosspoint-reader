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

// Longest typeName() ("datetime") plus its NUL.
constexpr size_t CSV_TYPE_BYTES = sizeof("datetime");

/** One log row's columns. Each buffer fits the longest value this firmware writes there. */
struct CsvRow {
  char timestamp[TIMESTAMP_BYTES];
  char key[MAX_LABEL_BYTES + 1];  // a label, or an id (MAX_ID_BYTES is shorter)
  char type[CSV_TYPE_BYTES];
  char value[MAX_TEXT_BYTES + 1];
};

/**
 * Splits one line (no line end) into exactly four RFC 4180 fields, unquoting
 * quoted ones. False for any other field count, an unterminated quote, text
 * after a closing quote, a quote inside an unquoted field, or a field longer
 * than its CsvRow buffer: such a row was not written by this firmware.
 */
bool parseCsvRow(const char* line, size_t len, CsvRow& out);

struct LastEntry {
  char timestamp[TIMESTAMP_BYTES];
  char value[MAX_TEXT_BYTES + 1];  // empty for a none item
  bool found;
};

/**
 * Finds the newest log row whose key column equals `key`, fed the log from
 * its end: each feedBackward() chunk is the bytes directly before the
 * previous one. Lines are assembled across chunk boundaries and tested
 * newest-first; scanning stops at the first match.
 *
 * Skipped, without ending the scan: blank lines, header lines, lines longer
 * than ROW_BYTES, lines parseCsvRow() rejects, and rows whose timestamp is
 * not exactly YYYY-MM-DDTHH:MM:SS (digits at the digit positions). A CR
 * before the LF is dropped, and a last line without a line end still counts.
 *
 * Accepted limit: a quoted field holding a line break (only a PC edit can
 * make one; the device keyboard cannot) splits into separate lines here. The
 * pieces normally fail to parse or lack a timestamp and are skipped; a piece
 * that is itself a well-formed, timestamped row is read as one.
 *
 * `key` must outlive the scanner. ~0.9 KB: device callers heap-allocate it.
 */
class LastEntryScanner {
 public:
  explicit LastEntryScanner(const char* key);

  /** Returns true once a match is found; later calls are no-ops. */
  bool feedBackward(const char* data, size_t len);
  /**
   * Call once the file's first byte has been fed: tests the pending first
   * line. Skip it when the scan stopped short of the file start, since the
   * pending bytes are then only the tail of a line.
   */
  void finishAtFileStart();

  bool found() const { return entry.found; }
  const LastEntry& result() const { return entry; }

 private:
  void endLine();

  const char* key;
  // The line being assembled, filled from the back: bytes [start, ROW_BYTES).
  char line[ROW_BYTES];
  size_t start = ROW_BYTES;
  bool overlong = false;
  CsvRow row{};
  LastEntry entry{};
};

// scanLogTail() starts each read on a multiple of this (the SD sector size),
// except one that begins at the cap boundary.
constexpr size_t LOG_READ_ALIGN = 512;

/** Reads exactly `len` bytes at `pos` into `buf`; false on any failure or short read. */
using LogReadFn = bool (*)(void* ctx, size_t pos, char* buf, size_t len);

/**
 * Feeds `scanner` the last `capBytes` of a `fileSize`-byte log, newest bytes
 * first, through `read` into the caller's `chunk` (at most `chunkBytes`, > 0,
 * per read; a multiple of LOG_READ_ALIGN keeps every read aligned). Stops at
 * the first match. The scanner tests the file's first line only when the scan
 * reached byte 0: past the cap, the pending bytes may be the tail of a cut
 * line, so a row whose preceding line end lies outside the cap is not found.
 * False when a read fails; the scanner's result is then not used.
 */
bool scanLogTail(size_t fileSize, size_t capBytes, LogReadFn read, void* ctx, LastEntryScanner& scanner, char* chunk,
                 size_t chunkBytes);

}  // namespace tracking
