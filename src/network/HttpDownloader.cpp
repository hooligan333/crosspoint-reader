#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <ResumableFetch.h>

#include <functional>
#include <string>
#ifdef CROSSPOINT_SOFT_CLOCK
#include <cstring>  // memcpy for the Date-header buffer below
#endif

#include "WifiPowerSaveGuard.h"

extern "C" void wolfSSL_Arduino_Serial_Print(const char* const msg) { LOG_DBG("WOLFSSL", "%s", msg); }

namespace {
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room.
constexpr int HTTP_TIMEOUT_MS = 60000;

#ifdef CROSSPOINT_SOFT_CLOCK
// Soft-clock Date-header capture (FLASHCARD_SPEC.md §7b.2). A fixed buffer, not
// a std::string: it is written on every fetch on a device with ~50 KB of
// fragmented heap, and an RFC-1123 date is 29 characters.
char g_lastResponseDate[40] = "";

void noteResponseDate(const std::string& value) {
  // A header longer than the buffer is not a date; store nothing rather than a
  // truncated string the parser would have to reject.
  const size_t len = value.size() < sizeof(g_lastResponseDate) ? value.size() : 0;
  memcpy(g_lastResponseDate, value.c_str(), len);
  g_lastResponseDate[len] = '\0';
}
#endif

// All HTTP(S) fetches go through wolfSSL (the firmware's only TLS stack: it
// speaks TLS 1.3 and reads large bodies reliably). Plain-http URLs still use a
// WiFiClient here, so this is safe for non-TLS targets too. A body cut short
// mid-transfer resumes with a Range request (see ResumableFetch.h).
HttpDownloader::DownloadError runGetSecure(const std::string& url, const std::string& username,
                                           const std::string& password,
                                           const std::vector<HttpDownloader::Header>& headers,
                                           const freeink::FetchSink& sink, const bool* cancelFlag = nullptr,
                                           size_t* bytesOut = nullptr, const bool downgradeRedirectsToHttp = false) {
  WifiPowerSaveGuard psGuard;
  freeink::FetchOptions options;
  options.redirectToHttp = downgradeRedirectsToHttp;
#ifdef CROSSPOINT_SOFT_CLOCK
  // Never let a previous fetch's Date be read as this one's.
  noteResponseDate("");
  // fetchResumable owns each attempt's client, so the Date header is read from
  // inside the body callback, where that attempt's client is alive and its
  // headers are parsed. The SDK hands the sink 2xx bodies only, so a redirect
  // hop's Date is never taken: a redirector's clock is not the feed server's.
  // A 2xx with an empty body never reaches the sink and leaves "", which the
  // sync flows treat as "no Date" and fall through to NTP.
  const freeink::SecureHttpClient* attemptClient = nullptr;
  bool attemptDateNoted = false;
  freeink::FetchSink dateSink = sink;
  dateSink.write = [&](const uint8_t* data, size_t len) {
    if (attemptClient && !attemptDateNoted) {
      noteResponseDate(attemptClient->getHeader("date"));
      attemptDateNoted = true;
    }
    return sink.write(data, len);
  };
#endif
  const freeink::FetchResult result = freeink::fetchResumable(
      url, options,
      [&](freeink::SecureHttpClient& http, const bool sameOrigin) {
#ifdef CROSSPOINT_SOFT_CLOCK
        attemptClient = &http;
        attemptDateNoted = false;
#endif
        http.setTimeout(HTTP_TIMEOUT_MS);
        http.setInsecure();
        // setUserAgent replaces SecureHttpClient's built-in UA; addHeader would
        // append a second User-Agent header, which strict servers reject (aiohttp
        // answers 400 "Duplicate 'User-Agent' header found").
        http.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
        // Credentials and caller headers stay with the starting origin; a
        // redirect elsewhere (or to plain http) gets neither.
        if (sameOrigin) {
          if (!username.empty() && !password.empty()) http.setBasicAuth(username, password);
          for (const auto& h : headers) http.addHeader(h.first, h.second);
        }
        LOG_DBG("HTTP", "wolfSSL GET: %s (heap %u, max block %u)", url.c_str(), (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap());
      },
#ifdef CROSSPOINT_SOFT_CLOCK
      dateSink,
#else
      sink,
#endif
      [cancelFlag] { return cancelFlag && *cancelFlag; });
  if (bytesOut) *bytesOut = result.bytes;

  if (result.aborted) return HttpDownloader::ABORTED;
  if (result.stopped) return HttpDownloader::FILE_ERROR;
  if (result.status == 401 || result.status == 403) {
    LOG_ERR("HTTP", "wolfSSL request unauthorized: status %d: %s", result.status, url.c_str());
    return HttpDownloader::UNAUTHORIZED;
  }
  if (result.status < 200 || result.status >= 300) {
    LOG_ERR("HTTP", "wolfSSL request failed: status %d: %s", result.status, url.c_str());
    return HttpDownloader::HTTP_ERROR;
  }
  if (!result.complete) {
    LOG_ERR("HTTP", "wolfSSL incomplete: got %zu of %zu bytes", result.bytes, result.total);
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}

}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  return fetchUrl(
      url, [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; }, username,
      password);
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  freeink::FetchSink sink;
  sink.write = onData;
  return runGetSecure(url, username, password, {}, sink) == OK;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, const bool* cancelFlag,
                                                             const std::string& username, const std::string& password,
                                                             const std::vector<Header>& headers,
                                                             bool downgradeRedirectsToHttp) {
  LOG_DBG("HTTP", "Downloading: %s -> %s", url.c_str(), destPath.c_str());

  // Stage in <dest>.part: a failed or cancelled download never replaces an
  // existing copy, and a partial file never sits under the real name.
  const std::string partPath = destPath + ".part";
  Storage.remove(partPath.c_str());
  HalFile file;
  if (!Storage.openFileForWrite("HTTP", partPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    return FILE_ERROR;
  }

  freeink::FetchSink sink;
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };
  // Reopening for write truncates: the server restarted the body from byte 0.
  sink.rewind = [&file, &partPath] {
    file.close();
    return Storage.openFileForWrite("HTTP", partPath.c_str(), file);
  };
  sink.progress = progress;

  size_t downloaded = 0;
  const DownloadError result =
      runGetSecure(url, username, password, headers, sink, cancelFlag, &downloaded, downgradeRedirectsToHttp);
  // Close before any remove() on the same path; DESTRUCTOR_CLOSES_FILE would
  // otherwise close only after the remove. A failed rewind leaves no open handle.
  if (file.isOpen()) file.close();

  if (result != OK) {
    Storage.remove(partPath.c_str());
    return result;
  }
  if (downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    Storage.remove(partPath.c_str());
    return HTTP_ERROR;
  }
  if (!Storage.replaceFile(partPath.c_str(), destPath.c_str())) {
    LOG_ERR("HTTP", "Failed to move download into place: %s", destPath.c_str());
    Storage.remove(partPath.c_str());
    return FILE_ERROR;
  }
  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);
  return OK;
}

#ifdef CROSSPOINT_SOFT_CLOCK
// Every fetch goes through runGetSecure (wolfSSL via fetchResumable), which
// fills the buffer from the 2xx response's first body chunk. A response with no
// Date header -- or a 2xx with an empty body -- leaves "", and the sync flows
// fall straight through to the NTP fallback, which is the intended degradation.
const char* HttpDownloader::lastResponseDate() { return g_lastResponseDate; }
#endif
