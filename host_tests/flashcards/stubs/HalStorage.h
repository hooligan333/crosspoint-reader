#pragma once
// Host stand-in for lib/hal's HalStorage / HalFile, backed by stdio.
//
// The flashcard logic layer reaches the card only through these two types, so
// this file IS the seam that makes DeckFile / StateStore / SessionQueue host
// testable. It mirrors the real API's shapes and, where behaviour matters, its
// semantics:
//   * openFileForWrite is O_RDWR | O_CREAT | O_TRUNC (SDCardManager), so it
//     truncates and the handle stays readable;
//   * open(path, O_RDWR) does NOT create — a missing file yields a false handle;
//   * rename REFUSES an existing destination, exactly as SdFat does. Code that
//     forgets the remove-then-rename dance fails here rather than on the device.
//
// FAULT INJECTION AND OP COUNTING (FLASHCARD_SPEC.md §7b.4). The C3 variant's
// defining change is that reading a card key became I/O, so its error paths are
// only real if they can be EXECUTED — and the cost of that I/O is only a claim
// until somebody counts it. Both live here, on the one seam every file access
// goes through:
//
//   * hostIoFaults() schedules a failure per operation kind on files whose path
//     ends in a chosen suffix: a short read (what a bad sector actually gives),
//     a refused seek, a short write. `allow*` is a countdown, so a test can let
//     a file open and validate and fail only the read after that.
//   * hostIoCounters() counts seeks/reads/writes, split by the three file kinds
//     this layer touches (.deck, .state, .state.tmp), which is what the merge's
//     read-ahead numbers are measured with.
//
// Both are process-global and are reset by the suite between groups; neither
// exists in the firmware, where HalFile is SdFat.

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

/** Per-operation failure scheduling for files whose path ends in `pathSuffix`. */
struct HostIoFaults {
  // Empty disables injection entirely, which is the state every test that does
  // not ask for a fault sees.
  std::string pathSuffix;
  // Operations of that kind allowed on a matching file before failures start.
  // -1 never fails; 0 fails the very next one. Once failing, stays failing.
  int allowSeeks = -1;
  int allowReads = -1;
  int allowWrites = -1;
  // A seek that REPORTS success and lands somewhere else, which is the fault a
  // key echo exists for: the read that follows comes back full-length, from the
  // wrong records. `allowSeeksBeforeSkew` counts matching seeks let through
  // first (-1 = never skew), `seekSkewBytes` is how far off the rest land.
  int allowSeeksBeforeSkew = -1;
  long seekSkewBytes = 0;
  // Observed, so a test can assert the fault it asked for actually happened
  // rather than passing because nothing ever reached the file.
  int seekFailures = 0;
  int readFailures = 0;
  int writeFailures = 0;
  int seekSkews = 0;

  void reset() {
    pathSuffix.clear();
    allowSeeks = allowReads = allowWrites = allowSeeksBeforeSkew = -1;
    seekSkewBytes = 0;
    seekFailures = readFailures = writeFailures = seekSkews = 0;
  }
  bool matches(const std::string& path) const {
    if (pathSuffix.empty()) return false;
    return path.size() >= pathSuffix.size() &&
           path.compare(path.size() - pathSuffix.size(), pathSuffix.size(), pathSuffix) == 0;
  }
};

inline HostIoFaults& hostIoFaults() {
  static HostIoFaults faults;
  return faults;
}

/** Op counts, split by file kind: what the merge read-ahead is measured with. */
struct HostIoCounters {
  long seeks = 0;
  long reads = 0;
  long writes = 0;
  long deckSeeks = 0;
  long deckReads = 0;
  long stateSeeks = 0;
  long stateReads = 0;
  long tempSeeks = 0;
  long tempWrites = 0;

  void zero() { *this = HostIoCounters(); }
};

inline HostIoCounters& hostIoCounters() {
  static HostIoCounters counters;
  return counters;
}

class HalFile {
 public:
  HalFile() = default;
  explicit HalFile(FILE* handle) : fp(handle) {}
  HalFile(FILE* handle, std::string filePath) : fp(handle), path(std::move(filePath)) {}
  HalFile(HalFile&&) = default;
  HalFile& operator=(HalFile&&) = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  size_t fileSize() {
    if (!fp) return 0;
    const long here = ftell(fp.get());
    fseek(fp.get(), 0, SEEK_END);
    const long end = ftell(fp.get());
    fseek(fp.get(), here, SEEK_SET);
    return end < 0 ? 0 : static_cast<size_t>(end);
  }
  bool seek(size_t pos) {
    HostIoCounters& counters = hostIoCounters();
    counters.seeks++;
    if (endsWith(".deck")) counters.deckSeeks++;
    if (endsWith(".state")) counters.stateSeeks++;
    if (endsWith(".tmp")) counters.tempSeeks++;
    HostIoFaults& faults = hostIoFaults();
    long target = static_cast<long>(pos);
    if (faults.matches(path)) {
      if (faults.allowSeeks >= 0) {
        if (faults.allowSeeks == 0) {
          faults.seekFailures++;
          return false;
        }
        faults.allowSeeks--;
      }
      if (faults.allowSeeksBeforeSkew >= 0) {
        if (faults.allowSeeksBeforeSkew == 0) {
          faults.seekSkews++;
          target += faults.seekSkewBytes;
          if (target < 0) target = 0;
        } else {
          faults.allowSeeksBeforeSkew--;
        }
      }
    }
    return fp && fseek(fp.get(), target, SEEK_SET) == 0;
  }
  int read(void* buf, size_t count) {
    HostIoCounters& counters = hostIoCounters();
    counters.reads++;
    if (endsWith(".deck")) counters.deckReads++;
    if (endsWith(".state")) counters.stateReads++;
    HostIoFaults& faults = hostIoFaults();
    if (faults.matches(path) && faults.allowReads >= 0) {
      if (faults.allowReads == 0) {
        faults.readFailures++;
        return 0;  // a short read, which is what a bad sector gives
      }
      faults.allowReads--;
    }
    if (!fp) return -1;
    return static_cast<int>(fread(buf, 1, count, fp.get()));
  }
  size_t write(const void* buf, size_t count) {
    HostIoCounters& counters = hostIoCounters();
    counters.writes++;
    if (endsWith(".tmp")) counters.tempWrites++;
    HostIoFaults& faults = hostIoFaults();
    if (faults.matches(path) && faults.allowWrites >= 0) {
      if (faults.allowWrites == 0) {
        faults.writeFailures++;
        return 0;  // a short write
      }
      faults.allowWrites--;
    }
    if (!fp) return 0;
    return fwrite(buf, 1, count, fp.get());
  }
  void flush() {
    if (fp) fflush(fp.get());
  }
  bool isOpen() const { return fp != nullptr; }
  explicit operator bool() const { return fp != nullptr; }

 private:
  bool endsWith(const char* suffix) const {
    const size_t length = strlen(suffix);
    return path.size() >= length && path.compare(path.size() - length, length, suffix) == 0;
  }

  struct Closer {
    void operator()(FILE* handle) const {
      if (handle != nullptr) fclose(handle);
    }
  };
  std::unique_ptr<FILE, Closer> fp;
  std::string path;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  bool openFileForRead(const char*, const char* path, HalFile& file) {
    file = HalFile(fopen(path, "rb"), path);
    return file.isOpen();
  }
  bool openFileForRead(const char* tag, const std::string& path, HalFile& file) {
    return openFileForRead(tag, path.c_str(), file);
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    file = HalFile(fopen(path, "w+b"), path);
    return file.isOpen();
  }
  bool openFileForWrite(const char* tag, const std::string& path, HalFile& file) {
    return openFileForWrite(tag, path.c_str(), file);
  }

  HalFile open(const char* path, int oflag) {
    const bool writable = (oflag & O_RDWR) != 0 || (oflag & O_WRONLY) != 0;
    return HalFile(fopen(path, writable ? "r+b" : "rb"), path);
  }

  bool exists(const char* path) {
    struct stat info;
    return stat(path, &info) == 0;
  }
  bool remove(const char* path) { return ::remove(path) == 0; }
  bool rename(const char* from, const char* to) {
    if (exists(to)) return false;  // SdFat will not overwrite
    return ::rename(from, to) == 0;
  }
  bool mkdir(const char* path, bool = true) { return ensureDirectoryExists(path); }

  bool ensureDirectoryExists(const char* path) {
    std::string built;
    const std::string full(path);
    for (size_t i = 0; i <= full.size(); i++) {
      if (i == full.size() || full[i] == '/') {
        if (built.size() > 1) {
          struct stat info;
          if (stat(built.c_str(), &info) != 0 && ::mkdir(built.c_str(), 0777) != 0) return false;
        }
      }
      if (i < full.size()) built.push_back(full[i]);
    }
    return true;
  }
};

#define Storage HalStorage::getInstance()
