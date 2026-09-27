#pragma once

// NewTuneDialog — the studio's "New Tune" dialog: name a tune and pick the firmware SCHEMA (meta) from the
// pool that seeds its default values. This is an APPLICATION dialog composed from JFramework primitives
// (JLineEdit, JListView, JTitleBar, JCloseButton, JTextHelper) — NOT a framework widget. A Tune Name
// field over a Schema list whose rows read
// "board  version · hash · date", a description line for the current selection, and a Create button gated
// on both a name AND a valid schema. onAccept(tuneName, metaPath) — the caller loads that schema.

#include <j/core/JStyle.h>
#include <j/core/JTitleBar.h>
#include <j/core/JDialogButtonBox.h>
#include <j/core/FocusManager.h>
#include <j/core/JCloseButton.h>
#include <j/core/JTextHelper.h>
#include <j/core/JLineEdit.h>
#include <j/core/JListView.h>
#include <j/core/Dialog.h>          // jf::JDialog::openFile — the framework file picker for "Browse…"
#include <j/config/Json.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>
#include "WrapText.h"

class NewTuneDialog {
public:
    static constexpr uint32_t kW = 500, kH = 480;
    static constexpr float kBtnW = 84.f, kPad = 12.f, kRowH = 22.f, kSecH = 18.f;
    static float kBtnH() { return jf::JStyle::current().buttonHeight; }   // HEIGHT from JStyle (single source of truth)
    static constexpr const char* kBrowseRow = "Browse for a schema file\xE2\x80\xA6";   // last list row (→ file picker)
    static float kHeader() { return jf::JStyle::current().titleBarHeight; }
    static float kBtnY()   { return (kHeader() - kBtnH()) * 0.5f; }
    static jf::JRect _closeRect() { return jf::JCloseButton::rectFor({0.f, 0.f, static_cast<float>(kW), kHeader()}); }
    static bool _inRect(const jf::JRect& r, float mx, float my) {
        return mx >= r.x && mx < r.x + r.width && my >= r.y && my < r.y + r.height;
    }

#if defined(_WIN32)
    using PlatformWinType     = jf::JWindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType     = jf::JLinuxPlatformWindow;
    using NativeWinHandleType = xcb_window_t;
#endif

    // One decoded schema from the meta pool — its path plus the fields the rows/description show.
    struct Schema { std::string path, board, version, hash, label, desc; std::time_t mtime{0}; };

    // Extract the JSON substring of object member `key` (its {...}) from `src`; false if absent/unclosed.
    // Brace-matched and string-aware, so it survives braces inside quoted values. Lets us parse just the
    // small "meta" header instead of a ~0.5 MB config file.
    static bool _extractObject(const std::string& src, const std::string& key, std::string& out) {
        const std::string tag = "\"" + key + "\"";
        const size_t k = src.find(tag);
        if (k == std::string::npos) return false;
        const size_t b = src.find('{', k + tag.size());
        if (b == std::string::npos) return false;
        int depth = 0; bool inStr = false, esc = false;
        for (size_t i = b; i < src.size(); ++i) {
            const char c = src[i];
            if (inStr) { if (esc) esc = false; else if (c == '\\') esc = true; else if (c == '"') inStr = false; }
            else if (c == '"') inStr = true;
            else if (c == '{') ++depth;
            else if (c == '}') { if (--depth == 0) { out = src.substr(b, i - b + 1); return true; } }
        }
        return false;
    }

    // Decode ONE .meta file into a Schema row. The identity we need (board/version/hash) is in the small
    // "meta" object at the TOP of the file. A .meta is ~0.5 MB (the whole config+telemetry+protocol); parsing
    // it WHOLE is what made New Tune take seconds to open — so read a short prefix and JSON-parse ONLY the
    // extracted "meta" object. Shared by scanPool (the library) and the "Browse…" picker (an arbitrary file).
    static bool decodeMeta(const std::string& path, Schema& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        char head[8192];
        f.read(head, sizeof(head));
        const std::string prefix(head, static_cast<size_t>(f.gcount()));
        std::string metaObj;
        if (!_extractObject(prefix, "meta", metaObj)) return false;
        auto parsed = jf::JJson::tryParse(metaObj);
        if (!parsed || !parsed->isObject()) return false;
        const jf::JJson& m = *parsed;
        const std::string board = m["board"].str(), ver = m["fw_version"].str(), hash = m["layout_hash"].str();
        if (board.empty() && hash.empty()) return false;   // not a schema meta
        const std::string h8 = hash.substr(0, 8), date = _mtime(path);
        std::time_t mt = 0;
        { struct stat st; if (stat(path.c_str(), &st) == 0) mt = st.st_mtime; }
        out = Schema{ path, board, ver, hash,
                      board + "  " + ver + "  \xC2\xB7  " + h8 + (date.empty() ? "" : "  \xC2\xB7  " + date),
                      board + " " + ver + "  \xC2\xB7  layout " + h8, mt };
        return true;
    }

    // Scan a meta-library directory, decoding each *.meta into a Schema row. Static + window-free so it can be
    // unit-tested. Rows sort NEWEST FIRST by file mtime (label breaks ties).
    static std::vector<Schema> scanPool(const std::string& dir) {
        namespace fs = std::filesystem;
        std::vector<Schema> out;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.path().extension() != ".meta") continue;
            Schema s;
            if (decodeMeta(e.path().string(), s)) out.push_back(std::move(s));
        }
        // Newest first: the schema you just imported or regenerated is the one you want, and a pool
        // accumulates many builds of the same board. Label breaks ties so the order stays stable.
        std::sort(out.begin(), out.end(), [](const Schema& a, const Schema& b) {
            if (a.mtime != b.mtime) return a.mtime > b.mtime;
            return a.label < b.label;
        });
        return out;
    }

    // (metaLibDir, onAccept) lead; hal/pos/handle tail from openModal. onAccept(tuneName, metaPath).
    NewTuneDialog(std::string metaLibDir, std::function<void(std::string, std::string)> onAccept,
                  jf::JGpuHal& hal, int sx, int sy, NativeWinHandleType parent)
        : m_onAccept(std::move(onAccept))
        , m_schemas(scanPool(metaLibDir))
        , m_window(std::make_unique<PlatformWinType>("New Tune", kW, kH, sx, sy, jf::JPlatformWindowStyle::Borderless, parent))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, kH)) {
        using namespace jf;
        m_name = std::make_unique<JLineEdit>(m_graph, "e.g. my nice supra, honda shitbox tune");
        // Return in the name field is the dialog's default button, as in any dialog: create the tune.
        m_name->onReturnPressed.connect([this] { if (_canCreate()) _accept(); });
        m_list = std::make_unique<JListView>(m_graph, _rows());
        if (!m_schemas.empty()) m_list->setSelectedIndex(0);   // a real schema is selected; the last row is "Browse…"
    }

    // Standard dialog footer: roles, not hand-drawn rectangles. The box owns order (JDialogOptions::
    // okOnRight), placement, Return/Escape and focus; "Create" stays the label because a verb beats a
    // generic OK. Previously these were pushRectangle + a hit-test that fired on mouse-DOWN.
    void _ensureButtons() {
        if (m_box) return;
        m_box = std::make_unique<jf::JDialogButtonBox>(m_graph);
        m_box->addButton("Cancel", jf::JDialogButtonBox::Role::Reject, kBtnW);
        m_create = m_box->addButton("Create", jf::JDialogButtonBox::Role::Accept, kBtnW);
        m_box->onReject.connect([this] { m_done = true; });
        m_box->onAccept.connect([this] { if (_canCreate()) _accept(); });
    }

    void destroySurface(jf::JGpuHal& hal) { hal.destroySurface(m_surface); }

    bool pollAndRender(jf::JGpuHal& hal, jf::JPrimitiveBuffer& buf) {
        using namespace jf;
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) return false;
        const float mx = m_window->mouseX(), my = m_window->mouseY();
        const bool pressed = m_window->consumePress(), released = m_window->consumeRelease(), held = m_window->isLeftButtonDown();

        const JRect closeR = _closeRect();
        if (pressed && _inRect(closeR, mx, my)) return false;
        const bool inTitle = (my >= 0.f && my < kHeader() && mx < closeR.x);
        if (held && inTitle && !m_drag) { m_drag = true; m_ax = mx; m_ay = my; }
        if (m_drag) { auto [gx, gy] = m_window->globalCursorPos(); m_window->setPosition(gx - int(m_ax), gy - int(m_ay)); }
        if (!held) m_drag = false;

        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            if (ke.key == JKeyEvent::JKey::Escape) return false;
            // Standard framework routing: the focused control first, then Tab / Shift-Tab traversal,
            // scoped to this dialog's own tree (name field, schema list, footer buttons).
            _refreshFocusRoots();
            if (jf::jRouteKey(ke, m_focus)) { if (m_done) return false; continue; }
            if (m_box && m_box->handleKeyEvent(ke)) { if (m_done) return false; continue; }   // Return/Escape
            if (ke.key == JKeyEvent::JKey::Return) { if (_canCreate()) { _accept(); return false; } continue; }
            m_name->handleKeyEvent(ke);
        }

        // Layout: [Tune Name section] name field | [Schema section] list | description line | buttons.
        float y = kHeader() + kPad;
        const float innerW = static_cast<float>(kW) - 2.f * kPad;
        y += kSecH;                                   // "Tune Name" section header
        m_name->setBounds({ kPad, y, innerW, kRowH }); y += kRowH + kPad;
        y += kSecH;                                   // "Schema" section header
        const float descH = _descH(), listBottom = static_cast<float>(kH) - kPad - kBtnH() - kPad - descH - 4.f;
        m_list->setBounds({ kPad, y, innerW, listBottom - y });

        m_name->handleMouseMove(mx, my);
        m_list->handleMouseMove(mx, my);
        if (pressed)  { m_name->handleMousePress(mx, my);   m_list->handleMousePress(mx, my); }
        if (released) { m_name->handleMouseRelease(mx, my); m_list->handleMouseRelease(mx, my); }

        // Selecting the last row ("Browse for a schema file…") opens the framework file picker (like the
        // original's onBrowse). The async pick appends the chosen schema and selects it; cancel reverts.
        if (m_list->selectedIndex() == static_cast<int>(m_schemas.size()) && !m_browseOpen) {
            m_browseOpen = true;
            jf::JDialog::openFile("Select a schema file", {"meta", "json"},
                [this](std::string path) { _onBrowsed(path); },
                [this]() { m_browseOpen = false; m_list->setSelectedIndex(0); });   // cancel → back to the first schema
        }

        // The footer owns its buttons: hover, arm/cancel, click-on-release, focus ring.
        _ensureButtons();
        m_box->setBounds({ kPad, static_cast<float>(kH) - kPad - kBtnH(),
                           static_cast<float>(kW) - 2.f * kPad, kBtnH() });   // bottom-right, the standard place
        // Gate Create on name + schema, but only ON CHANGE -- see JWidget::setEnabled.
        if (m_create) { const bool can = _canCreate(); if (m_create->isEnabled() != can) m_create->setEnabled(can); }
        _refreshFocusRoots();
        if (pressed) jf::jRouteMouse(mx, my, m_focus);          // clicking a control focuses it
        // The keyboard starts in the NAME field, so typing a name and pressing Return creates the tune.
        // Not focusFirst(): on the first frame the footer has not laid its buttons out yet, they sit at
        // (0,0), and reading order put Cancel first — so Return cancelled.
        if (!m_focusSeeded) { m_focusSeeded = true; m_focus.syncOrder(); m_focus.setFocus(m_name.get()); }
        m_box->handleMouseMove(mx, my);
        if (pressed)  m_box->handleMousePress(mx, my);
        if (released) m_box->handleMouseRelease(mx, my);
        if (m_done) return false;

        _render(buf, mx, my);
        auto frame = hal.beginFrame(m_surface); hal.drawPrimitives(buf); hal.submitAndPresentFrame(frame);
        return true;
    }

private:
    static std::string _mtime(const std::string& p) {
        struct stat st;
        if (stat(p.c_str(), &st) != 0) return {};
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &st.st_mtime);
#else
        localtime_r(&st.st_mtime, &tm);
#endif
        char b[32];
        std::strftime(b, sizeof(b), "%d %b %Y %H:%M", &tm);   // date AND time — several schemas can share a day
        // strip a leading zero on the day ("03 Jul" -> "3 Jul").
        std::string s(b);
        if (s.size() > 1 && s[0] == '0') s.erase(0, 1);
        return s;
    }
    // A real schema (not the trailing "Browse…" row) is selected AND a name is entered.
    bool _canCreate() const {
        const int i = m_list->selectedIndex();
        return i >= 0 && i < static_cast<int>(m_schemas.size()) && !m_name->text().empty();
    }
    // Accepting ENDS the dialog, whichever way it came (the Create button, or Return through the box):
    // the box's onAccept only called this, so a created tune left the dialog open behind it.
    void _accept() {
        const int i = m_list->selectedIndex();
        if (i >= 0 && i < static_cast<int>(m_schemas.size()) && m_onAccept)
            m_onAccept(m_name->text(), m_schemas[i].path);
        m_done = true;
    }
    // The list rows: every decoded schema, then the "Browse…" affordance as the last row.
    std::vector<std::string> _rows() const {
        std::vector<std::string> rows;
        for (const auto& s : m_schemas) rows.push_back(s.label);
        rows.push_back(kBrowseRow);
        return rows;
    }
    // A schema was chosen through the file picker: decode it, add it (dedup by path), and select it.
    void _onBrowsed(const std::string& path) {
        m_browseOpen = false;
        Schema s;
        if (path.empty() || !decodeMeta(path, s)) { m_list->setSelectedIndex(0); return; }   // invalid → revert
        for (size_t i = 0; i < m_schemas.size(); ++i)
            if (m_schemas[i].path == s.path) { m_list->setSelectedIndex(static_cast<int>(i)); return; }
        m_schemas.push_back(std::move(s));
        m_list->setItems(_rows());
        m_list->setSelectedIndex(static_cast<int>(m_schemas.size()) - 1);
    }
    // The selected schema's description, which WRAPS under the list; the list ends where it begins.
    std::string _desc() const {
        const int sel = m_list ? m_list->selectedIndex() : -1;
        return (sel >= 0 && sel < static_cast<int>(m_schemas.size()))
               ? m_schemas[size_t(sel)].desc
               : std::string(m_schemas.empty() ? "No schemas in the meta library" : "");
    }
    float _descH() const {
        return kRowH + wraptext::extra(_desc(), static_cast<float>(kW) - 2.f * kPad);
    }
    void _section(jf::JPrimitiveBuffer& buf, float x, float y, const char* text) {
        using namespace jf;
        if (JTextHelper::hasAtlas())
            JTextHelper::pushText(buf, x, y, text, Colors::TextSecondary);
    }
    void _render(jf::JPrimitiveBuffer& buf, float mx, float my) {
        using namespace jf;
        buf.clear();
        const float W = static_cast<float>(kW), H = static_cast<float>(kH), lh = JTextHelper::lineHeight();
        const float r = JStyle::current().cornerRadius;
        buf.pushRectangle(0.f, 0.f, W, H, Colors::Surface1, r, 1.f, Colors::Border);
        JTitleBar::draw(buf, 0.f, 0.f, W, kHeader(), "New Tune", r, 1, 0.f, _closeRect().width + 14.f);

        // Section headers for the two groups.
        float y = kHeader() + kPad;
        _section(buf, kPad, y, "Tune Name"); y += kSecH + kRowH + kPad;
        _section(buf, kPad, y, "Schema (ECU firmware layout)");

        // Description line for the current schema (below the list), in grey.
        const float descY = H - kPad - kBtnH() - kPad - _descH();
        if (JTextHelper::hasAtlas())
            wraptext::draw(buf, kPad, descY + (kRowH - lh) * 0.5f, _desc(), Colors::TextSecondary, W - 2.f * kPad);

        // Footer — the button box paints itself (order, states, focus ring). Create is greyed via
        // setEnabled() above rather than by hand-mixing an alpha.
        if (m_box) m_box->populateRenderPrimitives(buf);
        const JRect cr = _closeRect();
        JCloseButton::draw(buf, cr, _inRect(cr, mx, my));

        m_name->populateRenderPrimitives(buf);
        m_list->populateRenderPrimitives(buf);
    }

    std::function<void(std::string, std::string)> m_onAccept;
    std::vector<Schema> m_schemas;
    std::unique_ptr<PlatformWinType> m_window;
    jf::GpuSurfaceId m_surface{0};
    jf::JSceneGraph  m_graph;
    std::unique_ptr<jf::JLineEdit> m_name;
    std::unique_ptr<jf::JListView> m_list;
    // This dialog's focus tree: the name field, the schema list and the footer. Declared because JWidget
    // takes a scene graph rather than a parent, so members are not discoverable on their own.
    void _refreshFocusRoots() {
        std::vector<jf::JWidget*> roots;
        if (m_name) roots.push_back(m_name.get());
        if (m_list) roots.push_back(m_list.get());
        if (m_box)  roots.push_back(m_box.get());
        m_focus.setFocusRoots(std::move(roots));
    }

    jf::JFocusManager                     m_focus;     // this dialog's keyboard focus
    bool                                  m_focusSeeded{false};
    std::unique_ptr<jf::JDialogButtonBox> m_box;        // the standard footer (roles, order, Return/Escape)
    jf::JButton*                          m_create{nullptr};
    bool m_done{false};                                 // set by Cancel or Create; ends pollAndRender
    bool m_browseOpen{false};   // a "Browse…" file picker is currently open (don't re-trigger each frame)
    bool m_drag{false};
    float m_ax{0}, m_ay{0};
};
