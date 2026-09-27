// CanFieldWidget — the list of receive fields a CAN sensor may read, and the pair of settings a pick
// writes. The base does the paint and the input-forward; this supplies the control and the sync.

#include "CanFieldWidget.h"

#include <cmath>
#include <cstdio>
#include "../../model/Cache.h"
#include "../../model/MetaModel.h"

namespace {

// Frame flags, as the firmware reads them (Can/CanMessageTypes.h) and as GenericCanPanel writes them.
enum : int { M_USED = 1 << 0, M_TX = 1 << 1, M_EXT = 1 << 2 };
// No channel. 65535 is the firmware's SIG_NONE; it cannot be 0, because 0 is a real channel.
constexpr int kSigNone = 65535;

constexpr int kMaxFrames = 96;
constexpr int kMaxFields = 512;

std::string frameKeyText(uint32_t key) {
    char b[48];
    std::snprintf(b, sizeof b, "CAN%u %s0x%X", unsigned((key >> 30) & 1u) + 1u,
                  (key & (1u << 29)) ? "ext " : "", unsigned(key & 0x1FFFFFFFu));
    return b;
}

// "bit 16 +16" — the two numbers the Receive page calls Start Bit and Bit Length, printed the way it
// prints them. A start..end SPAN would have to be written twice, because Motorola runs DOWN from the
// start bit and Intel runs UP, and one span for both would misdescribe half the fields on the page.
std::string bitsText(int startBit, int width) {
    char b[32];
    std::snprintf(b, sizeof b, "bit %d +%d", startBit, width);
    return b;
}

}  // namespace

std::string CanFieldWidget::bitPath() const {
    const std::string bind = writableConfigPath(bindPath());
    const size_t dot = bind.rfind('.');
    if (dot == std::string::npos) return {};
    if (bind.compare(dot + 1, std::string::npos, "can_frame") != 0) return {};
    return bind.substr(0, dot + 1) + "can_bit";
}

std::vector<CanFieldWidget::Choice> CanFieldWidget::choices() const {
    const Cache& c = Cache::instance();
    std::vector<Choice> out;
    // "(none)" heads the list and is a real answer: a CAN sensor that names no field publishes nothing,
    // which is what an input that has not been set up yet should do.
    out.push_back({ "\xE2\x80\x94 none \xE2\x80\x94", 0, -1, true });

    for (int i = 0; i < kMaxFrames; i++) {
        const std::string fr = "can.gc_frame[" + std::to_string(i) + "].";
        const int flags = int(c.configValue(fr + "flags"));
        if (!(flags & M_USED)) continue;
        if (flags & M_TX) continue;        // nothing ever fills a transmit field's decoded value
        const uint32_t key =
            (uint32_t(int(c.configValue(fr + "bus")) & 1) << 30)
            | ((flags & M_EXT) ? (1u << 29) : 0u)
            | (uint32_t(c.configValue(fr + "id")) & 0x1FFFFFFFu);

        // The frame's NAME is a label and its id is its identity, so an unnamed frame still reads as
        // something — its bus and id — rather than as a blank row.
        std::string name = c.configString(fr + "name");
        const std::string where = frameKeyText(key);
        const std::string head = name.empty() ? where : (name + "  (" + where + ")");

        const int first = int(c.configValue(fr + "first_field"));
        const int n     = int(c.configValue(fr + "field_count"));
        for (int k = 0; k < n && first + k < kMaxFields; k++) {
            const std::string fl = "can.gc_field[" + std::to_string(first + k) + "].";
            const int bit   = int(c.configValue(fl + "bit_off"));
            const int width = int(c.configValue(fl + "width"));
            const int sig   = int(c.configValue(fl + "sig"));
            Choice ch;
            ch.frame = key;
            ch.bit   = bit;
            ch.label = head + "  \xC2\xB7  " + bitsText(bit, width);
            // A FIELD THAT ALREADY NAMES A CHANNEL is the frame's own business: it writes that channel
            // itself, and a sensor publishing the same one is the two-producers fault the firmware
            // raises P1656 for. Listed greyed with what it carries, so the reason is on screen.
            if (sig != kSigNone) {
                ch.pickable = false;
                std::string sn;
                if (const MetaModel* m = c.meta())
                    for (const auto& [nm, id] : m->signalMap())
                        if (id == sig) { sn = c.label(nm).empty() ? nm : c.label(nm); break; }
                ch.label += "   \xE2\x86\x92 " + (sn.empty() ? std::string("(a channel)") : sn);
            }
            out.push_back(std::move(ch));
        }
    }
    return out;
}

int CanFieldWidget::currentChoice(const std::vector<Choice>& cs) const {
    const std::string bp = bitPath();
    if (bp.empty()) return 0;
    const Cache& c = Cache::instance();
    const int      bit = int(std::lround(c.configValue(bp)));
    const uint32_t key = uint32_t(c.configValue(writableConfigPath(bindPath())));
    if (bit < 0) return 0;                       // names nothing
    for (size_t i = 1; i < cs.size(); ++i)
        if (cs[i].frame == key && cs[i].bit == bit) return int(i);
    return -1;                                   // names something the tune no longer has
}

jf::JControl* CanFieldWidget::control() {
    if (!m_combo) {
        m_combo = std::make_unique<jf::JComboBox>(sceneGraph());
        m_combo->onIndexChanged.connect([this](int idx) {
            if (syncing()) return;                       // our own setCurrentIndex echo
            const std::string bind = writableConfigPath(bindPath());
            const std::string bp   = bitPath();
            if (bind.empty() || bp.empty()) return;
            if (idx < 0 || idx >= int(m_choices.size())) return;
            const Choice& ch = m_choices[size_t(idx)];
            if (!ch.pickable) return;                    // greyed: the frame writes that channel itself
            // BOTH, ALWAYS, and the frame first: they are one choice, and a half-applied pick is a
            // reference to a field that was never offered.
            Cache& C = Cache::instance();
            C.setConfigValue(bind, double(ch.frame));
            C.setConfigValue(bp,   double(ch.bit));
        });
    }
    return m_combo.get();
}

void CanFieldWidget::syncControl() {
    if (bindPath().empty()) return;
    m_choices = choices();
    int cur = currentChoice(m_choices);

    // A REFERENCE THAT NO LONGER RESOLVES gets its own entry rather than falling back to "(none)".
    // The frame was deleted or the field moved to other bits; showing "(none)" would say the sensor
    // was never set up, and the user would have no way to tell that from a setting that has broken.
    // The firmware says the same thing from its side — the sensor raises its config DTC.
    if (cur < 0) {
        const Cache& c = Cache::instance();
        const int    bit = int(std::lround(c.configValue(bitPath())));
        Choice miss;
        miss.frame = uint32_t(c.configValue(writableConfigPath(bindPath())));
        miss.bit   = bit;
        miss.pickable = false;
        miss.label = "\xE2\x9A\xA0 missing: " + frameKeyText(miss.frame)
                   + "  \xC2\xB7  bit " + std::to_string(bit);
        m_choices.push_back(std::move(miss));
        cur = int(m_choices.size()) - 1;
    }

    // THE KEY NAMES THE LIST, not its length: two tunes with the same number of fields on different
    // frames are different lists, and a length check would keep showing the first one's labels.
    std::string key;
    for (const Choice& ch : m_choices)
        key += std::to_string(ch.frame) + ":" + std::to_string(ch.bit) + (ch.pickable ? "+" : "-");
    if (key != m_syncedKey) {
        m_syncedKey = key;
        std::vector<std::string> labels;
        std::vector<uint8_t>     on;
        labels.reserve(m_choices.size());
        on.reserve(m_choices.size());
        for (const Choice& ch : m_choices) { labels.push_back(ch.label); on.push_back(ch.pickable ? 1 : 0); }
        m_combo->setItems(std::move(labels));        // base SyncScope guards the echo
        m_combo->setItemsEnabled(std::move(on));
    }
    if (cur != m_combo->currentIndex()) m_combo->setCurrentIndex(cur);
}

// Self-registration — type key, palette order (beside the other pickers), factory.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("canfield", CanFieldWidget, 102);
