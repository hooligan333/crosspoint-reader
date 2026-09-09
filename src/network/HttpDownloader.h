#pragma once
#include <HalStorage.h>

#include <functional>
#include <string>

/**
 * HTTP client utility for fetching content and downloading files. Built on
 * esp_http_client: https is verified against the CA bundle, plain http is
 * used for local servers (transport is chosen from the URL scheme).
 */
class HttpDownloader {
 public:
  // Called after every body chunk is written. `total` is 0 when the response
  // carries no Content-Length (chunked): the transfer is indeterminate, not
  // finished and not idle, so callers must not divide by it — and must not
  // assume the callback only fires when a percentage exists. Several callers
  // pump input from here, which is the only thing keeping Cancel alive while
  // the activity loop is blocked.
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  /**
   * Fetch text content from a URL with optional credentials.
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "");

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Download a file to the SD card with optional credentials.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "");

#ifdef CROSSPOINT_SOFT_CLOCK
  /**
   * The `Date:` header of the most recent 200 response, raw, or "" when that
   * response carried none. The soft clock (FLASHCARD_SPEC.md §7b.2) uses it as
   * its primary time source: the board has no RTC, and a feed server's own
   * clock is free, arrives on every sync, and needs no route off the LAN.
   *
   * Points into a static buffer that the next fetch overwrites — read it
   * immediately after the fetch that should have set it. Declared last so the
   * class layout above is exactly what it is without the flag.
   */
  static const char* lastResponseDate();
#endif
};
