#include "TrackingFormat.h"

#ifdef CROSSPOINT_TRACKING

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tracking {

namespace {

bool isDigit(const char c) { return c >= '0' && c <= '9'; }

uint8_t clampU8(const uint8_t v, const uint8_t lo, const uint8_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Exactly "YYYY-MM-DDTHH:MM:SS", the only stamp this firmware writes.
bool isLogTimestamp(const char* ts) {
  static constexpr char SHAPE[] = "DDDD-DD-DDTDD:DD:DD";
  for (size_t i = 0; i + 1 < sizeof(SHAPE); i++) {
    if (SHAPE[i] == 'D' ? !isDigit(ts[i]) : ts[i] != SHAPE[i]) return false;
  }
  return ts[sizeof(SHAPE) - 1] == '\0';
}

}  // namespace

bool isLeapYear(const uint16_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

uint8_t daysInMonth(const uint16_t year, const uint8_t month) {
  static constexpr uint8_t DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && isLeapYear(year)) return 29;
  return DAYS[month - 1];
}

void clampDateTime(DateTime& dt) {
  dt.year = dt.year < YEAR_MIN ? YEAR_MIN : (dt.year > YEAR_MAX ? YEAR_MAX : dt.year);
  dt.month = clampU8(dt.month, 1, 12);
  dt.day = clampU8(dt.day, 1, daysInMonth(dt.year, dt.month));
  dt.hour = clampU8(dt.hour, 0, 23);
  dt.minute = clampU8(dt.minute, 0, 59);
  dt.second = clampU8(dt.second, 0, 59);
}

void formatTimestamp(const DateTime& dt, char* out, const size_t outBytes) {
  snprintf(out, outBytes, "%04u-%02u-%02uT%02u:%02u:%02u", static_cast<unsigned>(dt.year),
           static_cast<unsigned>(dt.month), static_cast<unsigned>(dt.day), static_cast<unsigned>(dt.hour),
           static_cast<unsigned>(dt.minute), static_cast<unsigned>(dt.second));
}

void formatDate(const DateTime& dt, char* out, const size_t outBytes) {
  snprintf(out, outBytes, "%04u-%02u-%02u", static_cast<unsigned>(dt.year), static_cast<unsigned>(dt.month),
           static_cast<unsigned>(dt.day));
}

void formatDateTime(const DateTime& dt, char* out, const size_t outBytes) {
  snprintf(out, outBytes, "%04u-%02u-%02uT%02u:%02u", static_cast<unsigned>(dt.year), static_cast<unsigned>(dt.month),
           static_cast<unsigned>(dt.day), static_cast<unsigned>(dt.hour), static_cast<unsigned>(dt.minute));
}

NumberCheck checkNumber(const char* input, const Item& item, char* canonical, const size_t canonicalBytes) {
  if (canonicalBytes == 0) return NumberCheck::NotANumber;
  canonical[0] = '\0';
  if (input == nullptr) return NumberCheck::Empty;

  const char* begin = input;
  const char* end = input + strlen(input);
  while (begin < end && (*begin == ' ' || *begin == '\t')) begin++;
  while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) end--;
  if (begin == end) return NumberCheck::Empty;
  if (static_cast<size_t>(end - begin) > MAX_NUMBER_INPUT_BYTES) return NumberCheck::NotANumber;

  const char* p = begin;
  bool negative = false;
  if (*p == '+' || *p == '-') {
    negative = *p == '-';
    p++;
  }
  const char* intBegin = p;
  while (p < end && isDigit(*p)) p++;
  const char* intEnd = p;
  const char* fracBegin = p;
  const char* fracEnd = p;
  if (p < end && *p == '.') {
    p++;
    fracBegin = p;
    while (p < end && isDigit(*p)) p++;
    fracEnd = p;
  }
  if (p != end || (intBegin == intEnd && fracBegin == fracEnd)) return NumberCheck::NotANumber;

  while (intBegin < intEnd && *intBegin == '0') intBegin++;
  while (fracEnd > fracBegin && fracEnd[-1] == '0') fracEnd--;
  const size_t intLen = static_cast<size_t>(intEnd - intBegin);
  const size_t fracLen = static_cast<size_t>(fracEnd - fracBegin);
  const bool zero = intLen == 0 && fracLen == 0;

  const size_t needed = (negative && !zero ? 1 : 0) + (intLen == 0 ? 1 : intLen) + (fracLen > 0 ? 1 + fracLen : 0) + 1;
  if (needed > canonicalBytes) return NumberCheck::NotANumber;
  size_t n = 0;
  if (negative && !zero) canonical[n++] = '-';
  if (intLen == 0) {
    canonical[n++] = '0';
  } else {
    memcpy(canonical + n, intBegin, intLen);
    n += intLen;
  }
  if (fracLen > 0) {
    canonical[n++] = '.';
    memcpy(canonical + n, fracBegin, fracLen);
    n += fracLen;
  }
  canonical[n] = '\0';

  if (item.integerOnly && fracLen > 0) return NumberCheck::NotInteger;
  const double value = strtod(canonical, nullptr);
  if (item.hasMin && value < item.minValue) return NumberCheck::BelowMin;
  if (item.hasMax && value > item.maxValue) return NumberCheck::AboveMax;
  return NumberCheck::Ok;
}

bool csvNeedsQuoting(const char* field) { return strpbrk(field, ",\"\r\n") != nullptr; }

size_t appendCsvField(char* out, const size_t outBytes, const size_t used, const char* field) {
  if (used >= outBytes) return 0;
  const bool quote = csvNeedsQuoting(field);
  size_t n = used;
  const auto put = [&](const char c) {
    if (n + 1 >= outBytes) return false;  // keep room for the terminator
    out[n++] = c;
    return true;
  };
  bool ok = !quote || put('"');
  for (const char* p = field; ok && *p != '\0'; p++) {
    if (*p == '"') ok = put('"');
    if (ok) ok = put(*p);
  }
  if (ok && quote) ok = put('"');
  if (!ok) {
    out[used] = '\0';
    return 0;
  }
  out[n] = '\0';
  return n;
}

bool formatCsvRow(char* out, const size_t outBytes, const char* timestamp, const char* key, const char* type,
                  const char* value) {
  if (outBytes == 0) return false;
  out[0] = '\0';
  const char* fields[4] = {timestamp, key, type, value};
  size_t used = 0;
  for (int i = 0; i < 4; i++) {
    if (i > 0) {
      if (used + 2 > outBytes) return false;
      out[used++] = ',';
      out[used] = '\0';
    }
    // An empty field appends nothing and legitimately leaves `used` unchanged.
    if (fields[i][0] == '\0') continue;
    const size_t next = appendCsvField(out, outBytes, used, fields[i]);
    if (next == 0) return false;
    used = next;
  }
  if (used + 2 > outBytes) return false;
  out[used++] = '\n';
  out[used] = '\0';
  return true;
}

bool startsWithCsvHeader(const char* data, size_t len, const bool wholeFile) {
  if (len >= 3 && static_cast<unsigned char>(data[0]) == 0xEF && static_cast<unsigned char>(data[1]) == 0xBB &&
      static_cast<unsigned char>(data[2]) == 0xBF) {
    data += 3;
    len -= 3;
  }
  const size_t headerLen = sizeof(CSV_HEADER) - 1;
  if (len < headerLen || memcmp(data, CSV_HEADER, headerLen) != 0) return false;
  if (len == headerLen) return wholeFile;
  if (data[headerLen] == '\n') return true;
  if (data[headerLen] != '\r') return false;
  if (len == headerLen + 1) return wholeFile;
  return data[headerLen + 1] == '\n';
}

LogHead classifyLogHead(const char* data, const int got, const size_t requested, const size_t fileSize) {
  if (fileSize == 0) return LogHead::Empty;
  const size_t expected = fileSize < requested ? fileSize : requested;
  if (got < 0 || static_cast<size_t>(got) != expected) return LogHead::Unreadable;
  return startsWithCsvHeader(data, expected, expected == fileSize) ? LogHead::Ours : LogHead::Foreign;
}

bool rotatedLogPath(const unsigned index, char* out, const size_t outBytes) {
  if (index < 1 || index > MAX_ROTATED_LOGS || outBytes == 0) return false;
  const int n = index == 1 ? snprintf(out, outBytes, "/tracking/log.old.csv")
                           : snprintf(out, outBytes, "/tracking/log.old-%u.csv", index);
  return n > 0 && static_cast<size_t>(n) < outBytes;
}

unsigned findFreeRotatedLog(bool (*taken)(const char* path, void* ctx), void* ctx, char* out, const size_t outBytes) {
  for (unsigned i = 1; i <= MAX_ROTATED_LOGS; i++) {
    if (!rotatedLogPath(i, out, outBytes)) return 0;
    if (!taken(out, ctx)) return i;
  }
  out[0] = '\0';
  return 0;
}

const char* booleanValue(const bool value) { return value ? "true" : "false"; }

bool parseCsvRow(const char* line, const size_t len, CsvRow& out) {
  char* const fields[4] = {out.timestamp, out.key, out.type, out.value};
  const size_t caps[4] = {sizeof(out.timestamp), sizeof(out.key), sizeof(out.type), sizeof(out.value)};
  for (char* field : fields) field[0] = '\0';

  size_t p = 0;
  for (int f = 0; f < 4; f++) {
    char* const dst = fields[f];
    size_t n = 0;
    const auto put = [&](const char c) {
      if (n + 1 >= caps[f]) return false;
      dst[n++] = c;
      return true;
    };
    if (p < len && line[p] == '"') {
      p++;
      bool closed = false;
      while (p < len) {
        const char c = line[p++];
        if (c == '"') {
          if (p < len && line[p] == '"') {
            p++;
          } else {
            closed = true;
            break;
          }
        }
        if (!put(c)) return false;
      }
      if (!closed) return false;
    } else {
      while (p < len && line[p] != ',') {
        if (line[p] == '"' || !put(line[p])) return false;
        p++;
      }
    }
    dst[n] = '\0';
    // Each field ends at a comma, or the last one at the line's end.
    if (f < 3) {
      if (p >= len || line[p] != ',') return false;
      p++;
    } else if (p != len) {
      return false;
    }
  }
  return true;
}

LastEntryScanner::LastEntryScanner(const char* key) : key(key) {}

bool LastEntryScanner::feedBackward(const char* data, const size_t len) {
  for (size_t i = len; i > 0 && !entry.found; i--) {
    const char c = data[i - 1];
    if (c == '\n') {
      endLine();
    } else if (overlong) {
      // Dropping the rest of a line that cannot be ours.
    } else if (start == 0) {
      overlong = true;
    } else {
      line[--start] = c;
    }
  }
  return entry.found;
}

void LastEntryScanner::finishAtFileStart() {
  if (!entry.found) endLine();
}

void LastEntryScanner::endLine() {
  const char* text = line + start;
  size_t len = ROW_BYTES - start;
  const bool skip = overlong;
  start = ROW_BYTES;
  overlong = false;
  if (skip) return;
  if (len > 0 && text[len - 1] == '\r') len--;
  if (len == 0) return;
  // A header has a "label" key column; never let it pass for a row.
  if (startsWithCsvHeader(text, len, true)) return;
  if (!parseCsvRow(text, len, row) || strcmp(row.key, key) != 0 || !isLogTimestamp(row.timestamp)) return;
  memcpy(entry.timestamp, row.timestamp, sizeof(entry.timestamp));
  memcpy(entry.value, row.value, sizeof(entry.value));
  entry.found = true;
}

bool scanLogTail(const size_t fileSize, const size_t capBytes, const LogReadFn read, void* const ctx,
                 LastEntryScanner& scanner, char* const chunk, const size_t chunkBytes) {
  if (chunkBytes == 0) return false;  // the loop below could not make progress
  const size_t scanStart = fileSize > capBytes ? fileSize - capBytes : 0;
  size_t pos = fileSize;
  while (pos > scanStart && !scanner.found()) {
    size_t start = pos - (pos - scanStart < chunkBytes ? pos - scanStart : chunkBytes);
    if (start > scanStart) {
      // The first read takes the unaligned remainder, so later ones start aligned.
      const size_t up = (start + LOG_READ_ALIGN - 1) / LOG_READ_ALIGN * LOG_READ_ALIGN;
      if (up < pos) start = up;
    }
    const size_t n = pos - start;
    if (!read(ctx, start, chunk, n)) return false;
    pos = start;
    scanner.feedBackward(chunk, n);
  }
  if (pos == 0) scanner.finishAtFileStart();
  return true;
}

}  // namespace tracking

#endif  // CROSSPOINT_TRACKING
