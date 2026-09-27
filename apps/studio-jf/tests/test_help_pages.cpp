// The Lua API reference as HTML (HelpPages): every function the definition exports is on the page, in
// a code block and in the index, and nothing from the definition reaches the page unescaped.
//
//   cmake --build build --target help_pages_test && ./build/help_pages_test

#include "app/HelpPages.h"
#include "model/MetaModel.h"

#include <j/io/HttpClient.h>

#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  %s  %s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : ("   - " + detail).c_str());
    if (!ok) ++fails;
}

static size_t count(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + needle.size())) ++n;
    return n;
}

int main() {
    std::printf("=== help pages ===\n");
    ck(helppages::escape("a<b> & \"c\" 'd'") == "a&lt;b&gt; &amp; &quot;c&quot; &#39;d&#39;",
       "text is escaped for HTML");

    MetaModel meta;
    if (!meta.loadFile(REAL_META)) { std::puts("  (no meta — skipped)"); return 0; }
    const std::string html = helppages::luaReferenceHtml(meta);
    const size_t fns = meta.luaFunctions().size(), cbs = meta.luaCallbacks().size();
    ck(fns > 0, "the definition exports Lua functions", std::to_string(fns));
    ck(html.rfind("<!DOCTYPE html>", 0) == 0 && html.find("</html>") != std::string::npos,
       "the page is a complete HTML document");
    ck(count(html, "class=\"entry\"") == fns + cbs, "one entry per function and callback",
       std::to_string(count(html, "class=\"entry\"")) + " vs " + std::to_string(fns + cbs));
    for (const auto& e : meta.luaFunctions())
        if (html.find("href=\"#fn-" + helppages::escape(e.name) + "\"") == std::string::npos ||
            html.find("id=\"fn-"   + helppages::escape(e.name) + "\"") == std::string::npos) {
            ck(false, "every function is in the index and has its anchor", e.name);
            break;
        }
    ck(html.find("-> number") == std::string::npos && html.find("-&gt; number") != std::string::npos,
       "signatures are escaped (-> becomes -&gt;)");

    MetaModel empty;
    ck(helppages::luaReferenceHtml(empty).find("declares no Lua API") != std::string::npos,
       "a definition with no Lua API says so rather than showing a blank page");

    // THE MANUAL, OVER HTTP — what the browser is actually handed. The build tree's copy is found from
    // here (make manual); a shipped studio finds its own beside the executable by the same code.
    std::string why;
    const std::string url = helppages::pageUrl("manual/index.html", why);
    if (url.empty()) {
        std::printf("  (manual not built — %s; skipped)\n", why.c_str());
    } else {
        ck(url.rfind("http://127.0.0.1:", 0) == 0, "the manual is served on the loopback address", url);
        const jf::JHttpResponse page = jf::JHttpClient::getSync(url, 3000);
        ck(page.status == 200 && page.text().find("jayecu User Manual") != std::string::npos,
           "…and its front page arrives", std::to_string(page.status));
        const std::string css = url.substr(0, url.rfind('/') + 1) + "css/manual.css";
        ck(jf::JHttpClient::getSync(css, 3000).status == 200, "…and so do its stylesheets");
        ck(jf::JHttpClient::getSync(url.substr(0, url.find("/manual/")) + "/manual/../../etc/passwd", 3000).status == 404,
           "…and nothing outside it");
    }

    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
