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

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

class HalFile {
 public:
  HalFile() = default;
  explicit HalFile(FILE* handle) : fp(handle) {}
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
  bool seek(size_t pos) { return fp && fseek(fp.get(), static_cast<long>(pos), SEEK_SET) == 0; }
  int read(void* buf, size_t count) {
    if (!fp) return -1;
    return static_cast<int>(fread(buf, 1, count, fp.get()));
  }
  size_t write(const void* buf, size_t count) {
    if (!fp) return 0;
    return fwrite(buf, 1, count, fp.get());
  }
  void flush() {
    if (fp) fflush(fp.get());
  }
  bool isOpen() const { return fp != nullptr; }
  explicit operator bool() const { return fp != nullptr; }

 private:
  struct Closer {
    void operator()(FILE* handle) const {
      if (handle != nullptr) fclose(handle);
    }
  };
  std::unique_ptr<FILE, Closer> fp;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  bool openFileForRead(const char*, const char* path, HalFile& file) {
    file = HalFile(fopen(path, "rb"));
    return file.isOpen();
  }
  bool openFileForRead(const char* tag, const std::string& path, HalFile& file) {
    return openFileForRead(tag, path.c_str(), file);
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    file = HalFile(fopen(path, "w+b"));
    return file.isOpen();
  }
  bool openFileForWrite(const char* tag, const std::string& path, HalFile& file) {
    return openFileForWrite(tag, path.c_str(), file);
  }

  HalFile open(const char* path, int oflag) {
    const bool writable = (oflag & O_RDWR) != 0 || (oflag & O_WRONLY) != 0;
    return HalFile(fopen(path, writable ? "r+b" : "rb"));
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
