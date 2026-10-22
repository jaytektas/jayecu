#include "RangedFetch.h"

#include <j/core/Log.h>

#include <chrono>
#include <cstdlib>
#include <thread>

namespace rangedfetch {

bool parseContentRange(const std::string& value, int64_t& from, int64_t& to, int64_t& total) {
    const std::string unit = "bytes ";
    if (value.compare(0, unit.size(), unit) != 0) return false;
    const char* p = value.c_str() + unit.size();
    char* end = nullptr;
    // Digits only: strtoll would also take a sign or leading space, and "bytes -5-3/9" is not a range.
    auto number = [&](int64_t& into) {
        if (*p < '0' || *p > '9') return false;
        into = std::strtoll(p, &end, 10);
        p = end;
        return true;
    };
    if (!number(from) || *p++ != '-' || !number(to) || *p++ != '/' || !number(total) || *p) return false;
    return from <= to && to < total;
}

namespace {

std::string mb(int64_t bytes) {
    const int64_t tenths = bytes * 10 / (1024 * 1024);
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " MB";
}

}  // namespace

jf::JHttpResponse fetch(const std::string& url, const std::vector<jf::JHttpHeader>& headers,
                        const Options& opt, const Transport& transport) {
    const Transport get = transport ? transport
        : Transport([](const std::string& u, int ms, const std::vector<jf::JHttpHeader>& hs) {
              return jf::JHttpClient::getSync(u, ms, hs);
          });

    std::vector<uint8_t> body;
    int64_t total = -1;                                 // unknown until the first piece says
    int     failures = 0;                               // in a row, at the current offset
    std::string finalUrl = url;

    while (total < 0 || static_cast<int64_t>(body.size()) < total) {
        const int64_t from = static_cast<int64_t>(body.size());
        std::vector<jf::JHttpHeader> hs = headers;
        hs.push_back({ "Range", "bytes=" + std::to_string(from) + "-" + std::to_string(from + opt.pieceBytes - 1) });
        jf::JHttpResponse r = get(url, opt.pieceTimeoutMs, hs);

        std::string why;
        bool retry = true;                              // false: asking again cannot give a different answer
        int64_t a = 0, b = 0, t = 0;
        if (!r.error.empty()) {
            why = r.error;
        } else if (r.status == 200 && (from == 0 || static_cast<int64_t>(r.body.size()) == total)) {
            // A server that ignores Range sends the whole file — at the start, or (a CDN node that does
            // not do ranges, met part-way) the whole file again, which is just as good.
            return r;
        } else if (r.status == 416 && from == 0) {
            // Nothing to range over: a zero-length file. Ask for it plainly and take what comes.
            return get(url, opt.pieceTimeoutMs, headers);
        } else if (r.status == 206) {
            if (!parseContentRange(r.header("Content-Range"), a, b, t))
                why = "the server sent a part without a usable Content-Range ('" + r.header("Content-Range") + "')";
            else if (a != from || static_cast<int64_t>(r.body.size()) != b - a + 1)
                why = "the server sent " + std::to_string(r.body.size()) + " byte(s) as bytes " + std::to_string(a) +
                      "-" + std::to_string(b) + " when bytes from " + std::to_string(from) + " were asked for";
            else if (total >= 0 && t != total) {
                // Replaced on the server mid-way. The bytes in hand are the old file's, so no later piece can
                // complete it; a fresh download from the start (the person's "try again") can.
                why = "the file changed size during the download (" + std::to_string(total) + " -> " +
                      std::to_string(t) + " bytes)";
                retry = false;
            }
        } else if (r.status >= 500) {
            why = "HTTP " + std::to_string(r.status);   // the server's trouble, and usually brief: retried
        } else {
            return r;                                   // a server speaking (404, 403...): its answer stands
        }

        if (why.empty()) {
            body.insert(body.end(), r.body.begin(), r.body.end());
            total    = t;
            failures = 0;
            finalUrl = r.finalUrl;
            if (opt.onProgress) opt.onProgress(static_cast<int64_t>(body.size()), total);
            continue;
        }

        ++failures;
        const std::string where = " at " + mb(from) + (total > 0 ? " of " + mb(total) : std::string());
        if (!retry || failures >= opt.attempts) {
            jf::JHttpResponse out;
            out.finalUrl = finalUrl;
            out.error = why + " (" + std::to_string(failures) + (failures == 1 ? " try" : " tries") + where + ")";
            return out;
        }
        JLOGC("http", jf::JLogLevel::Warn) << "piece" << where << " failed (" << why << "), trying again ("
                                           << failures << " of " << opt.attempts << ")";
        // A pause that grows with each failure: a stalled CDN node or a dropped route often needs a moment,
        // and asking again at once would only spend the attempts.
        if (opt.retryDelayMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(opt.retryDelayMs * failures));
    }

    jf::JHttpResponse out;
    out.status   = 200;
    out.finalUrl = finalUrl;
    out.body     = std::move(body);
    return out;
}

}  // namespace rangedfetch
