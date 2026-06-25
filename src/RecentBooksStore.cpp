#include "RecentBooksStore.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <iterator>

namespace {
constexpr uint8_t RECENT_BOOKS_FILE_VERSION = 3;
constexpr char RECENT_BOOKS_FILE_BIN[] = "/.crosspoint/recent.bin";
constexpr char RECENT_BOOKS_FILE_JSON[] = "/.crosspoint/recent.json";
constexpr char RECENT_BOOKS_FILE_BAK[] = "/.crosspoint/recent.bin.bak";
constexpr int MAX_RECENT_BOOKS = 10;
constexpr uint32_t TXT_CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t TXT_CACHE_VERSION = 3;

int normalizeProgress(int progressPercent) {
  if (progressPercent < 0) return -1;
  if (progressPercent > 100) return 100;
  return progressPercent;
}

int readEpubProgressPercent(const std::string& path) {
  Epub epub(path, "/.crosspoint");
  HalFile f;
  if (!Storage.openFileForRead("RBS", epub.getCachePath() + "/progress.bin", f)) {
    return -1;
  }

  uint8_t data[6];
  const int dataSize = f.read(data, 6);
  f.close();
  if (dataSize != 6) {
    return -1;
  }

  const int spineIndex = data[0] + (data[1] << 8);
  const int currentPage = data[2] + (data[3] << 8);
  const int pageCount = data[4] + (data[5] << 8);
  if (pageCount <= 0 || currentPage == UINT16_MAX) {
    return -1;
  }

  if (!epub.load(false, true) || epub.getBookSize() == 0) {
    return -1;
  }

  const float chapterProgress = static_cast<float>(currentPage) / static_cast<float>(pageCount);
  return normalizeProgress(static_cast<int>(epub.calculateProgress(spineIndex, chapterProgress) * 100.0f + 0.5f));
}

int readXtcProgressPercent(const std::string& path) {
  Xtc xtc(path, "/.crosspoint");
  HalFile f;
  if (!Storage.openFileForRead("RBS", xtc.getCachePath() + "/progress.bin", f)) {
    return -1;
  }

  uint8_t data[4];
  const int dataSize = f.read(data, 4);
  f.close();
  if (dataSize != 4 || !xtc.load()) {
    return -1;
  }

  const uint32_t pageCount = xtc.getPageCount();
  if (pageCount == 0) {
    return -1;
  }

  uint32_t currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
  if (currentPage >= pageCount) {
    currentPage = pageCount - 1;
  }
  return normalizeProgress(xtc.calculateProgress(currentPage));
}

int readTxtCachedPageCount(const std::string& cachePath) {
  HalFile f;
  if (!Storage.openFileForRead("RBS", cachePath + "/index.bin", f)) {
    return -1;
  }

  uint32_t magic;
  uint8_t version;
  uint32_t ignoredFileSize;
  int32_t ignoredWidth;
  int32_t ignoredLines;
  int32_t ignoredFont;
  int32_t ignoredMargin;
  uint8_t ignoredAlignment;
  uint32_t numPages;
  serialization::readPod(f, magic);
  serialization::readPod(f, version);
  serialization::readPod(f, ignoredFileSize);
  serialization::readPod(f, ignoredWidth);
  serialization::readPod(f, ignoredLines);
  serialization::readPod(f, ignoredFont);
  serialization::readPod(f, ignoredMargin);
  serialization::readPod(f, ignoredAlignment);
  serialization::readPod(f, numPages);
  f.close();

  if (magic != TXT_CACHE_MAGIC || version != TXT_CACHE_VERSION || numPages == 0 || numPages > INT32_MAX) {
    return -1;
  }
  return static_cast<int>(numPages);
}

int readTxtProgressPercent(const std::string& path) {
  Txt txt(path, "/.crosspoint");
  HalFile f;
  if (!Storage.openFileForRead("RBS", txt.getCachePath() + "/progress.bin", f)) {
    return -1;
  }

  uint8_t data[4];
  const int dataSize = f.read(data, 4);
  f.close();
  if (dataSize != 4) {
    return -1;
  }

  const int totalPages = readTxtCachedPageCount(txt.getCachePath());
  if (totalPages <= 0) {
    return -1;
  }

  int currentPage = data[0] + (data[1] << 8);
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;
  return normalizeProgress(static_cast<int>((currentPage + 1) * 100.0f / totalPages + 0.5f));
}

int readProgressPercent(const std::string& path) {
  const size_t lastSlash = path.find_last_of('/');
  const std::string fileName = lastSlash == std::string::npos ? path : path.substr(lastSlash + 1);
  if (FsHelpers::hasEpubExtension(fileName)) {
    return readEpubProgressPercent(path);
  }
  if (FsHelpers::hasXtcExtension(fileName)) {
    return readXtcProgressPercent(path);
  }
  if (FsHelpers::hasTxtExtension(fileName) || FsHelpers::hasMarkdownExtension(fileName)) {
    return readTxtProgressPercent(path);
  }
  return -1;
}
}  // namespace

RecentBooksStore RecentBooksStore::instance;

void RecentBooksStore::addBook(const std::string& path, const std::string& title, const std::string& author,
                               const std::string& coverBmpPath) {
  // Drop stale entries first so a new add can't evict a valid book in their stead.
  pruneMissing();

  // Remove existing entry if present
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    recentBooks.erase(it);
  }

  // Add to front
  RecentBook book{path, title, author, coverBmpPath};
  refreshProgress(book);
  recentBooks.insert(recentBooks.begin(), book);

  // Trim to max size
  if (recentBooks.size() > MAX_RECENT_BOOKS) {
    recentBooks.resize(MAX_RECENT_BOOKS);
  }

  saveToFile();
}

void RecentBooksStore::updateBook(const std::string& path, const std::string& title, const std::string& author,
                                  const std::string& coverBmpPath) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    RecentBook& book = *it;
    book.title = title;
    book.author = author;
    book.coverBmpPath = coverBmpPath;
    refreshProgress(book);
    saveToFile();
  }
}

void RecentBooksStore::refreshProgress(RecentBook& book) const { book.progressPercent = readProgressPercent(book.path); }

bool RecentBooksStore::removeByPath(const std::string& path) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }
  recentBooks.erase(it);
  if (!saveToFile()) {
    LOG_ERR("RBS", "Failed to persist removal of recent book: %s", path.c_str());
  }
  return true;
}

void RecentBooksStore::updatePath(const std::string& oldPath, const std::string& newPath,
                                  const std::string& oldCachePath, const std::string& newCachePath) {
  auto it = std::find_if(recentBooks.begin(), recentBooks.end(),
                         [&](const RecentBook& book) { return book.path == oldPath; });
  if (it == recentBooks.end()) {
    return;
  }
  it->path = newPath;
  if (!oldCachePath.empty() && !it->coverBmpPath.empty() && it->coverBmpPath.rfind(oldCachePath, 0) == 0) {
    it->coverBmpPath = newCachePath + it->coverBmpPath.substr(oldCachePath.size());
  }
  saveToFile();
}

bool RecentBooksStore::isMissing(const RecentBook& book) { return !Storage.exists(book.path.c_str()); }

bool RecentBooksStore::pruneMissing() {
  const size_t before = recentBooks.size();
  recentBooks.erase(std::remove_if(recentBooks.begin(), recentBooks.end(), &isMissing), recentBooks.end());
  return recentBooks.size() != before;
}

bool RecentBooksStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveRecentBooks(*this, RECENT_BOOKS_FILE_JSON);
}

RecentBook RecentBooksStore::getDataFromBook(std::string path) const {
  std::string lastBookFileName = "";
  const size_t lastSlash = path.find_last_of('/');
  if (lastSlash != std::string::npos) {
    lastBookFileName = path.substr(lastSlash + 1);
  }

  LOG_DBG("RBS", "Loading recent book: %s", path.c_str());

  // If epub, try to load the metadata for title/author and cover.
  // Use buildIfMissing=false to avoid heavy epub loading on boot; getTitle()/getAuthor() may be
  // blank until the book is opened, and entries with missing title are omitted from recent list.
  if (FsHelpers::hasEpubExtension(lastBookFileName)) {
    Epub epub(path, "/.crosspoint");
    epub.load(false, true);
    RecentBook book{path, epub.getTitle(), epub.getAuthor(), epub.getThumbBmpPath()};
    refreshProgress(book);
    return book;
  } else if (FsHelpers::hasXtcExtension(lastBookFileName)) {
    // Handle XTC file
    Xtc xtc(path, "/.crosspoint");
    if (xtc.load()) {
      RecentBook book{path, xtc.getTitle(), xtc.getAuthor(), xtc.getThumbBmpPath()};
      refreshProgress(book);
      return book;
    }
  } else if (FsHelpers::hasTxtExtension(lastBookFileName) || FsHelpers::hasMarkdownExtension(lastBookFileName)) {
    RecentBook book{path, lastBookFileName, "", ""};
    refreshProgress(book);
    return book;
  }
  RecentBook book{path, "", "", ""};
  refreshProgress(book);
  return book;
}

bool RecentBooksStore::loadFromFile() {
  // Try JSON first
  if (Storage.exists(RECENT_BOOKS_FILE_JSON)) {
    String json = Storage.readFile(RECENT_BOOKS_FILE_JSON);
    if (!json.isEmpty()) {
      return JsonSettingsIO::loadRecentBooks(*this, json.c_str());
    }
  }

  // Fall back to binary migration
  if (Storage.exists(RECENT_BOOKS_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      saveToFile();
      Storage.rename(RECENT_BOOKS_FILE_BIN, RECENT_BOOKS_FILE_BAK);
      LOG_DBG("RBS", "Migrated recent.bin to recent.json");
      return true;
    }
  }

  return false;
}

bool RecentBooksStore::loadFromBinaryFile() {
  HalFile inputFile;
  if (!Storage.openFileForRead("RBS", RECENT_BOOKS_FILE_BIN, inputFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version == 1 || version == 2) {
    // Old version, just read paths
    uint8_t count;
    serialization::readPod(inputFile, count);
    recentBooks.clear();
    recentBooks.reserve(count);
    for (uint8_t i = 0; i < count; i++) {
      std::string path;
      serialization::readString(inputFile, path);

      // load book to get missing data
      RecentBook book = getDataFromBook(path);
      if (book.title.empty() && book.author.empty() && version == 2) {
        // Fall back to loading what we can from the store
        std::string title, author;
        serialization::readString(inputFile, title);
        serialization::readString(inputFile, author);
        RecentBook book{path, title, author, ""};
        refreshProgress(book);
        recentBooks.push_back(book);
      } else {
        recentBooks.push_back(book);
      }
    }
  } else if (version == 3) {
    uint8_t count;
    serialization::readPod(inputFile, count);

    recentBooks.clear();
    recentBooks.reserve(count);
    uint8_t omitted = 0;

    for (uint8_t i = 0; i < count; i++) {
      std::string path, title, author, coverBmpPath;
      serialization::readString(inputFile, path);
      serialization::readString(inputFile, title);
      serialization::readString(inputFile, author);
      serialization::readString(inputFile, coverBmpPath);

      // Omit books with missing title (e.g. saved before metadata was available)
      if (title.empty()) {
        omitted++;
        continue;
      }

      RecentBook book{path, title, author, coverBmpPath};
      refreshProgress(book);
      recentBooks.push_back(book);
    }

    if (omitted > 0) {
      // Explicitly close() file before saveToFile() rewrites the same file
      inputFile.close();
      saveToFile();
      LOG_DBG("RBS", "Omitted %u recent book(s) with missing title", omitted);
      return true;
    }
  } else {
    LOG_ERR("RBS", "Deserialization failed: Unknown version %u", version);
    return false;
  }

  LOG_DBG("RBS", "Recent books loaded from binary file (%d entries)", static_cast<int>(recentBooks.size()));
  return true;
}
