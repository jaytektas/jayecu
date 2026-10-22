// A large release file downloaded in pieces (RangedFetch) — against a fake server, so no network.
//
// The bug this guards: the studio's update download of a 27 MB AppImage failed with "read failed" while
// curl fetched the same file fine. GitHub's asset CDN stopped sending part-way, JHttpClient's 30 s stall
// limit expired inside SSL_read, OpenSSL 3 reported it as WANT_READ, and the client gave up on the whole
// file with nothing kept. What must hold here:
//   * every byte arrives, in order, whatever the piece size and the file size have in common (nothing);
//   * a piece that fails the way the bench's did — "read failed", a stall, a short or misplaced 206, a 5xx —
//     is asked for again, and only that piece;
//   * a piece is asked of the ORIGINAL url every time, never the signed CDN url it was redirected to (those
//     expire), and the caller's headers go with every piece;
//   * a server that ignores Range, a zero-length file, a 404 and a file that changes size mid-way each get
//     the answer they deserve rather than a retry loop or a stitched-together file;
//   * a piece that keeps failing ends the download with the client's own reason, not a vague one.
//
//   cmake --build build --target ranged_fetch_test && ./build/ranged_fetch_test

#include "app/RangedFetch.h"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

static int fails = 0;
static void check(const char* what, bool ok, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : " — ", detail.c_str());
    if (!ok) ++fails;
}

// GitHub's shape: the asset URL redirects to a long signed URL on another host. getSync follows that, so the
// fake answers with the signed URL as finalUrl, as the real transport would.
static const std::string kAssetUrl =
    "https://github.com/jaytektas/jayecu/releases/download/v0.3.22/jayecu-studio-0.3.22-x86_64.AppImage";
static const std::string kSignedUrl =
    "https://release-assets.githubusercontent.com/github-production-release-asset/1/abc?sp=r&sv=2018-11-09&" +
    std::string(900, 'x');

// A file server that honours "Range: bytes=a-b" the way GitHub's CDN does, with a hook to misbehave on any
// one request. Every request is recorded.
struct FakeServer {
    std::vector<uint8_t> file;
    bool honoursRange = true;
    struct Request { std::string url, range, agent; };
    std::vector<Request> requests;
    // Called with the request's index and the answer about to be sent; may replace it.
    std::function<void(size_t, jf::JHttpResponse&)> fault;

    jf::JHttpResponse operator()(const std::string& url, int, const std::vector<jf::JHttpHeader>& hs) {
        Request q{ url, "", "" };
        for (const auto& h : hs) {
            if (h.name == "Range") q.range = h.value;
            if (h.name == "User-Agent") q.agent = h.value;
        }
        requests.push_back(q);

        jf::JHttpResponse r;
        r.finalUrl = kSignedUrl;
        long long a = -1, b = -1;
        if (honoursRange && !q.range.empty() && std::sscanf(q.range.c_str(), "bytes=%lld-%lld", &a, &b) == 2) {
            const long long size = static_cast<long long>(file.size());
            if (a >= size) {
                r.status = 416;
                r.headers.push_back({ "Content-Range", "bytes */" + std::to_string(size) });
            } else {
                if (b >= size) b = size - 1;
                r.status = 206;
                r.body.assign(file.begin() + a, file.begin() + b + 1);
                r.headers.push_back({ "Content-Range", "bytes " + std::to_string(a) + "-" + std::to_string(b) +
                                                       "/" + std::to_string(size) });
            }
        } else {
            r.status = 200;
            r.body   = file;
        }
        if (fault) fault(requests.size() - 1, r);
        return r;
    }
};

// A transport failure exactly as JHttpClient reports one: no status, no body, a reason.
static void failWith(jf::JHttpResponse& r, const std::string& why) {
    r = jf::JHttpResponse{};
    r.error = why;
}

static std::vector<uint8_t> makeFile(size_t n) {
    std::vector<uint8_t> f(n);
    uint32_t x = 2463534242u;                            // xorshift: no two pieces look alike
    for (auto& c : f) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; c = static_cast<uint8_t>(x); }
    return f;
}

static rangedfetch::Options opts() {
    rangedfetch::Options o;
    o.pieceBytes   = 1000003;                         // prime: pieces never line up with the file's end
    o.retryDelayMs = 0;
    return o;
}

static const std::vector<jf::JHttpHeader> kHeaders = { { "User-Agent", "jayecu-studio/test" } };

static jf::JHttpResponse run(FakeServer& s, const rangedfetch::Options& o = opts()) {
    return rangedfetch::fetch(kAssetUrl, kHeaders, o,
                              [&s](const std::string& u, int ms, const std::vector<jf::JHttpHeader>& h) {
                                  return s(u, ms, h);
                              });
}

int main() {
    std::printf("=== a release file in pieces ===\n");

    {
        std::printf("Content-Range\n");
        int64_t a, b, t;
        check("a whole range parses", rangedfetch::parseContentRange("bytes 0-4194303/26958328", a, b, t) &&
                                          a == 0 && b == 4194303 && t == 26958328);
        check("the last byte alone parses", rangedfetch::parseContentRange("bytes 9-9/10", a, b, t) && a == 9 && b == 9);
        check("an unsatisfied range is not a range", !rangedfetch::parseContentRange("bytes */10", a, b, t));
        check("an end past the total is refused", !rangedfetch::parseContentRange("bytes 0-10/10", a, b, t));
        check("a backwards range is refused", !rangedfetch::parseContentRange("bytes 5-4/10", a, b, t));
        check("a sign is not a digit", !rangedfetch::parseContentRange("bytes -5-4/10", a, b, t));
        check("trailing junk is refused", !rangedfetch::parseContentRange("bytes 0-4/10x", a, b, t));
        check("another unit is refused", !rangedfetch::parseContentRange("items 0-4/10", a, b, t));
    }

    FakeServer s;
    s.file = makeFile(26958328);                       // the studio AppImage's size

    {
        std::printf("a clean download\n");
        std::vector<std::pair<int64_t, int64_t>> progress;
        rangedfetch::Options o = opts();
        o.onProgress = [&](int64_t got, int64_t total) { progress.emplace_back(got, total); };
        const auto r = run(s, o);
        check("ok", r.ok(), r.error);
        check("every byte, in order", r.body == s.file);
        check("in ceil(size / piece) pieces", s.requests.size() == 27, std::to_string(s.requests.size()));
        bool sameUrl = true, agent = true;
        for (const auto& q : s.requests) { sameUrl &= q.url == kAssetUrl; agent &= q.agent == "jayecu-studio/test"; }
        check("every piece asks the asset URL, not the signed one it redirected to", sameUrl);
        check("the caller's headers go with every piece", agent);
        check("the first piece asks for the first bytes", s.requests[0].range == "bytes=0-1000002", s.requests[0].range);
        check("the second follows on", s.requests[1].range == "bytes=1000003-2000005", s.requests[1].range);
        check("the final URL is where the bytes came from", r.finalUrl == kSignedUrl);
        bool rising = !progress.empty();
        for (size_t i = 1; i < progress.size(); ++i) rising &= progress[i].first > progress[i - 1].first;
        check("progress rises to the whole file",
              rising && progress.back().first == 26958328 && progress.back().second == 26958328);
    }

    {
        std::printf("the bench's failure: \"read failed\" 24 MB in\n");
        s.requests.clear();
        s.fault = [](size_t i, jf::JHttpResponse& r) { if (i == 24 || i == 25) failWith(r, "read failed"); };
        const auto r = run(s);
        check("ok all the same", r.ok(), r.error);
        check("every byte, in order", r.body == s.file);
        check("only the failed piece was asked again (27 + 2)", s.requests.size() == 29, std::to_string(s.requests.size()));
        check("asked again from where it failed",
              s.requests[24].range == s.requests[25].range && s.requests[25].range == s.requests[26].range);
    }

    {
        std::printf("every piece stalls once\n");
        s.requests.clear();
        s.fault = [](size_t i, jf::JHttpResponse& r) {
            if (i % 2 == 0) failWith(r, "the connection stalled (no data for 30 s)");
        };
        const auto r = run(s);
        check("ok", r.ok(), r.error);
        check("every byte, in order", r.body == s.file);
        check("each piece twice", s.requests.size() == 54, std::to_string(s.requests.size()));
    }

    {
        std::printf("a piece cut short, and a piece from the wrong place\n");
        s.requests.clear();
        s.fault = [](size_t i, jf::JHttpResponse& r) {
            if (i == 3) r.body.resize(r.body.size() - 1);          // claims a range it does not carry
            if (i == 7) r.body.resize(4096);
            if (i == 10) {                                          // a range that starts one byte late
                r.body.erase(r.body.begin());
                int64_t a, b, t;
                rangedfetch::parseContentRange(r.header("Content-Range"), a, b, t);
                r.headers = { { "Content-Range", "bytes " + std::to_string(a + 1) + "-" + std::to_string(b) +
                                                   "/" + std::to_string(t) } };
            }
            if (i == 13) r.headers.clear();                         // no Content-Range at all
        };
        const auto r = run(s);
        check("ok", r.ok(), r.error);
        check("every byte, in order: no short piece was stitched in", r.body == s.file);
        check("each bad piece asked again", s.requests.size() == 31, std::to_string(s.requests.size()));
    }

    {
        std::printf("the server's own trouble\n");
        s.requests.clear();
        s.fault = [](size_t i, jf::JHttpResponse& r) {
            if (i == 1) { r = jf::JHttpResponse{}; r.status = 503; }
        };
        const auto r = run(s);
        check("a 503 is retried", r.ok() && r.body == s.file && s.requests.size() == 28, r.error);

        s.requests.clear();
        s.fault = [](size_t i, jf::JHttpResponse& r) {
            if (i == 0) { r = jf::JHttpResponse{}; r.status = 404; r.body = { 'N', 'o', 't' }; }
        };
        const auto n = run(s);
        check("a 404 is the server's answer, passed back as it came",
              n.status == 404 && n.error.empty() && n.body.size() == 3 && s.requests.size() == 1);
    }

    {
        std::printf("a piece that never comes\n");
        s.requests.clear();
        s.fault = [](size_t i, jf::JHttpResponse& r) { if (i >= 5) failWith(r, "read failed"); };
        const auto r = run(s);
        check("not ok", !r.ok());
        check("no part of the file is handed back", r.body.empty());
        check("tried the default five times, then stopped", s.requests.size() == 5 + 5, std::to_string(s.requests.size()));
        check("the reason is the client's, with where it stopped",
              r.error.find("read failed") == 0 && r.error.find("5 tries") != std::string::npos &&
              r.error.find("of 25.7 MB") != std::string::npos, r.error);
    }

    {
        std::printf("the file changes under the download\n");
        s.requests.clear();
        s.fault = [](size_t i, jf::JHttpResponse& r) {
            if (i < 2) return;
            int64_t a, b, t;
            rangedfetch::parseContentRange(r.header("Content-Range"), a, b, t);
            r.headers = { { "Content-Range", "bytes " + std::to_string(a) + "-" + std::to_string(b) + "/" +
                                               std::to_string(t + 1) } };
        };
        const auto r = run(s);
        check("not stitched together from two files", !r.ok() && r.body.empty());
        check("and said so", r.error.find("changed size") != std::string::npos, r.error);
        check("at once: the bytes in hand are the old file's, so no retry can finish it", s.requests.size() == 3,
              std::to_string(s.requests.size()));
    }

    {
        std::printf("servers that do not range\n");
        s.requests.clear();
        s.fault = nullptr;
        s.honoursRange = false;
        const auto r = run(s);
        check("a 200 to a ranged request is the whole file", r.ok() && r.body == s.file && s.requests.size() == 1);

        FakeServer mixed;                                   // one CDN node ranges, the next does not
        mixed.file  = s.file;
        mixed.fault = [&s](size_t i, jf::JHttpResponse& r) {
            if (i >= 2) { r = jf::JHttpResponse{}; r.status = 200; r.body = s.file; }
        };
        const auto m = run(mixed);
        check("a whole file met part-way is taken whole", m.ok() && m.body == s.file && mixed.requests.size() == 3);

        FakeServer empty;
        const auto e = run(empty);
        check("a zero-length file: 416, then asked for plainly",
              e.ok() && e.body.empty() && empty.requests.size() == 2 && empty.requests[1].range.empty());
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "all passed", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
