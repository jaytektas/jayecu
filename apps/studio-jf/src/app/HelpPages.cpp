#include "HelpPages.h"

#include "../model/MetaModel.h"
#include "../model/StudioPaths.h"
#include "Resources.h"

#include <j/io/JLocalWebServer.h>
#include <j/platform/JDesktop.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace helppages {

std::string escape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += c;
        }
    }
    return out;
}

namespace {

// THE PAGES GO TO THE BROWSER OVER HTTP, not as file:// paths. A browser sandboxed as a snap (Ubuntu's
// default Firefox and Chromium) cannot read the manual inside the AppImage's mount or the help pages
// under the hidden data folder; over http://127.0.0.1 every browser can. Started the first time help
// is opened, and stopped as the studio exits.
std::string manualDir() {
    namespace fs = std::filesystem;
    const fs::path exe = resources::exeDir();
    for (const fs::path& d : { exe / "manual",                                // shipped beside the studio
                               exe / ".." / ".." / ".." / "manual" / "site" }) {   // the build tree's copy
        std::error_code ec;
        if (fs::exists(d / "index.html", ec)) return fs::weakly_canonical(d, ec).string();
    }
    return {};
}

jf::JLocalWebServer* server(std::string& error) {
    static jf::JLocalWebServer web;
    static bool mounted = false;
    if (!mounted) {
        mounted = true;
        web.mount("help", StudioPaths::dataDir("help"));
        if (const std::string m = manualDir(); !m.empty()) web.mount("manual", m);
    }
    return web.start(error) ? &web : nullptr;
}

bool openPage(const std::string& path, std::string& error);

// One stylesheet for every help page, light or dark as the reader's system is.
constexpr const char* kStyle = R"CSS(
:root { --bg:#ffffff; --fg:#1d1f23; --muted:#5f6670; --rule:#e2e4e8; --code-bg:#f3f4f6; --accent:#c8641e; }
@media (prefers-color-scheme: dark) {
  :root { --bg:#16181c; --fg:#e4e6ea; --muted:#9aa1ab; --rule:#2c3038; --code-bg:#20232a; --accent:#f08a3c; }
}
* { box-sizing: border-box; }
body { margin:0; background:var(--bg); color:var(--fg);
       font: 15px/1.55 system-ui, -apple-system, "Segoe UI", Ubuntu, sans-serif; }
main { max-width: 860px; margin: 0 auto; padding: 32px 20px 64px; }
h1 { font-size: 1.7rem; margin: 0 0 4px; }
.sub { color: var(--muted); margin: 0 0 28px; }
h2 { font-size: 1.2rem; margin: 36px 0 12px; padding-bottom: 6px; border-bottom: 1px solid var(--rule); }
nav { columns: 3 180px; margin: 0 0 8px; }
nav a { display:block; padding: 1px 0; color: var(--accent); text-decoration: none;
        font-family: ui-monospace, "Ubuntu Mono", Consolas, monospace; font-size: 0.9rem; }
nav a:hover { text-decoration: underline; }
.entry { padding: 12px 0; border-bottom: 1px solid var(--rule); }
.entry:last-child { border-bottom: 0; }
code.sig { display:inline-block; background: var(--code-bg); padding: 3px 8px; border-radius: 4px;
           font-family: ui-monospace, "Ubuntu Mono", Consolas, monospace; font-size: 0.95rem; }
.entry p { margin: 6px 0 0; }
.empty { color: var(--muted); }
)CSS";

std::string page(const std::string& title, const std::string& subtitle, const std::string& body) {
    return "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
           "<title>" + escape(title) + "</title>\n<style>" + kStyle + "</style>\n</head>\n<body>\n<main>\n"
           "<h1>" + escape(title) + "</h1>\n<p class=\"sub\">" + escape(subtitle) + "</p>\n"
           + body + "</main>\n</body>\n</html>\n";
}

// A section of entries with its own index at the top, anchors named by section so a function and a
// callback of the same name cannot collide.
std::string section(const char* heading, const char* idPrefix, const std::vector<MetaModel::LuaApiEntry>& list) {
    if (list.empty()) return {};
    std::string nav, items;
    for (const auto& e : list) {
        const std::string id = std::string(idPrefix) + "-" + escape(e.name);
        nav   += "<a href=\"#" + id + "\">" + escape(e.name) + "</a>\n";
        items += "<div class=\"entry\" id=\"" + id + "\"><code class=\"sig\">"
               + escape(e.sig.empty() ? e.name : e.sig) + "</code>";
        if (!e.doc.empty()) items += "<p>" + escape(e.doc) + "</p>";
        items += "</div>\n";
    }
    return std::string("<h2>") + heading + "</h2>\n<nav>\n" + nav + "</nav>\n" + items;
}

} // namespace

std::string luaReferenceHtml(const MetaModel& meta) {
    std::string body = section("Functions", "fn", meta.luaFunctions())
                     + section("Callbacks", "cb", meta.luaCallbacks());
    if (body.empty())
        body = "<p class=\"empty\">This ECU's definition declares no Lua API.</p>\n";
    std::string subtitle = meta.product();
    if (!meta.fwVersion().empty()) subtitle += (subtitle.empty() ? "" : " \xC2\xB7 ") + meta.fwVersion();
    if (!meta.layoutHash().empty()) subtitle += " \xC2\xB7 layout " + meta.layoutHash();
    return page("Lua API Reference", subtitle, body);
}

bool openLuaReference(const MetaModel& meta, std::string& error) {
    const std::string path = StudioPaths::dataDir("help") + "/lua-api.html";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) { error = "cannot write " + path; return false; }
        f << luaReferenceHtml(meta);
        if (!f) { error = "writing " + path + " failed"; return false; }
    }
    return openPage("help/lua-api.html", error);
}

bool openUserManual(std::string& error) { return openPage("manual/index.html", error); }

std::string pageUrl(const std::string& path, std::string& error) {
    if (path.rfind("manual/", 0) == 0 && manualDir().empty()) {
        error = "the user manual is not installed with this studio";
        return {};
    }
    jf::JLocalWebServer* web = server(error);
    return web ? web->url(path) : std::string();
}

namespace {
bool openPage(const std::string& path, std::string& error) {
    const std::string url = pageUrl(path, error);
    if (url.empty()) return false;
    if (!jf::JDesktop::openUrl(url)) {
        error = "no browser could be opened \xE2\x80\x94 the page is at " + url;
        return false;
    }
    return true;
}
}

} // namespace helppages
