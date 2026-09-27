#pragma once

// HelpPages — the studio's help, as HTML pages opened in the user's own browser.
//
// A browser is the right reader for reference text: it scrolls, searches (Ctrl+F), zooms, prints,
// keeps its place and can sit beside the studio on another screen. A modal text box inside the studio
// did none of that, and its scroll bar did not even drag.
//
// Pages are GENERATED from the loaded definition rather than shipped, so a page always describes the
// firmware in front of the studio -- the Lua reference lists the functions THIS firmware exports.
// They are written to the studio's data folder (help/) and handed to the desktop (JDesktop::openUrl).

#include <string>

class MetaModel;

namespace helppages {

// The Lua API reference for this definition, as a complete, self-contained HTML document.
std::string luaReferenceHtml(const MetaModel& meta);

// Write it to help/lua-api.html and open it in the browser. On failure returns false with `error`
// saying why (and, when the file was written but no browser could be asked, where it is).
bool openLuaReference(const MetaModel& meta, std::string& error);

// THE USER MANUAL, a built MkDocs site. It travels beside the studio (manual/ next to the executable,
// put there by the AppImage and the Windows installer); a studio run from its build tree finds the one
// `make manual` built in the repository instead. Opens its index in the browser; false with `error`
// when neither copy is there.
bool openUserManual(std::string& error);

// The address a page is served at ("manual/index.html", "help/lua-api.html"), starting the local server
// if it is not running — without opening a browser. "" with `error` when it cannot be served.
std::string pageUrl(const std::string& path, std::string& error);

// Text as HTML: & < > " ' escaped. Exposed for the test.
std::string escape(const std::string& text);

} // namespace helppages
