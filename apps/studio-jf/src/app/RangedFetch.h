#pragma once

// RangedFetch — a large release file, downloaded in pieces so that one interrupted piece costs a retry of
// that piece, not the whole download.
//
// WHY. JHttpClient fetches a file in one exchange and keeps nothing when it fails part-way. Its receive
// timeout is a 30 s stall limit, and on OpenSSL 3 that limit expiring inside SSL_read comes back as
// SSL_ERROR_WANT_READ, which the client reports as a bare "read failed". GitHub's asset CDN does stop
// sending on a connection now and then (seen: 24 MB and 25.7 MB into the 27 MB studio AppImage), and such a
// connection does not start again — one sat silent for 180 s with a longer limit. A NEW connection asking
// for the rest is answered at once; that is why curl, on a connection that happened not to stop, got the
// same file by hand. The client itself is JFramework's; until it resumes on its own, a file fetched here
// survives a stall, a dropped connection or a short body.
//
// HOW. Each piece is a GET with "Range: bytes=<from>-<to>", through the same redirects as a whole-file
// GET (GitHub answers with a fresh signed CDN URL every time, so an expired one is never reused). A piece
// is taken only when the 206's Content-Range starts where the file has got to, its length is what was
// asked for, and its total agrees with every earlier piece; anything else is retried. A server that
// ignores Range answers 200 with the whole file, and that is taken as the whole file. A piece that keeps
// failing ends the download with the client's own reason, so what the person is told stays specific.
//
// The checksum still has the last word: callers prove the result against SHA256SUMS exactly as before.

#include <j/io/HttpClient.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rangedfetch {

// One GET, redirects followed — JHttpClient::getSync's shape, so a test can hand in a fake.
using Transport = std::function<jf::JHttpResponse(const std::string& url, int timeoutMs,
                                                  const std::vector<jf::JHttpHeader>& headers)>;

struct Options {
    int64_t pieceBytes = 4 * 1024 * 1024;   // small enough that a lost piece is seconds, not minutes
    int     pieceTimeoutMs = 3 * 60 * 1000; // one piece, connect through last byte
    int     attempts = 5;                   // per piece, before the download is given up
    int     retryDelayMs = 1000;            // before the Nth retry of a piece, N times this
    // Bytes in hand and the file's size (0 until the first piece says), after every piece. Any thread.
    std::function<void(int64_t, int64_t)> onProgress;
};

// Download `url` whole. ok() on success with the full body; otherwise `error` (or a non-2xx status from
// the server, passed through as it came) says why. `transport` defaults to JHttpClient::getSync.
jf::JHttpResponse fetch(const std::string& url, const std::vector<jf::JHttpHeader>& headers,
                        const Options& opt = {}, const Transport& transport = {});

// "bytes 0-4194303/26958328" -> from, to, total. False for anything else, "bytes */123" included.
bool parseContentRange(const std::string& value, int64_t& from, int64_t& to, int64_t& total);

}  // namespace rangedfetch
