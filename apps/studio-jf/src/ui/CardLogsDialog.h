#pragma once
//
// CardLogsDialog — the ECU's logs, brought across from the card.
//
// It opens on whatever the card has, because the card is a folder: at key-off the ECU hands its SD
// to USB on its own, the OS mounts it, and from here it is an ordinary directory listing. No
// protocol, no ownership dance, no window where both ends think they hold the card.
//
// The list says which logs are ALREADY IN the studio's log folder, so "bring across the ones I have
// not got" is one press rather than a memory exercise. That state is read from the disk each time
// rather than from a manifest — a manifest is a second copy of the truth, and this one would be
// wrong the moment somebody moved a file.
//
// DELETE REFUSES A LOG THAT HAS NOT BEEN IMPORTED unless it is confirmed, because until it is
// imported the card is the only copy of that drive.
//
#include <j/app/JDialogWindow.h>
#include <j/core/JStyle.h>
#include <j/core/JButton.h>
#include <j/core/JCheckBox.h>
#include <j/core/JDialogButtonBox.h>
#include <j/core/JTreeView.h>
#include <j/core/JTextHelper.h>
#include <j/core/Dialog.h>
#include <j/graphics/RenderPrimitive.h>

#include "../model/CardLogs.h"
#include "WrapText.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

class CardLogsDialog : public jf::JDialogWindow {
public:
    static constexpr uint32_t kW = 720, kH = 560;
    static constexpr float kPad = 14.f, kBtnW = 118.f;

    CardLogsDialog(jf::JGpuHal& hal, int sx, int sy, NativeWinHandleType parent)
        : jf::JDialogWindow("Logs on Card", kW, kH, hal, sx, sy, parent) {

        m_tree = std::make_unique<jf::JTreeView>(graph());
        m_tree->onNodeChecked.connect([this](jf::JTreeViewNode* n) { _toggle(n); });
        m_tree->onNodeActivated.connect([this](jf::JTreeViewNode* n) { _toggle(n); });

        m_importSel = std::make_unique<jf::JButton>(graph(), "Import ticked", kBtnW, rowH());
        m_importNew = std::make_unique<jf::JButton>(graph(), "Import all new", kBtnW, rowH());
        m_erase     = std::make_unique<jf::JButton>(graph(), "Delete ticked", kBtnW, rowH());
        m_locate    = std::make_unique<jf::JButton>(graph(), "Find card\xE2\x80\xA6", kBtnW, rowH());
        m_importSel->onClicked.connect([this] { _import(false); });
        m_importNew->onClicked.connect([this] { _import(true);  });
        m_erase->onClicked.connect    ([this] { _erase();       });
        m_locate->onClicked.connect   ([this] { _locate();      });

        m_box = std::make_unique<jf::JDialogButtonBox>(graph());
        m_box->addButton("Close", jf::JDialogButtonBox::Role::Reject, 100.f);
        m_box->onReject.connect([this] { close(); });

        add(m_tree.get());
        add(m_importSel.get()); add(m_importNew.get()); add(m_erase.get()); add(m_locate.get());
        add(m_box.get());
        _rescan();
    }

protected:
    void layout(float w, float h) override {
        const float r = rowH();
        _pollForCard();

        // WHAT NEEDS A CARD, needs a card. Find card… never does — it is the way OUT of having no
        // card on a machine that does not automount, and disabling it would close the only door.
        const bool have = !m_card.empty();
        m_importSel->setEnabled(have);
        m_importNew->setEnabled(have);
        m_erase->setEnabled(have);
        // room for the card's own line, which WRAPS — as many lines as it takes
        const float treeY = contentTop() + r + 10.f + wraptext::extra(_line(), w - 2.f * kPad);
        const float treeH = h - treeY - (2.f * r + 3.f * kPad);
        m_tree->setBounds({ kPad, treeY, w - 2.f * kPad, treeH });
        float x = kPad;
        for (jf::JButton* b : { m_importNew.get(), m_importSel.get(),
                                m_erase.get(), m_locate.get() }) {
            b->setBounds({ x, h - kPad - 2.f * r - 8.f, kBtnW, r });
            x += kBtnW + 8.f;
        }
        m_box->setBounds({ kPad, h - kPad - r, w - 2.f * kPad, r });
    }

    void paint(jf::JPrimitiveBuffer& buf, float w, float h) override {
        if (!jf::JTextHelper::hasAtlas()) return;
        (void)h;
        wraptext::draw(buf, kPad, contentTop() + 4.f, _line(),
                       m_note.empty() ? jf::Colors::TextSecondary : jf::Colors::TextPrimary, w - 2.f * kPad);
    }

    // The card's own line: what the last action did, else what is on the card.
    std::string _line() const {
        if (!m_note.empty())     return m_note;
        if (m_card.empty())      return "No card found \xE2\x80\x94 turn the key off and give it a moment, or Find card\xE2\x80\xA6";
        return m_card + "  \xE2\x80\x94  " + std::to_string(m_logs.size()) +
               " log" + (m_logs.size() == 1 ? "" : "s") + ", " +
               std::to_string(_newCount()) + " not yet imported";
    }

private:
    static std::string _size(uint64_t b) {
        char s[32];
        if (b >= (1ull << 20)) std::snprintf(s, sizeof s, "%.1f MB", double(b) / double(1ull << 20));
        else                   std::snprintf(s, sizeof s, "%.0f KB", double(b) / 1024.0);
        return s;
    }
    static std::string _when(int64_t epoch) {
        if (epoch <= 0) return "date unknown";
        const std::time_t t = static_cast<std::time_t>(epoch);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char s[32];
        std::strftime(s, sizeof s, "%Y-%m-%d %H:%M", &tm);
        return s;
    }

    size_t _newCount() const {
        size_t n = 0;
        for (const auto& e : m_logs) if (!e.imported) ++n;
        return n;
    }

    // THE CARD TURNS UP WHILE THIS IS OPEN. That is the actual workflow — finish driving, key off,
    // the ECU hands its SD to USB and the machine mounts it — so the dialog watches instead of
    // making you close and reopen it. Only when the PATH changes, because a rescan resets the ticks
    // and that is only harmless when the card itself has come or gone.
    void _pollForCard() {
        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastPoll < std::chrono::milliseconds(1500)) return;
        m_lastPoll = now;
        if (CardLogs::findCard() == m_card) return;
        m_note.clear();
        _rescan();
    }

    void _rescan() {
        m_card = CardLogs::findCard();
        m_logs = CardLogs::logsOn(m_card);
        m_ticked.assign(m_logs.size(), false);
        _rebuild();
    }

    void _rebuild() {
        jf::JTreeViewNode root;
        for (size_t i = 0; i < m_logs.size(); ++i) {
            const CardLogs::Entry& e = m_logs[i];
            jf::JTreeViewNode n;
            n.label = e.name + "   " + _when(e.epoch) + "   " + _size(e.bytes) +
                      (e.imported ? "   \xE2\x80\x94 imported" : "");
            n.userData  = std::to_string(i);
            n.checkable = true;
            n.checked   = m_ticked[i];
            root.children.push_back(std::move(n));
        }
        m_tree->setRootNode(std::move(root));
    }

    void _toggle(jf::JTreeViewNode* n) {
        if (!n || n->userData.empty()) return;
        const size_t i = static_cast<size_t>(std::atoi(n->userData.c_str()));
        if (i < m_ticked.size()) m_ticked[i] = !m_ticked[i];
        m_note.clear();
        _rebuild();
    }

    void _import(bool allNew) {
        size_t done = 0, failed = 0;
        std::string firstErr;
        for (size_t i = 0; i < m_logs.size(); ++i) {
            const bool want = allNew ? !m_logs[i].imported : m_ticked[i];
            if (!want || m_logs[i].imported) continue;
            std::string why;
            if (CardLogs::import(m_logs[i], &why)) ++done;
            else { ++failed; if (firstErr.empty()) firstErr = why; }
        }
        if (done == 0 && failed == 0) m_note = "Nothing to import.";
        else if (failed)              m_note = "Imported " + std::to_string(done) +
                                               ", failed " + std::to_string(failed) + ": " + firstErr;
        else                          m_note = "Imported " + std::to_string(done) + " into " +
                                               CardLogs::importDirectory();
        const std::string keep = m_note;
        _rescan();
        m_note = keep;
    }

    void _erase() {
        std::vector<size_t> pick;
        bool anyUnimported = false;
        for (size_t i = 0; i < m_logs.size(); ++i)
            if (m_ticked[i]) { pick.push_back(i); if (!m_logs[i].imported) anyUnimported = true; }
        if (pick.empty()) { m_note = "Tick the logs to delete first."; return; }

        // UNIMPORTED MEANS THE CARD IS THE ONLY COPY. That is worth one question, and the question
        // says how many rather than making it sound routine.
        if (anyUnimported) {
            jf::JDialog::confirm("Delete from card?",
                "Some of the ticked logs have not been imported.\n\n"
                "Deleting them removes the only copy. Continue?",
                [this, pick] { _eraseNow(pick); });
            return;
        }
        _eraseNow(pick);
    }

    void _eraseNow(const std::vector<size_t>& pick) {
        size_t gone = 0;
        std::string firstErr;
        for (const size_t i : pick) {
            std::string why;
            if (CardLogs::erase(m_logs[i], true, &why)) ++gone;
            else if (firstErr.empty()) firstErr = why;
        }
        const std::string note = firstErr.empty()
            ? "Deleted " + std::to_string(gone) + " from the card."
            : "Deleted " + std::to_string(gone) + "; " + firstErr;
        _rescan();
        m_note = note;
    }

    // For the machine with no automounter, or a card the search cannot recognise. Remembered, so it
    // is asked once rather than every session.
    void _locate() {
        jf::JDialog::openFolder("Where is the card?", [this](std::string dir) {
            if (dir.empty()) return;
            if (!CardLogs::looksLikeCard(dir)) { m_note = "No .MLG logs in " + dir; return; }
            CardLogs::rememberPath(dir);
            _rescan();
            m_note = "Using " + dir;
        });
    }

    std::unique_ptr<jf::JTreeView>        m_tree;
    std::unique_ptr<jf::JButton>          m_importSel, m_importNew, m_erase, m_locate;
    std::unique_ptr<jf::JDialogButtonBox> m_box;

    std::string                   m_card, m_note;
    std::chrono::steady_clock::time_point m_lastPoll{};
    std::vector<CardLogs::Entry>  m_logs;
    std::vector<bool>             m_ticked;
};
