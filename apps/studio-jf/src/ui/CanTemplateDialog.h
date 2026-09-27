#pragma once
//
// CanTemplateDialog — picking a published protocol, as a table rather than a sentence.
//
// The list picker this replaces stated every fact about a template inside one string: name, then a
// dot, then a frame count, then a signal count, then a bracketed direction, then — once the bit rate
// started being checked — why it could not be loaded. Seven facts in a line that had to be read left
// to right to find any of them, in a 420px window that could not be widened, with no way to put the
// 1 Mbit protocols next to each other or see at a glance which ones this bus can actually carry.
//
// They are columns. A column can be sorted, it can be widened when a name is long, and a number in
// one reads as a number instead of as more prose. The dialog is resizable because the answer to "the
// name is cut off" should be dragging the edge, not a wider constant in the source.
//
// It carries no knowledge of CAN. The caller builds the rows, says which can be chosen and why not,
// and gets back an index into the vector it passed — which is what lets Load and Remove, two
// different questions about the same library, be the same dialog.
//
#include <j/app/JDialogWindow.h>
#include <j/core/JDataGrid.h>
#include <j/core/JDialogButtonBox.h>
#include <j/core/JStyle.h>
#include <j/core/JTextHelper.h>
#include <j/graphics/RenderPrimitive.h>
#include "WrapText.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class CanTemplateDialog : public jf::JDialogWindow {
public:
    // openModal reads these statically for the initial size; the window grows from here.
    static constexpr uint32_t kW = 860, kH = 460;
    static constexpr uint32_t kMinW = 560, kMinH = 300;
    static constexpr float kPad = 12.f;

    // One row. `why` is the long form of whatever the Status column says in two words — it goes in
    // the footer when the row is selected, so the table stays narrow and the explanation stays whole.
    struct Entry {
        std::vector<std::string> cells;
        bool                     enabled = true;
        std::string              why;
    };

    struct Column {
        std::string                  title;
        float                        width = 90.f;
        jf::JDataGrid::ColAlign      align = jf::JDataGrid::ColAlign::Left;
    };

    CanTemplateDialog(std::string title, std::string acceptLabel,
                      std::vector<Column> cols, std::vector<Entry> entries,
                      std::function<void(int)> onChoose,
                      jf::JGpuHal& hal, int sx, int sy, NativeWinHandleType parent)
        : jf::JDialogWindow(std::move(title), kW, kH, hal, sx, sy, parent)
        , m_entries(std::move(entries)), m_onChoose(std::move(onChoose)) {
        using namespace jf;
        setResizable(true, kMinW, kMinH);

        std::vector<std::string> heads;
        std::vector<float>       widths;
        std::vector<JDataGrid::ColAlign> aligns;
        for (const Column& c : cols) {
            heads.push_back(c.title); widths.push_back(c.width); aligns.push_back(c.align);
        }

        m_grid = std::make_unique<JDataGrid>(graph(), heads);
        m_grid->setColumnWidths(widths);
        m_grid->setColumnAlignments(std::move(aligns));
        m_grid->setSortable(true);            // put the 1 Mbit ones together, or the receives
        m_grid->setColumnsResizable(true);    // a long protocol name is a drag, not a rebuild

        std::vector<std::vector<std::string>> rows;
        std::vector<uint8_t>                  flags;
        rows.reserve(m_entries.size()); flags.reserve(m_entries.size());
        for (const Entry& e : m_entries) { rows.push_back(e.cells); flags.push_back(e.enabled ? 1 : 0); }
        m_grid->setRows(rows);
        m_grid->setEnabledFlags(std::move(flags));

        // Double-click or Return on a row IS the choice — the accept button is for people who would
        // rather press a button, not the only way through.
        m_grid->onRowActivated.connect([this](int r) { _choose(r); });
        m_grid->onSelectionChanged.connect([this](int) { _syncAccept(); });

        m_box = std::make_unique<JDialogButtonBox>(graph());
        m_box->addButton("Cancel", JDialogButtonBox::Role::Reject, 96.f);
        m_accept = m_box->addButton(acceptLabel.empty() ? std::string("Load") : acceptLabel,
                                    JDialogButtonBox::Role::Accept, 96.f);
        m_box->onReject.connect([this] { close(); });
        m_box->onAccept.connect([this] { _choose(m_grid->selectedIndex()); });

        add(m_grid.get());
        add(m_box.get());

        m_grid->setSelectedIndex(0);          // lands on the first row that can be chosen, or nowhere
        _syncAccept();
    }

protected:
    void layout(float w, float h) override {
        const float r = rowH();
        const float footH = r + kPad + wraptext::extra(_why(), w - 2.f * kPad);   // the why line above the buttons, wrapped
        const float gridY = contentTop();
        const float gridH = h - gridY - footH - r - 2.f * kPad;
        m_grid->setBounds({ kPad, gridY, w - 2.f * kPad, (gridH > 60.f) ? gridH : 60.f });
        m_box->setBounds({ kPad, h - kPad - r, w - 2.f * kPad, r });
    }

    void paint(jf::JPrimitiveBuffer& buf, float w, float h) override {
        using namespace jf;
        if (!JTextHelper::hasAtlas()) return;
        // WHY THE SELECTED ROW CANNOT BE LOADED, in full. The Status column has room for "1 Mbit
        // bus" and no more; the sentence that names both rates belongs somewhere it can be read.
        const int  row = m_grid->selectedIndex();
        const std::string line = _why();
        if (line.empty()) return;
        const bool bad = (row >= 0 && row < (int)m_entries.size() && !m_entries[(size_t)row].enabled);
        const JColor c = bad ? jstyle::pal().color(JColorRole::Text, JColorGroup::Disabled)
                             : jstyle::pal().color(JColorRole::Text, JColorGroup::Active);
        uint8_t tc[4] = { c.r, c.g, c.b, 220 };
        // Wrapped, and raised by its extra lines — layout() took them off the grid.
        wraptext::draw(buf, kPad, h - kPad - 2.f * rowH() - 4.f - wraptext::extra(line, w - 2.f * kPad),
                       line, tc, w - 2.f * kPad);
    }

    std::string _why() const {
        const int row = m_grid ? m_grid->selectedIndex() : -1;
        if (row >= 0 && row < (int)m_entries.size()) return m_entries[(size_t)row].why;
        if (m_grid && !m_grid->anyRowEnabled())       return m_note;
        return {};
    }

public:
    // Shown under the table when nothing at all can be chosen — the one case where the reason is
    // about the BUS rather than about any single row.
    void setEmptyNote(std::string n) { m_note = std::move(n); }

private:
    void _choose(int row) {
        if (row < 0 || row >= (int)m_entries.size() || !m_entries[(size_t)row].enabled) return;
        if (m_onChoose) m_onChoose(row);
        close();
    }
    void _syncAccept() {
        const int r = m_grid->selectedIndex();
        const bool ok = (r >= 0 && r < (int)m_entries.size() && m_entries[(size_t)r].enabled);
        if (m_accept) m_accept->setEnabled(ok);
    }

    std::vector<Entry>                        m_entries;
    std::function<void(int)>                  m_onChoose;
    std::string                               m_note;
    std::unique_ptr<jf::JDataGrid>            m_grid;
    std::unique_ptr<jf::JDialogButtonBox>     m_box;
    jf::JButton*                              m_accept = nullptr;
};
