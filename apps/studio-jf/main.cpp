// JFramework Studio — ECU tuning application built on JFramework.
// main() declares the shell (menus, toolbar, dock space) and wires the ECU pipeline: the serial
// link feeds the value store; connecting an ECU brings its meta / dashboard / tune into memory and
// populates the Tree + Dictionary docks. The runner (JAppWindow) owns layout/render/input.

#include <j/app/JAppWindow.h>
#include <j/core/JToggleButton.h>   // Locked/Editing toggle in the toolbar
#include <j/core/GenesisComponents.h>
#include <j/core/DockWidget.h>
#include <j/core/MenuSystem.h>
#include <j/core/StyleSheet.h>
#include <j/core/Log.h>
#include <j/config/Json.h>
#include <j/config/Settings.h>

#include "model/MetaModel.h"
#include "model/Cache.h"
#include "model/Ecu.h"
#include "comms/Crc32.h"        // verify the CRC footer on a dashboard fetched from SD
#include "model/StudioPaths.h"   // where the studio keeps its files — one answer, both platforms
#include "app/Resources.h"        // and where the files it SHIPS WITH are
#include "app/DesktopIntegration.h"   // an AppImage registers its menu entry and icon
#include "model/TuneFile.h"        // .tune JSON (jayecu-tune/1) <-> raw config image
#include "model/TsDashboard.h"
#include "model/TsDashboardConvert.h"
#include "model/TsIniImporter.h"   // import a TunerStudio/rusEFI .ini → native meta
#include "model/MathEvaluator.h"   // omnidyno-ported expression engine ([#config] [$telemetry] [@widget])
#include "model/SigilResolvers.h"  // the # config + $ telemetry sigil providers
#include "comms/EcuLink.h"
#include "comms/DeviceScanner.h"
#include "ui/ConnectButton.h"
#include "surface/WidgetRegistry.h"
#include "src/ui/NavTree.h"                 // tree <-> JSON, path utilities, TreeSwap undo
#include "src/ui/DictionaryTree.h"          // buildDictionary / buildSigilTree / buildSourceTree
#include "src/app/SelfTest.h"               // STUDIO_SELFTEST=1 early-out (the test itself lives there)
#include "surface/CanvasWidget.h"   // widgetByUid resolves the @-sigil against the live CanvasWidget tree
#include "surface/ControlPalette.h"
#include "ui/DiagnosticsDock.h"
#include "ui/DtcDock.h"
#include "ui/KnockScopeView.h"
#include <chrono>
#include "ui/TriggerDesigner.h"   // TriggerLibraryDock (left) + TriggerDesignerView (centre editor tab)
#include "ui/AboutLogo.h"
#include <j/core/MdiArea.h>
#include "ui/PreferencesDialog.h"
#include "ui/KeyBindingsDialog.h"
#include "ui/AxisSetupDialog.h"
#include "ui/InsertValueDialog.h"   // Insert asks for the value (and states the channel's range)
#include <j/app/JProgressDialog.h>      // a 90-second SD fetch is a modal, not a status line
#include "ui/AxisWizardDialog.h"    // start / end / increment -> a whole axis, size included
#include "ui/SelectConnectionDialog.h"
#include "ui/OutputWizardDialog.h"
#include "model/EngineOutputLayout.h"   // the studio lays out coil + injector output rows
#include "surface/widgets/ComboBoxWidget.h"   // …and filters a row's Cylinder picker (optionFilter)
#include "surface/widgets/LabelWidget.h"      // the audit measures a caption the way its paint does
#include "model/CardLogs.h"
#include "ui/CardLogsDialog.h"        // Logging▸Logs on Card… → import the ECU's own logs
#include "ui/OnboardLoggingDialog.h"    // Logging▸Onboard Logging… → the card's channels + gate
#include "surface/widgets/GenericCanWidget.h"   // the generic CAN editor, hosted ON the page
#include "surface/widgets/WiringWidget.h"
#include "surface/widgets/WizardButtonWidget.h"
#include "ui/AutotunePanel.h"        // centre tool tab: the VE autotuner
#include "surface/widgets/LearnedActionWidget.h"   // Apply to Base / Reset, as page buttons
#include "ui/EngineCyclePanel.h"     // centre tool tab: rolling cycle recorder + transport
#include "model/CycleWire.h"         // 0x26 capture bytes -> the native enginecycle::Cycle
#include "model/TriggerLog.h"        // 0x27 RAW trigger log: time between edges
#include "ui/TriggerLogPanel.h"      // ...and its own view. It is NOT the cycle view.
#include "model/RusefiCycle.h"       // rusEFI's composite tooth log -> the same model
#include "model/ImageCache.h"      // Needle Image Override — decode once, keep the texture
#include "ui/ExpressionEditor.h"   // visibility EXPRESSION editor (? → sigil-field autocomplete)
#include "ui/SaveChangesDialog.h"  // "unsaved changes" prompt on close (Save/Discard/Cancel + remember)

#include <csignal>

// Document dirty state + close-prompt state machine. File-scope so the Surface::onModified hook and the
// close/save wiring in main() can share them without capture gymnastics. saveOnExit: 0=Ask,1=Save,2=Discard.
static bool g_docDirty  = false;
static int  g_closeState = 0;    // 0=none, 1=prompting, 2=confirmed (user chose save/discard)
// SIGTERM/SIGINT land here (async-signal-safe flag); a main-thread timer routes them through the same
// guarded requestClose() as the window ✕, so an external `kill -TERM` still gets the save prompt.
static volatile std::sig_atomic_t g_termRequest = 0;
// Dock housekeeping, wired once the docks and the View menu exist (they are locals of main). The
// housekeeping timer calls these: one keeps each panel's last-known placement current so hiding and
// re-showing puts it BACK, the other keeps the View menu's ticks matching what is actually on screen.
static std::function<void()> g_trackDockHomes, g_syncViewToggles;
#include "ui/PresetEditorDialog.h"
#include "app/HelpPages.h"        // help as HTML, opened in the browser (Lua API reference)
#include <j/platform/JDesktop.h>   // hand a folder or a page to the desktop to open
#include "ui/LineEditorDialog.h"
#include "ui/PanelContentsDialog.h"
#include "ui/BurnButton.h"       // toolbar Burn + the unburned-changes indicator
#include "ui/MenuGate.h"         // offer only what the ECU in front of the studio can do
#include "model/DatalogRecorder.h"   // toolbar Record + the studio's own MSL datalog
#include "ui/ChoiceDialog.h"        // a question with named answers (not a list box)
#include "ui/TuneDiffDialog.h"      // …and the same question with the evidence in front of it
#include "model/TuneDiff.h"
#include "ui/ListPickerDialog.h"
#include "ui/CanTemplateDialog.h"
#include "ui/ChannelPickerDialog.h"
#include "ui/ChannelPropsDialog.h"
#include "model/ChannelPrefs.h"
#include "model/UnitManager.h"
#include "ui/ReseedDialog.h"           // Help▸Add Missing Navigation Entries: nodes the definition has gained
#include "ui/NewTuneDialog.h"       // New Tune: name + schema picker (app dialog on framework primitives)
#include "ui/LandingView.h"
#include "ui/FilterTree.h"
#include <j/core/Dialog.h>
#include <j/io/HttpClient.h>       // JHttpClient — fetching a published ECU definition
#include "comms/RusefiIniUrl.h"    // signature -> the URL rusEFI publishes its .ini at
#include <j/app/JAppUpdater.h>     // the studio's own updates: check, download, verify, install on close
#include <j/update/JVersion.h>     // which of two versions is newer — one rule for studio, firmware, dashboards
#include "app/FirmwareFetch.h"     // newer ECU firmware kits, from the firmware releases
#include "model/FirmwareKits.h"    // the kits on hand: shipped with the studio + downloaded
#include "app/FirmwareUpgrade.h"   // ...and putting one on the connected ECU, tune and all
#include "ui/FirmwareChangesDialog.h" // what an update takes away and brings, on the pages
#include "comms/Dfu.h"              // is an ECU sitting in its bootloader?
#include "StudioVersion.h"         // STUDIO_VERSION (generated by CMake)
#include "comms/TsLink.h"
#include "model/Perf.h"          // TunerStudio binary protocol (interop for imported-ini ECUs)
#include "comms/TsCacheBridge.h"   // Cache <-> TS link adapter (write + burn + telemetry poll)
#include <j/core/UndoStack.h>   // tree-edit undo (whole-tree snapshots)
#include "surface/SurfaceTabs.h"
#include "surface/PropertiesDock.h"
#include "ui/PopupSignalPicker.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstdint>
#include <csignal>
#include <atomic>
#include <cstdio>
#if !defined(_WIN32)
#include <execinfo.h>   // backtrace() — glibc only (crash-handler debug aid below)
#endif
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <cctype>
#include <filesystem>
#include <cmath>
#include <cstring>

#include <j/core/JAiBus.h>        // opt-in AI bus — install onAction for value control (set_value/select)
#include <j/core/JSpinBox.h>
#include <j/core/JDoubleSpinBox.h>
#include <j/core/JComboBox.h>
#include <j/core/JCheckBox.h>

using namespace jf;


// Build the Dictionary tree from a meta descriptor: telemetry channels and config scalars, each
// grouped by module. This is the draggable binding palette the studio edits panels against.


// Map a level name to a level (for JF_LOG parsing).
static jf::JLogLevel parseLogLevel(const std::string& s) {
    if (s == "trace") return jf::JLogLevel::Trace;
    if (s == "debug") return jf::JLogLevel::Debug;
    if (s == "warn")  return jf::JLogLevel::Warn;
    if (s == "error") return jf::JLogLevel::Error;
    if (s == "off")   return jf::JLogLevel::Off;
    return jf::JLogLevel::Info;
}

// Apply category log levels from the JF_LOG env var: comma-separated cat=level pairs. A trailing '*'
// sets a whole subtree. e.g.  JF_LOG=comms=info,comms.*=trace  (dump every protocol byte on comms).
static void applyLogEnv() {
    const char* env = std::getenv("JF_LOG");
    if (!env) return;
    std::stringstream ss(env);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        const auto eq = tok.find('=');
        if (eq == std::string::npos) continue;
        jf::JLog::instance().setLevel(tok.substr(0, eq), parseLogLevel(tok.substr(eq + 1)));
    }
}


// Scripted-run switches, so a
// test can drive the studio without a hand on the mouse; these let an import be loaded, a page selected and
// the frame captured to a file with no one at the controls, so a converted page can be inspected.
//
//   --import <ini>     import a TunerStudio ini at startup, exactly as File > Import does
//   --node <path>      select this navigation node (so its converted page is on screen)
//   --shot <prefix>    capture the frame to <prefix>_<surface>.ppm once the UI has settled
//   --quit-after <ms>  exit after this long
//   --dirty            mark the document modified, so quitting hits the unsaved-changes prompt
//   --mode-flips <n>   toggle Locked/Editing n times, one flip per scripted tick. The mode flip swaps the
//                      WHOLE dock layout (capture the outgoing mode's, restore the incoming mode's), which
//                      is where panels have gone missing: `logPlacement` shouts ORPHANED for any panel left
//                      in no host, so the run's own log is the pass/fail.
//
// Headless note: run under Xvfb with Mesa's software Vulkan, since a virtual display has no GPU —
//   VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json DISPLAY=:99 ./build/studio --import ...
struct ScriptedRun {
    std::string importIni, node, shot;
    // OPEN A DEMO PROJECT, OFFLINE: a new tune from this definition, exactly as File▸New Tune makes one,
    // so a page can be opened and photographed with no ECU in the room (manual/tools/shoot.sh). The
    // tune lands in the studio's data folder, so a documentation run points HOME somewhere disposable.
    std::string openMeta;
    // SET A VALUE in the demo before the page is shot, in the units the studio shows ("boost.enabled=1",
    // "boost.activation_kpa=120"; an option by its index). A screenshot of a switched-off module is a page
    // of greyed controls, which documents nothing. Only the throwaway demo tune is touched.
    std::vector<std::pair<std::string, double>> sets;
    // AUDIT EVERY PAGE, one per tick, and say what is wrong with each. A document's geometry can be
    // checked offline (tools/layout/check_doc.py reads the file), but that reads what was AUTHORED — and
    // where a page is arranged by a managed layout, what is authored and what the reader ends up looking
    // at are two different things. This opens each page for real and measures the rects the layout
    // actually produced: anything that lands outside the page, and any two things on top of each other.
    std::string auditFile;
    // OPEN A TOOLS TAB by its menu name ("Auto Tune", "Trigger Log") once the page is up, so a manual
    // screenshot of a tool is scripted like a page is. Clicking the menu from a script means guessing
    // where a popup's items land, and the popup is not in an X capture to check against.
    std::string tool;
    int quitAfterMs = 0;
    int modeFlips = 0;    // Locked/Editing toggles to perform, one per tick (dock-layout regression check)
    bool connect = false;
    bool dirty = false;   // the unsaved-changes prompt is only reachable from a DIRTY document
    bool any() const { return !importIni.empty() || !openMeta.empty() || !sets.empty() || !node.empty() || !shot.empty() || quitAfterMs > 0 || connect || dirty || modeFlips > 0 || !auditFile.empty() || !tool.empty(); }
};
static ScriptedRun g_run;

// THE reconcile — one routine, both connect paths. A jayecu ECU and a TunerStudio one are the same
// situation: the studio holds a tune, the ECU holds a tune, and on connect they may not be the same tune.
// Nothing about that is protocol-specific, so nothing about the question the user gets should be either.
//
// Sync is decided by the DICTIONARY (the serialised field set), not raw bytes — padding and firmware-owned
// bytes that a dict round-trip cannot reproduce must not raise a prompt on every single connect.
//
// There is no merge. One of the two tunes is about to be lost, so the only question is WHICH — and the
// third answer is not "keep both" (that is a live session tuning against bytes it does not have; the next
// edit writes a delta onto a base it never saw) but "neither, don't connect". `decide` is called exactly
// once: synchronously when there is nothing to ask, otherwise when the user answers.
enum class JTuneSync { NothingLocal, InSync, Pull, Push, PushDefaults, Cancel };

// THE PAGES, FOR GROUPING THE REPORT. reconcileTune runs before main's page library exists as far as
// this translation unit is concerned, and the report needs it only to say WHICH PAGE a difference is
// on — so main points this at the library once it has one. Null is a working state, not a broken one:
// every difference then reports as unpaged rather than going unmentioned.
static PanelLibrary* g_pagesForDiff = nullptr;

// THE PAGES OF THE ECU BEING RECONCILED, WITHOUT ADOPTING ITS PROJECT.
//
// The report groups by page, and the live page library is whatever project is currently OPEN — which
// on the connect path is deliberately not this ECU's. Nothing is adopted until the user has answered:
// a connection they then cancel must leave the studio holding exactly what it held before Connect was
// pressed. Correct, and it left the report with no pages to group by, so every difference on a first
// connect — to a rusEFI board, or to any ECU whose project was not already open — reported as a
// setting the document shows on no page, and the side-by-side render never ran.
//
// So the document is read for its pages ALONE: a throwaway library, built from the file, used to
// answer "which page shows this" and dropped. It adopts nothing and changes nothing.
static std::shared_ptr<PanelLibrary> pagesOfEcu(Ecu* e) {
    if (!e) return nullptr;
    auto j = jf::JJson::tryParseFile(e->dashboardPath());
    if (!j || !j->isObject() || !(*j)["panelLibrary"].isObject()) return nullptr;
    auto lib = std::make_shared<PanelLibrary>();
    lib->load((*j)["panelLibrary"]);
    return lib->pages().empty() ? nullptr : lib;
}

static void reconcileTune(jf::JAppWindow& win, const std::string& tuneName,
                          const std::vector<uint8_t>& localImg, const std::vector<uint8_t>& ecuImg,
                          const MetaModel* meta, Ecu* ecuForPages,
                          std::function<void(JTuneSync)> decide) {
    // THE ECU HAS NO TUNE AT ALL — not a different one. It rejected its stored tune at boot (a firmware
    // update that never put the tune back, a damaged bank) and is running with every setting zero. That
    // is not a calibration to weigh against the saved one, and "keep the ECU's tune" would have copied
    // the zeros over the saved tune — offered as the default answer, with nothing to say what it meant.
    // Asked before anything else, because with no saved tune (a new board) the zeros were adopted
    // without a word. The answers: the saved tune if there is a real one, the firmware's default tune
    // (all a new board has), or walk away.
    const auto hashOf = [](const std::vector<uint8_t>& img) {
        return img.size() >= 4 ? (uint32_t(img[0]) | uint32_t(img[1]) << 8 | uint32_t(img[2]) << 16 |
                                  uint32_t(img[3]) << 24) : 0u;
    };
    const uint32_t layout = meta && !meta->layoutHash().empty()
                          ? uint32_t(std::strtoul(meta->layoutHash().c_str(), nullptr, 16)) : 0u;
    if (layout != 0u && hashOf(ecuImg) != layout) {
        const bool haveSaved = localImg.size() == ecuImg.size() && hashOf(localImg) == layout;
        JLOGC("ui.connect", jf::JLogLevel::Warn) << "ECU has NO valid tune (layout hash field does not match "
            << meta->layoutHash() << "); saved tune '" << tuneName << "' " << (haveSaved ? "usable" : "none");
        win.setStatusText("The ECU has no tune \xC2\xB7 waiting for a decision");
        auto once = std::make_shared<bool>(false);
        std::vector<ChoiceDialog::Choice> choices;
        std::vector<JTuneSync> answers;
        if (haveSaved) {
            choices.push_back({ "Put \"" + tuneName + "\" on the ECU", jf::JDialogButtonBox::Role::Accept });
            answers.push_back(JTuneSync::Push);
        }
        choices.push_back({ "Put the default tune on the ECU",
                            haveSaved ? jf::JDialogButtonBox::Role::Action : jf::JDialogButtonBox::Role::Accept });
        answers.push_back(JTuneSync::PushDefaults);
        choices.push_back({ "Cancel", jf::JDialogButtonBox::Role::Reject });
        win.openModal<ChoiceDialog>(
            std::string("This ECU has no tune"),
            std::string("It started without a valid tune \xE2\x80\x94 a new board, a firmware update that did not "
                        "finish putting the tune back, or a damaged tune \xE2\x80\x94 so it is not running and every "
                        "reading except the battery is 0.\n") +
            (haveSaved ? "Put your saved tune \"" + tuneName + "\" on it, or the firmware's default tune to start from."
                       : std::string("There is no saved tune for this ECU, so it starts from the firmware's default tune.")) +
            "\nThen press Burn to keep it, and Reset ECU so the ECU starts on it.",
            choices,
            std::function<void(int)>([decide, once, answers](int idx) {
                if (*once) return;
                *once = true;
                JLOGC("ui.connect", jf::JLogLevel::Info) << "tune reconcile (no tune) choice=" << idx;
                decide(idx >= 0 && idx < int(answers.size()) ? answers[size_t(idx)] : JTuneSync::Cancel);
            }));
        return;
    }
    const bool comparable = !localImg.empty() && localImg.size() == ecuImg.size();
    if (!comparable) { decide(JTuneSync::NothingLocal); return; }   // nothing to protect → the ECU's image
    const bool inSync = meta ? TuneFile::serialise(localImg, *meta) == TuneFile::serialise(ecuImg, *meta)
                             : localImg == ecuImg;
    if (inSync) { decide(JTuneSync::InSync); return; }


    // WHAT THE QUESTION ACTUALLY IS. The saved tune MIRRORS the ECU: the studio writes edits to the
    // device live and auto-saves the same bytes to the file, so the two are in step by the end of every
    // session. A difference at connect therefore does not mean "two rival tunes" — it means THIS ECU HAS
    // BEEN CHANGED SOMEWHERE THE STUDIO COULD NOT SEE: another tuner, another machine, a firmware flash
    // that wiped the config bank. The user is not being asked to arbitrate; they are being told what
    // happened and offered their own calibration back.
    //
    // And when the device's image IS the meta's defaults, the cause is not a mystery at all — it has
    // been flashed or wiped, so say that rather than making the user infer it from a byte count.
    const bool ecuIsDefaults = meta && ecuImg == meta->defaultImage();
    JLOGC("ui.connect", jf::JLogLevel::Warn) << "ECU differs from the saved tune '" << tuneName
        << "'" << (ecuIsDefaults ? " (device is at DEFAULTS \xE2\x80\x94 flashed or wiped)" : "")
        << " \xE2\x86\x92 prompting restore/keep/cancel";
    win.setStatusText("The ECU does not match \"" + tuneName + "\" \xC2\xB7 waiting for a decision");
    // Guarded so the two ways out of the dialog — a choice, and a dismissal — cannot both fire.
    auto once = std::make_shared<bool>(false);
    // A QUESTION WITH THREE NAMED ANSWERS, not a list box. This was a ListPickerDialog — the options as
    // rows inside a scrolling list, with OK and Cancel underneath — so the two answers that matter were
    // rows you selected and then confirmed, and the two that did nothing (the Cancel button, dismissal)
    // were buttons. One of these answers OVERWRITES THE ECU; it should not be reachable by
    // double-clicking a line of text in a list.
    // THE COMPARISON IS THE PROMPT. Asking "keep the ECU's tune or push yours" over 141 KB of config
    // with nothing shown is a coin toss with an engine on the other end — one changed idle target and
    // a completely different tune ask the same question and look identical. So the report comes first:
    // only the pages that differ, both values side by side, and the two answers underneath it.
    //
    // A report needs a document to group by; without one every difference still shows, as unpaged.
    {
        // This ECU's own pages first; the live library only when it IS this ECU's project (already
        // open), and neither when the document has none — a report with no pages is still a report.
        //
        // HELD IN A SLOT, not on the stack: the dialog is modal and asynchronous, so it outlives this
        // scope and keeps reading these pages while it renders them. One reconcile can be in flight at
        // a time — it is the prompt that blocks the connect — so one slot says exactly that, and the
        // next reconcile replaces what the last one held.
        static std::shared_ptr<PanelLibrary> s_diffPages;
        s_diffPages = pagesOfEcu(ecuForPages);
        const PanelLibrary* pages = s_diffPages ? s_diffPages.get() : g_pagesForDiff;
        const tunediff::Report rep = tunediff::compare(*meta, localImg, ecuImg, pages);
        if (!rep.empty()) {
            win.openModal<TuneDiffDialog>(
                ecuIsDefaults
                    ? std::string("This ECU is at its DEFAULTS \xE2\x80\x94 it has been flashed or wiped")
                    : std::string("The ECU does not match \"" + tuneName + "\""),
                tuneName, std::string("On the ECU"), rep, localImg, ecuImg,
                const_cast<PanelLibrary*>(pages),
                std::function<void(int)>([decide, once](int idx) {
                    if (*once) return;
                    *once = true;
                    JLOGC("ui.connect", jf::JLogLevel::Info) << "tune reconcile (diff) choice=" << idx;
                    decide(idx == 0 ? JTuneSync::Pull : idx == 1 ? JTuneSync::Push : JTuneSync::Cancel);
                }));
            return;
        }
    }
    win.openModal<ChoiceDialog>(
        ecuIsDefaults
            ? std::string("This ECU is at its DEFAULTS \xE2\x80\x94 it has been flashed or wiped")
            : std::string("This ECU has been changed since \"" + tuneName + "\" last matched it"),
        std::string("The saved tune mirrors the ECU, so a difference means this ECU was changed "
                    "somewhere the studio could not see \xE2\x80\x94 another machine, another tuner, or a "
                    "firmware flash that wiped the config bank.\n"
                    "Keeping the ECU's tune updates \"" + tuneName + "\" to match what is on the device. "
                    "Restoring writes your saved calibration back over it."),
        std::vector<ChoiceDialog::Choice>{
            { "Keep the ECU's tune",  jf::JDialogButtonBox::Role::Accept },
            { "Restore \"" + tuneName + "\" to the ECU", jf::JDialogButtonBox::Role::Destructive },
            { "Cancel",               jf::JDialogButtonBox::Role::Reject } },
        // ONE ANSWER PATH. Cancel, Escape, the close [x] and the window manager all arrive here as -1 —
        // the dialog owns that rule — and it MUST be heard: there is an open link and a mid-flight
        // connect button waiting behind this prompt.
        std::function<void(int)>([decide, once](int idx) {
            if (*once) return;
            *once = true;
            JLOGC("ui.connect", jf::JLogLevel::Info) << "tune reconcile choice=" << idx;
            decide(idx == 0 ? JTuneSync::Pull : idx == 1 ? JTuneSync::Push : JTuneSync::Cancel);
        }));
}


// WHAT THE CANVAS IS ACTUALLY SHOWING, when it is not simply live data.
//
// RUN MODE ONLY. Editing designs the layout — the tune is locked, widgets are being moved — so a banner
// about the ECU's state is both beside the point and sitting on top of the things being arranged.
// Cache::readOnly() is that mode flag (setReadOnly(editing), from the Locked/Editing toggle).
// THE WINDOW THE NOTICE LIVES IN. Set once in main; the state changes that drive the strip arrive on
// signals from the link and from the cache, none of which carry the window with them.
static jf::JAppWindow* g_noticeWin = nullptr;

// IN THE CHROME, NOT ACROSS THE CANVAS. This used to be a banner drawn over the middle of the surface
// with the whole canvas dimmed behind it, which is unmissable exactly once — after that it is a box
// sitting on top of the gauges being read, and there is no way to push it aside. Above the toolbar it
// stays visible for as long as it is true, takes its own strip rather than borrowing the content's,
// and covers nothing.
static void setNotice(const char* text, const char* detail, const uint8_t* accent) {
    if (!g_noticeWin) return;
    g_noticeWin->setNotice(text ? text : "", detail ? detail : "", accent);
}

// THE OTHER PROTOCOL'S LINK, asked the same question. The banner is about whether there is an ECU on
// the wire, which is not the same as whether the NATIVE link is open — and on a TunerStudio session it
// never is. A live rusEFI board at 8 Hz sat behind a red NOT CONNECTED strip saying its readings were
// stale, which is the exact opposite of what that strip exists to tell you, and it is worse than no
// banner: it teaches you to ignore the one warning that matters. Bound in main, where tsLink lives.
static std::function<bool()> g_altLinkOpen;

// THE OUTPUT GATEWAYS, as a headline or nothing. Null when both are on, or when this definition has
// no such switch at all — a rusEFI board over TunerStudio has neither field, and configValue() answers
// 0 for a path it cannot resolve, which would have read as "both disabled" on every ECU that does not
// have them. isConfig() is the question that actually distinguishes "off" from "not a field here", and
// hasConfig() keeps the gap between connecting and the first config read from flashing a false one.
static const char* outputGateNotice() {
    const Cache& c = Cache::instance();
    if (!c.hasConfig()) return nullptr;
    const bool haveIgn = c.isConfig("engine.ign_enable");
    const bool haveInj = c.isConfig("engine.inj_enable");
    const bool ignOff  = haveIgn && c.configValue("engine.ign_enable") == 0.0;
    const bool injOff  = haveInj && c.configValue("engine.inj_enable") == 0.0;
    if (ignOff && injOff) return "IGNITION AND INJECTORS DISABLED";
    if (ignOff)           return "IGNITION DISABLED";
    if (injOff)           return "INJECTORS DISABLED";
    return nullptr;
}

static bool s_noTuneSeen = false;   // the ECU reports it has no valid tune (no_tune)
static void refreshLinkOverlay(bool open) {
    if (Cache::instance().readOnly()) { setNotice("", "", nullptr); return; }
    if (!open && g_altLinkOpen && g_altLinkOpen()) open = true;
    if (!open) {
        setNotice("NOT CONNECTED",
                  "No ECU on the link. Readings are the last values received, not live.",
                  jf::Colors::Danger);
        return;
    }
    // NO TUNE BEATS KEY OFF. An ECU that rejected its stored tune at boot (wrong layout — a firmware
    // update whose tune was never put back, a corrupt bank) runs disabled with every setting zero, so
    // every live value but the battery reads 0 and it looked exactly like KEY OFF, leaving you to guess.
    // The ECU says so on the wire (no_tune), and it stays set until the ECU is reset onto a burned tune.
    s_noTuneSeen = Cache::instance().has("no_tune") && Cache::instance().value("no_tune") != 0.0;
    if (s_noTuneSeen) {
        setNotice("NO TUNE ON THE ECU",
                  "It started without a valid tune (after a firmware update, or a damaged one), so it is not "
                  "running and every reading except the battery is 0. Open a tune (File \xE2\x96\xB8 Open Tune \xE2\x80\x94 "
                  "backups are in the ECU's backups folder), Burn, then Reset ECU.",
                  jf::Colors::Danger);
        return;
    }
    // key_on is published by Sensors; absent (older firmware) claims nothing rather than asserting off.
    if (!Cache::instance().has("key_on")) { setNotice("", "", nullptr); return; }
    if (Cache::instance().value("key_on") == 0.0)
        // WHAT KEY-OFF MEANS, AND WHY THE BANNER IS NOT OPTIONAL. With the key off the firmware runs only
        // the bootstrap battery: every other input sits behind the 5 V followers, which USB feeds through
        // a diode, and the drop across it shifts the reference the ADC measures against. The rail is up
        // and the numbers would look perfectly plausible — they would simply be calibrated against a
        // different reference than the running engine, which is worse than absent. So those channels
        // read zero on purpose, and without this banner a correct ECU is indistinguishable from a rig
        // full of dead sensors. An afternoon went into a wideband reading 0 for exactly that reason.
        setNotice("KEY OFF",
                  "Only the battery is read. The 5 V followers are USB-fed through a diode, so ADC "
                  "offsets do not match key-on \xE2\x80\x94 readings and calibration wait for 12 V.",
                  jf::Colors::Warning);
    else if (const char* gate = outputGateNotice())
        // AN ENGINE THAT WILL NOT START, ANNOUNCED RATHER THAN DIAGNOSED. The ignition and injection
        // gateways are two bytes on two different pages, and switching one off makes the engine behave
        // exactly like a dead crank sensor, a dead coil driver or an empty firing order — it cranks and
        // nothing happens. This is the same class of afternoon the KEY OFF strip exists to prevent, so
        // it says so in the same place: deliberate, remembered across a burn, and invisible until now
        // unless you happened to be looking at the page that holds the switch.
        setNotice(gate,
                  "Switched off in Engine Configuration. The ECU is cutting these outputs on purpose "
                  "\xE2\x80\x94 the engine will crank without starting until they are switched back on.",
                  jf::Colors::Warning);
    else
        setNotice("", "", nullptr);
}

// THE RECENT ECU LIST, written wherever a project is opened.
//
// It used to be written in ONE place: the native identity handshake, which parses
// "jayecu <board> <version> <build> <layout_hash> <device_uid> …". A rusEFI ECU never goes through
// that — it connects over TunerStudio's protocol and is keyed "tsimport:<layout hash>" — so it was
// never recorded, and "Reopen last project on launch" could only ever reopen the last NATIVE project
// however recently the rusEFI one had been used. The same held for a project opened from the ECU
// library or created offline.
//
// So it is written where a project actually BECOMES the open one, which every route shares, and from
// the Ecu itself rather than from one protocol's handshake. Most-recent-first, deduplicated, eight kept.
static void noteRecentEcu(const std::string& label, const std::string& uid) {
    if (uid.empty() || label.empty()) return;
    auto& settings = jf::JSettings::instance();
    const std::string entry = label + "\t" + uid;
    std::string rec = entry;
    const std::string prev = settings.get<std::string>("recent.ecus", "");
    size_t rp = 0; int rn = 1;
    while (rp < prev.size() && rn < 8) {
        const size_t nl = prev.find('\n', rp);
        const std::string ln = prev.substr(rp, nl == std::string::npos ? std::string::npos : nl - rp);
        rp = (nl == std::string::npos) ? prev.size() : nl + 1;
        // DEDUPLICATED BY UID, not by the whole line. An ECU whose label changes — an import that learns
        // its signature, a board renamed — would otherwise appear twice, the same ECU under two names,
        // and the older name would sit in the list for ever because nothing ever writes it again.
        const size_t lt = ln.find('\t');
        const std::string lu = (lt == std::string::npos) ? ln : ln.substr(lt + 1);
        if (!ln.empty() && lu != uid) { rec += "\n" + ln; ++rn; }
    }
    settings.set("recent.ecus", jf::JVariant(rec));
}

// STARTUP STOPWATCH: milliseconds since main() began, at each phase boundary, under JF_LOG=ui.boot=info.
// Kept because "the window appears and the interface arrives seconds later" is a real complaint that
// cannot be answered by reading code — it was answered by these, and the answer was not what anyone
// guessed: the whole of the app's construction is 90 ms, and the wait was the meta LIBRARY being parsed
// in full (16 files, 23 MB) to read one string out of each. Six marks cost nothing and mean the next
// regression is measured in one run instead of bisected by hand.
static std::chrono::steady_clock::time_point g_bootT0;
static void bootMark(const char* what) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - g_bootT0).count();
    JLOGC("ui.boot", jf::JLogLevel::Info) << what << " @" << ms << "ms";
}


// These two were statics INSIDE main(), which is the same storage but out of reach of anything lifted out
// of it. At file scope they are visible to the blocks that move, and identical in every other way.
static std::function<void(const std::string&)> g_doImport;      // File▸Import a TunerStudio .ini, by path
static std::function<void(const std::string&)> g_newDemoTune;   // --open-meta: File▸New Tune, from a definition path
static std::unique_ptr<Surface> pageSurface;                    // the page currently in an MDI window
static std::function<void(const std::string&)> g_openTool;      // --tool: a Tools▸ item, by its name

// THE SCRIPTED RUN, in a function of its own — and the reason is compile time, not tidiness. main() had
// grown to 4,850 lines with hundreds of lambdas in it, and GCC spends its time PER FUNCTION: of the 82 s
// this file took, 6 s was parsing every header behind it and 65 s was the optimiser working through that
// one function. Ten ordinary functions cost a fraction of one enormous one, so the blocks come out.
static void installScriptedRun(jf::JAppWindow& win, jf::JTreeView& nodeTree,
                               std::function<void()>& connectFn, jf::JToggleButton& lockBtn) {
    static jf::JTimer scriptTimer;
    if (g_run.any()) {
        static int step = 0;
        scriptTimer.onTick.connect([&win, &nodeTree, &connectFn, &lockBtn]{
            ++step;
            if (step == 2 && !g_run.openMeta.empty() && g_newDemoTune) {
                JLOGC("ui.script", jf::JLogLevel::Info) << "--open-meta " << g_run.openMeta;
                g_newDemoTune(g_run.openMeta);
            }
            if (step == 2 && !g_run.importIni.empty() && g_doImport) {
                JLOGC("ui.script", jf::JLogLevel::Info) << "--import " << g_run.importIni;
                g_doImport(g_run.importIni);
            }
            if (step == 4 && !g_run.sets.empty()) {
                const MetaModel* mm = Cache::instance().meta();
                for (const auto& [path, value] : g_run.sets) {
                    const MetaModel::Location loc = mm ? mm->locate(path) : MetaModel::Location{};
                    if (loc.kind != MetaModel::Location::Kind::Scalar) {
                        JLOGC("ui.script", jf::JLogLevel::Warn) << "--set " << path << ": not a setting";
                        continue;
                    }
                    // The CACHE's scale, not the location's: a threshold stored in its sensor's units
                    // takes the type's scale, which only the Cache resolves (typeScaledField).
                    const double sc = Cache::instance().configScale(path);
                    Cache::instance().setConfigValue(path, value / (sc != 0.0 ? sc : 1.0));
                    JLOGC("ui.script", jf::JLogLevel::Info) << "--set " << path << " = " << value;
                }
            }
            if (step == 4 && g_run.connect && connectFn) {
                JLOGC("ui.script", jf::JLogLevel::Info) << "--connect";
                connectFn();
            }
            if (step == 5 && g_run.dirty) {
                JLOGC("ui.script", jf::JLogLevel::Info) << "--dirty (document marked modified)";
                g_docDirty = true;
            }
            if (step == 6 && !g_run.node.empty()) {
                JLOGC("ui.script", jf::JLogLevel::Info) << "--node " << g_run.node;
                nodeTree.selectByPath(g_run.node);
            }
            if (step == 7 && !g_run.tool.empty() && g_openTool) {
                JLOGC("ui.script", jf::JLogLevel::Info) << "--tool " << g_run.tool;
                g_openTool(g_run.tool);
            }
            // One flip per tick from tick 8, driven through the TOOLBAR TOGGLE rather than by calling the
            // mode hook: the button is what a user presses, and its handler does work of its own.
            if (g_run.modeFlips > 0 && step >= 8 && step < 8 + g_run.modeFlips) {
                const bool want = !lockBtn.isToggled();
                JLOGC("ui.script", jf::JLogLevel::Info) << "--mode-flips: -> " << (want ? "EDITING" : "LOCKED");
                lockBtn.setToggled(want);          // emits onToggled -> the real mode switch, same as a click
            }
            // Shoot LATE when the run also quits on a timer: two ticks before the end. A fixed tick 10
            // (2.5 s) fired before a --connect had finished pulling the tune, so the capture showed a page
            // rendered against an image that had not arrived — every bound value zero, which reads as a
            // broken widget rather than as a photograph taken too early.
            const int shotStep = (g_run.quitAfterMs > 0) ? std::max(10, g_run.quitAfterMs / 250 - 2) : 10;
            if (step == shotStep && !g_run.shot.empty()) {
                JLOGC("ui.script", jf::JLogLevel::Info) << "--shot " << g_run.shot;
                jf::JAppWindow::s_capturePath   = g_run.shot.c_str();
                jf::JAppWindow::s_captureRequest = true;
            }
            // --audit: one page per tick from tick 6. Select it, and on the NEXT tick — once the layout has
            // run — measure what the page actually put on screen.
            if (!g_run.auditFile.empty() && step >= 6) {
                static std::vector<std::string> paths;
                static size_t at = 0;
                static int bad = 0, seen = 0, widgets = 0, inputs = 0, reach = 0, bound = 0, structural = 0, templated = 0;
                static bool loaded = false;
                if (!loaded) {
                    loaded = true;
                    std::ifstream in(g_run.auditFile);
                    for (std::string ln; std::getline(in, ln);) if (!ln.empty()) paths.push_back(ln);
                    JLOGC("ui.audit", jf::JLogLevel::Info) << "auditing " << paths.size() << " page(s)";
                }
                // Measure the page selected on the previous tick, then move on to the next.
                if (at > 0 && at <= paths.size() && pageSurface) {
                    const std::string& path = paths[at - 1];
                    ++seen;
                    const auto els = pageSurface->elementsSnapshot();
                    const Surface::Xform t = pageSurface->xform();
                    const auto bb = pageSurface->getBoundingBox();
                    const jf::JRect page{ bb.x, bb.y, bb.width, bb.height };
                    // ONLY WHAT IS ACTUALLY ON SCREEN. A page states its alternatives at the same
                    // coordinates — three ignition modes in one box, twelve cylinders in another — and
                    // exactly one of them is ever visible. Measuring them all reports a pile that no
                    // reader can ever see, and buries the real ones.
                    // MEASURED FROM THE WIDGET, not from the model. toScreen() applies the free-mode
                    // transform to an element's stored rect — right for a page laid out in coordinates,
                    // and meaningless for one arranged by a managed layout, where the stored rect is not
                    // where the thing goes. The live widget's bounds are what the renderer just set: the
                    // rect actually on screen, whatever decided it. (The model rect is the fallback for a
                    // widget that has no instance yet.)
                    std::vector<jf::JRect> rects;
                    std::vector<bool> shown;
                    for (const PanelElement& e : els) {
                        CanvasWidget* w = pageSurface->widgetById(e.id);
                        if (w) {
                            const auto b = w->getBoundingBox();
                            rects.push_back(jf::JRect{ b.x, b.y, b.width, b.height });
                        } else {
                            rects.push_back(pageSurface->toScreen(e, t));
                        }
                        shown.push_back(w ? w->visibleNow() : true);
                    }
                    for (size_t i = 0; i < rects.size(); ++i) {
                        const jf::JRect& r = rects[i];
                        if (r.width <= 0.f || r.height <= 0.f || !shown[i]) continue;
                        if (r.x < page.x - 1.f || r.x + r.width > page.x + page.width + 1.f) {
                            ++bad; JLOGC("ui.audit", jf::JLogLevel::Info) << "  " << path << ": "
                                << els[i].type << " runs off the page across (x " << r.x << " w " << r.width
                                << " vs page x " << page.x << " w " << page.width << ")";
                        }
                    }
                    // ---- DOES EVERY WIDGET WORK AS ITS TYPE INTENDS? -------------------------------
                    // Geometry says a page is arranged; it says nothing about whether the things on it
                    // do their job. Two properties decide that for an input, and both were broken at some
                    // point in this work:
                    //   * the keyboard can REACH it. The canvas is its own focus domain — nothing outside
                    //     can tree-walk into it — so a control missing from the tab ring cannot be typed
                    //     into however interactive it claims to be, and nothing on screen says so.
                    //   * it has a DATA SOURCE. A field, a combo or a table with no binding is a live
                    //     control wired to nothing: it takes the caret, accepts the keystroke, and the
                    //     value goes nowhere.
                    // Checked by asking, not by typing: pressing 6,000 checkboxes to see if they toggle
                    // would rewrite the tune this is supposed to be auditing.
                    {
                        const std::vector<int> ring  = pageSurface->keyboardReachable();
                        const std::vector<int> ringA = pageSurface->keyboardReachable(/*assumeEnabled=*/true);
                        static const std::set<std::string> kNeedsBinding = {
                            "field", "configedit", "combobox", "enum", "value", "checkbox", "toggle",
                            "text", "slider", "table", "curve", "array1d",
                        };
                        // A SETTING SELECTOR IS BOUND BY ITS PRESETS, not by a dataSource: each preset names
                        // the config paths that preset writes, which is the whole point of the control
                        // (one pick sets a firing order AND a cylinder count). Asking it for a dataSource
                        // called seven correctly-wired pickers unbound.
                        auto unbound = [](const PanelElement& pe) {
                            if (pe.type == "settingselector") return pe.prop("presets").empty();
                            return pe.prop("dataSource").empty() && pe.prop("signalName").empty();
                        };
                        for (size_t i = 0; i < els.size(); ++i) {
                            const PanelElement& e = els[i];
                            CanvasWidget* w = pageSurface->widgetById(e.id);
                            if (w && shown[i]) ++widgets;
                            if (!w || !shown[i] || !w->interactive()) continue;
                            // A CONTAINER IS NOT A TAB STOP. A panel or a viewport is interactive because it
                            // routes the mouse to what is inside it; the keyboard goes to the controls
                            // themselves, which is why the ring skips them and should.
                            if (e.type == "panel" || e.type == "viewport") continue;
                            ++structural;
                            // STRUCTURE, asked of every control whether its condition has switched it on or
                            // not: enabled, would the ring enumerate it at all?
                            if (std::find(ringA.begin(), ringA.end(), e.id) == ringA.end()) {
                                ++bad; JLOGC("ui.audit", jf::JLogLevel::Info) << "  " << path << ": "
                                    << e.type << " can never enter the tab ring, even enabled";
                            }
                            if (kNeedsBinding.count(e.type) || e.type == "settingselector") {
                                ++bound;
                                // A field, a combo or a table with no binding is a live control wired to
                                // nothing: it takes the caret, accepts the keystroke, and the value goes
                                // nowhere. True of a disabled one too, so it is asked here.
                                if (unbound(e)) {
                                    ++bad; JLOGC("ui.audit", jf::JLogLevel::Info) << "  " << path << ": " << e.type
                                        << " has no data source - it can be typed into and changes nothing";
                                }
                            }
                            // A CONTROL ITS CONDITION HAS DISABLED IS NOT A TAB STOP, and should not be: the
                            // sensor switchboard carries one link per sensor with an enableCondition, and a
                            // tune that fits eight sensors leaves a hundred and twenty greyed. Tabbing
                            // through those to reach the eight that work is the bug, not the fix.
                            // Asked as renderDisabled(), the flag the paint just set, and NOT as
                            // enabledNow(): disable is INHERITED (PanelWidget.cpp sets a child disabled when
                            // its panel is), so a field whose own condition is empty is still inert inside a
                            // panel gated on a sensor nobody fitted. enabledNow() sees only the control's own
                            // condition and called 1,446 of those unreachable.
                            if (w->renderDisabled()) continue;
                            ++inputs;
                            if (std::find(ring.begin(), ring.end(), e.id) != ring.end()) { ++reach; continue; }
                            ++bad; JLOGC("ui.audit", jf::JLogLevel::Info) << "  " << path << ": "
                                << e.type << " is interactive but the keyboard cannot reach it";
                        }
                        // AND EVERY WIDGET INSIDE THE CONTAINERS. A page's elements are the top level only:
                        // seven or eight boxes, with the fields, combos and tables inside them. Auditing that
                        // level alone looked at 4,359 widgets across 597 pages and called it every widget on
                        // every page. A container is a nested focus domain, so each is asked for its own ring
                        // and its children are held to it, all the way down.
                        // DOES ITS BINDING NAME ANYTHING? A template page writes its paths with "[*]" —
                        // sensors.sensor[*].enabled — and the star is spliced at EVALUATION time from the
                        // widget's element scope, not stored resolved. A widget that never receives a
                        // scope therefore keeps a literal star, which names no field: the control draws,
                        // takes the caret, and reads and writes nothing. Checked the way the widget does
                        // it (resolveTemplate then resolveIndexed, CanvasWidget.cpp boundPathOf), and the
                        // result is then required to BE something — a live channel or a field the meta can
                        // locate.
                        const MetaModel* am = Cache::instance().meta();
                        auto checkBindings = [&](CanvasWidget* cw, const PanelElement& pe,
                                                 const std::string& where) {
                            const std::string ctx = cw->elementContext();
                            for (const auto& [k, v] : pe.props) {
                                if (v.find("[*]") == std::string::npos && v.find("$*") == std::string::npos)
                                    continue;
                                const std::string r = MathEvaluator::instance().resolveIndexed(
                                    MathEvaluator::resolveTemplate(v, ctx));
                                if (r.find("[*]") != std::string::npos || r.find("$*") != std::string::npos) {
                                    ++bad; JLOGC("ui.audit", jf::JLogLevel::Info) << "  " << path << ": "
                                        << pe.type << where << " prop " << k
                                        << " still holds a template star after resolution"
                                        << (ctx.empty() ? " (no element scope)" : " (scope " + ctx + ")");
                                }
                            }
                            ++templated;
                            // The bound path itself, resolved: it must name a channel or a config field.
                            const std::string bind = MathEvaluator::instance().resolveIndexed(
                                MathEvaluator::resolveTemplate(pe.prop("signalName"), ctx));
                            if (bind.empty() || bind.find('.') == std::string::npos) return;
                            // A path ending in a subscript names the ELEMENT, not a field in it — what a
                            // setup wizard binds ("outputs.output[14]"), and what locate() is not asked
                            // for. The element's existence is the page's own scope, checked above.
                            if (bind.back() == ']') return;
                            // AND A BINDING MAY BE AN EXPRESSION, not a path: a readout that counts how
                            // many injector outputs the staging modes add up to is a page of select()s,
                            // and the evaluator resolves it. Only something SHAPED like a path is asked
                            // of the meta.
                            if (bind.find_first_not_of("abcdefghijklmnopqrstuvwxyz"
                                                       "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.[]")
                                != std::string::npos) return;
                            if (Cache::instance().has(bind)) return;
                            if (am && am->locate(bind).valid()) return;
                            ++bad; JLOGC("ui.audit", jf::JLogLevel::Info) << "  " << path << ": " << pe.type
                                << where << " binds " << bind
                                << ", which is neither a live channel nor a field the meta can locate";
                        };
                        for (const PanelElement& e : els) {
                            CanvasWidget* w = pageSurface->widgetById(e.id);
                            if (w && w->visibleNow()) checkBindings(w, e, "");
                        }
                        // ---- DOES IT FIT? -----------------------------------------------------------------
                        // What a reader sees as a broken page: a caption cut off at the edge of its panel,
                        // a control sticking out of the box it belongs to, two controls drawn over each
                        // other. None of it shows in the top-level geometry above, because it all happens
                        // INSIDE containers. Measured from the rects the layout produced and from the
                        // label's own text metrics (LabelWidget::naturalSize / wrappedHeight, the same
                        // arithmetic its paint uses).
                        auto rectOf = [](CanvasWidget* w) { const auto b = w->getBoundingBox(); return jf::JRect{ b.x, b.y, b.width, b.height }; };
                        auto checkFit = [&](CanvasWidget* parent, const std::string& where) {
                            const jf::JRect pr = rectOf(parent);
                            std::vector<std::pair<CanvasWidget*, jf::JRect>> kids;
                            for (CanvasWidget* c : parent->childWidgets()) {
                                const PanelElement* ce = c ? c->element() : nullptr;
                                if (!ce || !c->visibleNow()) continue;
                                const jf::JRect r = rectOf(c);
                                if (r.width <= 0.f || r.height <= 0.f) continue;
                                kids.emplace_back(c, r);
                                if (r.x + r.width > pr.x + pr.width + 2.f || r.y + r.height > pr.y + pr.height + 2.f
                                    || r.x < pr.x - 2.f || r.y < pr.y - 2.f) {
                                    ++bad; JLOGC("ui.audit", jf::JLogLevel::Info) << "  " << path << ": " << ce->type
                                        << " runs outside its " << where << " (" << r.x << "," << r.y << " "
                                        << r.width << "x" << r.height << " in " << pr.x << "," << pr.y << " "
                                        << pr.width << "x" << pr.height << ")";
                                }
                                if (ce->type == "label") {
                                    const std::string text = ce->prop("labelText");
                                    if (text.empty()) continue;
                                    const std::string font = ce->prop("fontName");
                                    if (ce->prop("wrap") == "1") {
                                        const float needH = LabelWidget::wrappedHeight(text, font, r.width);
                                        if (needH > r.height + 2.f) {
                                            ++bad; JLOGC("ui.audit", jf::JLogLevel::Info) << "  " << path << ": label \""
                                                << text.substr(0, 40) << "\" wraps taller than its box (" << needH
                                                << " > " << r.height << ")";
                                        }
                                    } else {
                                        float needW = 0.f, needH = 0.f;
                                        LabelWidget::naturalSize(text, font, 0.0, needW, needH);
                                        if (needW > r.width + 2.f) {
                                            ++bad; JLOGC("ui.audit", jf::JLogLevel::Info) << "  " << path << ": label \""
                                                << text.substr(0, 40) << "\" is wider than its box (" << needW
                                                << " > " << r.width << ") and does not wrap";
                                        }
                                    }
                                }
                            }
                            for (size_t i = 0; i < kids.size(); ++i)
                                for (size_t j = i + 1; j < kids.size(); ++j) {
                                    const jf::JRect &A = kids[i].second, &B = kids[j].second;
                                    const float ox = std::min(A.x + A.width,  B.x + B.width)  - std::max(A.x, B.x);
                                    const float oy = std::min(A.y + A.height, B.y + B.height) - std::max(A.y, B.y);
                                    if (ox > 2.f && oy > 2.f) {
                                        ++bad; JLOGC("ui.audit", jf::JLogLevel::Info) << "  " << path << ": "
                                            << kids[i].first->element()->type << " and " << kids[j].first->element()->type
                                            << " overlap by " << ox << "x" << oy << " in a " << where;
                                    }
                                }
                        };
                        std::function<void(CanvasWidget*, const std::string&)> descend =
                            [&](CanvasWidget* parent, const std::string& where) {
                            checkFit(parent, where);
                            const std::vector<int> cring  = parent->keyboardRing();
                            const std::vector<int> cringA = parent->keyboardRing(/*assumeEnabled=*/true);
                            for (CanvasWidget* c : parent->childWidgets()) {
                                const PanelElement* ce = c ? c->element() : nullptr;
                                if (!ce || !c->visibleNow()) continue;
                                ++widgets;
                                checkBindings(c, *ce, " in a " + where);
                                if (c->isContainer()) { descend(c, where); continue; }
                                if (!c->interactive()) continue;
                                ++structural;
                                if (std::find(cringA.begin(), cringA.end(), ce->id) == cringA.end()) {
                                    ++bad; JLOGC("ui.audit", jf::JLogLevel::Info) << "  " << path << ": "
                                        << ce->type << " in a " << where
                                        << " can never enter the tab ring, even enabled";
                                }
                                if (kNeedsBinding.count(ce->type) || ce->type == "settingselector") {
                                    ++bound;
                                    if (unbound(*ce)) {
                                        ++bad; JLOGC("ui.audit", jf::JLogLevel::Info) << "  " << path << ": "
                                            << ce->type << " in a " << where << " has no data source"
                                            << " - it can be typed into and changes nothing";
                                    }
                                }
                                if (c->renderDisabled()) continue;   // inherited from its container; see above
                                ++inputs;
                                if (std::find(cring.begin(), cring.end(), ce->id) != cring.end()) { ++reach; continue; }
                                ++bad; JLOGC("ui.audit", jf::JLogLevel::Info) << "  " << path << ": " << ce->type
                                    << " in a " << where << " is interactive but the keyboard cannot reach it";
                            }
                        };
                        for (const PanelElement& e : els) {
                            CanvasWidget* w = pageSurface->widgetById(e.id);
                            if (w && w->isContainer() && w->visibleNow()) descend(w, e.type);
                        }
                    }

                    // Two things in the same place is the failure a reader sees as "widgets on top of
                    // widgets". Cards stack on purpose (one shows at a time); a column never should.
                    if (pageSurface->model() && pageSurface->model()->layout() != 4) {
                        for (size_t i = 0; i < rects.size(); ++i)
                            for (size_t j = i + 1; j < rects.size(); ++j) {
                                if (!shown[i] || !shown[j]) continue;
                                const jf::JRect &A = rects[i], &B = rects[j];
                                const float ox = std::min(A.x + A.width,  B.x + B.width)  - std::max(A.x, B.x);
                                const float oy = std::min(A.y + A.height, B.y + B.height) - std::max(A.y, B.y);
                                if (ox > 2.f && oy > 2.f) {
                                    ++bad; JLOGC("ui.audit", jf::JLogLevel::Info) << "  " << path << ": "
                                        << els[i].type << " and " << els[j].type << " overlap by "
                                        << ox << "x" << oy;
                                    j = rects.size(); i = rects.size();   // one report per page is enough
                                }
                            }
                    }
                }
                if (at < paths.size()) {
                    nodeTree.selectByPath(paths[at]);
                    ++at;
                } else if (at == paths.size()) {
                    // COUNTS, so a clean run is evidence rather than an assertion nobody can fail. A check
                    // that never looked at anything reports zero problems just as loudly as one that did.
                    JLOGC("ui.audit", jf::JLogLevel::Info) << "audit done: " << seen << " page(s), "
                        << widgets << " visible widget(s), " << inputs << " interactive, "
                        << reach << " reachable by keyboard, " << structural
                        << " ring-enumerable when enabled, " << bound
                        << " checked for a data source, " << templated
                        << " binding paths resolved, " << bad << " complaint(s)";
                    ++at;
                    win.requestClose();
                }
            }
            if (g_run.quitAfterMs > 0 && step * 250 >= g_run.quitAfterMs) win.requestClose();
        });
        scriptTimer.start(std::chrono::milliseconds(250), jf::JTimer::JMode::Repeating);
    }
}

int main(int argc, char** argv) {
    g_bootT0 = std::chrono::steady_clock::now();
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if      (a == "--import")     g_run.importIni = next();
        else if (a == "--node")       g_run.node      = next();
        else if (a == "--tool")       g_run.tool      = next();
        else if (a == "--open-meta")  g_run.openMeta  = next();
        else if (a == "--set") {
            const std::string kv = next();
            const size_t eq = kv.find('=');
            if (eq != std::string::npos) g_run.sets.emplace_back(kv.substr(0, eq), std::atof(kv.c_str() + eq + 1));
        }
        else if (a == "--shot")       g_run.shot      = next();
        else if (a == "--quit-after") g_run.quitAfterMs = std::atoi(next().c_str());
        else if (a == "--connect")    g_run.connect   = true;
        else if (a == "--dirty")      g_run.dirty     = true;
        else if (a == "--mode-flips") g_run.modeFlips = std::atoi(next().c_str());
        else if (a == "--audit")      g_run.auditFile = next();
    }
    if (const char* st = std::getenv("STUDIO_SELFTEST"); st && *st == '1') return runOwnedStateSelfTest();
    bootMark("args parsed");
    // TEMP: dump a backtrace on abort (fortify "buffer overflow detected" → SIGABRT) to pinpoint the popup crash.
#if !defined(_WIN32)
    {
        auto crashHandler = +[](int sig) {
            void* bt[40]; int n = backtrace(bt, 40);
            std::fprintf(stderr, "\n*** signal %d backtrace (%d) ***\n", sig, n); std::fflush(stderr);
            backtrace_symbols_fd(bt, n, fileno(stderr));
            std::signal(sig, SIG_DFL); std::raise(sig);
        };
        std::signal(SIGABRT, crashHandler);
        std::signal(SIGSEGV, crashHandler);
    }
#endif
    // SIGUSR1 → capture the next rendered frame to /tmp/studio_shot.ppm (for headless visual inspection of
    // the Vulkan output, which X screen-grab can't read). Handler only sets a flag (async-signal-safe).
#if defined(SIGUSR1)
    std::signal(SIGUSR1, [](int) { jf::JAppWindow::s_captureRequest = true; });
#endif
    // Graceful external termination: flag only here; the housekeeping timer (below, once the window
    // exists) turns the flag into requestClose() → the unsaved-changes prompt (studio-graceful-kill).
    std::signal(SIGTERM, [](int) { g_termRequest = 1; });
    std::signal(SIGINT,  [](int) { g_termRequest = 1; });
    applyLogEnv();
    JGuiApplication app;

    // Persistent settings (JSON) — window geometry, colour-picker customs, unit prefs, etc. Loaded
    // BEFORE the window so its saved size/position can be restored.
    {
        jf::JSettings::instance().setPath(StudioPaths::settingsFile()).loadJson();
    }
    // Bridge the framework's numeric controls to the studio keymap: a spin box / slider asks
    // JWidget::valueKeyAction(ke) whether a key means increase/decrease, and we answer from the global
    // Keymap. One user-set binding (e.g. "." = increase) then reaches every numeric editor — the framework
    // spin boxes in the property dock AND the canvas widgets — without the framework knowing our bindings.
    jf::JWidget::s_valueKeyHook = [](const jf::JKeyEvent& ke) {
        using VA = jf::JWidget::JValueKeyAction;
        switch (Keymap::instance().action(ke)) {
            case Keymap::Action::IncreaseValue:      return VA::Increase;
            case Keymap::Action::DecreaseValue:      return VA::Decrease;
            case Keymap::Action::IncreaseValueLarge: return VA::IncreaseLarge;
            case Keymap::Action::DecreaseValueLarge: return VA::DecreaseLarge;
            default:                                 return VA::None;
        }
    };

    auto& settings = jf::JSettings::instance();
    // Persist preferences AS THEY CHANGE, not only at a clean exit — JSettings::set() writes memory and
    // fires onChange, but nothing hits disk until saveJson(). Without this, a hard kill (or any exit that
    // doesn't reach the saveJson() at the end of main) loses every setting changed this session, which
    // reads as "preferences don't persist". Coalesce to one write ~400 ms after the last change.
    static jf::JTimer prefsSaveTimer;
    prefsSaveTimer.onTick.connect([]{ jf::JSettings::instance().saveJson(); });
    settings.onChange.connect([](const std::string&, const jf::JVariant&){
        prefsSaveTimer.start(std::chrono::milliseconds(400), jf::JTimer::JMode::SingleShot);
    });
    // THE NAME ON THE WINDOW IS THE NAME ON THE LAUNCHER. "JFramework Studio" is what the toolkit is
    // called, not what this application is; the desktop entry says "jayECU Studio (JF)" and the title
    // bar disagreeing with it is the same confusion as the WM_CLASS one below, in a place the user
    // reads constantly. (The grouping identity comes from the executable name — see
    // JLinuxPlatformWindow::applicationClass — which is already "studio", matching StartupWMClass.)
    JAppWindow win("jayECU Studio (JF)", settings.get<int>("window.w", 1280), settings.get<int>("window.h", 800));
    // The image cache uploads textures, and a widget's render() is handed a primitive buffer, not the hal.
    // Give it the one the window owns, once, here — before anything can ask it for a picture.
    ImageCache::instance().setHal(&win.hal());

    std::function<void()> saveAllFn = []{};   // assigned to the real saveAll once ecu/tree/surfaces exist
    std::function<void()> saveLayoutFn = []{};  // the layout alone (File▸Save Layout) — bound with saveAll
    std::function<void(const std::string&)> openLayoutFn = [](const std::string&){};    // import a full dashboard.gui document
    std::function<void(const std::string&)> saveLayoutAsFn = [](const std::string&){};  // export the full document to a path
    if (!win.valid()) return -1;
    { const int wx = settings.get<int>("window.x", -1), wy = settings.get<int>("window.y", -1);
      if (wx >= 0 && wy >= 0) win.setWindowPos(wx, wy); }
    // The studio's text: Ubuntu Sans at 13px. The framework's default is Ubuntu (classic) at 14px, which
    // reads heavier and larger. A persisted ui.font wins; else load Ubuntu Sans; if that font is missing,
    // fall back to the default system font at the same size (the right SIZE, just a different face).
    // 13px draws what a freetype 11px font draws: stb_truetype's ScaleForPixelHeight takes pixel-size from
    // the font's ascent+descent, so it renders ~1px smaller at the same nominal px. Measured: stb@13px has
    // cap height 8 and x-height 6, the same as freetype@11px. Shared by startup + the runtime
    // Application-Font pick so a user-chosen font lands at the SAME size, not the framework's 14px default.
    constexpr float kAppFontPx = 13.0f;
    {
        const std::string uf = settings.get<std::string>("ui.font", "");
        const float upx = static_cast<float>(settings.get<int>("ui.fontPx", static_cast<int>(kAppFontPx)));   // persisted size
        // SHIPPED WITH THE STUDIO, not looked for in /usr/share/fonts. That path exists only on a Linux
        // that happens to have the Ubuntu fonts, so Windows fell back to Arial — wider than the face every
        // page was laid out in, so page text ran out of its boxes and was cut off. Every system now draws
        // the same face the pages were drawn in.
        const std::string ubuntuSans = resources::path("UbuntuSans.ttf");
        if (!uf.empty())                                  win.setAppFont(uf, upx);
        else if (win.setAppFont(ubuntuSans, upx))         { std::fprintf(stderr, "[font] loaded Ubuntu Sans @%.0fpx\n", upx); std::fflush(stderr); }
        else                                              { win.setAppFontSize(upx); std::fprintf(stderr, "[font] Ubuntu Sans missing — default face @%.0fpx\n", upx); std::fflush(stderr); }
    }
    PreferencesDialog::onPickAppFont = [&win, &settings]{   // Appearance▸Application Font → in-app font picker (NO zenity)
        // Lists the real installed fonts, previews the selection in the picker's own sample line, and on
        // Select applies the chosen face AND size to the whole app + persists both (empty path = built-in Default).
        win.openAppFontPicker([&win, &settings](std::string path, int px) {
            settings.set("ui.font", jf::JVariant(path));
            settings.set("ui.fontPx", jf::JVariant(px));
            win.showStatus("Application font set", 2000);
        });
    };
    PreferencesDialog::onClearAppFont = [&win, &settings, kAppFontPx]{   // the row's ✕: drop the override → built-in default face at the default size
        settings.set("ui.font", jf::JVariant(std::string()));
        settings.set("ui.fontPx", jf::JVariant(static_cast<int>(kAppFontPx)));
        win.setAppFontDefault(kAppFontPx);
        win.showStatus("Application font cleared", 2000);
    };

    // Theme: studio.style IS the app's Dark identity (orange accent). The Appearance▸Theme choice is
    // persisted and re-applied here at startup, so a picked theme survives a relaunch (previously the
    // stylesheet was applied unconditionally and the choice was never saved → every launch reset to Dark).
    // Resolved ONCE, beside the executable — a bare "studio.style" is only found when the working
    // directory happens to be the source tree, and when it is not the studio comes up silently wearing
    // the framework's default theme instead of its own.
    const std::string kStyleSheet  = resources::path("studio.style");         // the DARK palette
    const std::string kCommonSheet = resources::path("studio.common.style");  // proportions + accent
    // A THEME IS A PALETTE; THE PROPORTIONS ARE THE APP'S EITHER WAY. Light used to be
    // `JStyle::apply(JStyle::light())` and nothing else — it never loaded the app's stylesheet at all,
    // so it took the framework's defaults for all twenty-odd metrics in it. The visible symptom was
    // dock tabs: the sheet says `tabFill: left` (compact, at the near edge) and the framework default
    // is Fill (stretched across the bar), so the tabs changed shape with the theme. Every control,
    // label, tick box and tab in light was a different size from the same one in dark for the same
    // reason. Now both themes start from a palette and then take the studio's own proportions.
    auto applyTheme = [kStyleSheet, kCommonSheet](int idx) {
        const bool dark = (idx == 0) || (idx == 2 && PreferencesDialog::osPrefersDark());
        jf::JStyle t = dark ? jf::JStyle::dark() : jf::JStyle::light();
        if (dark) jf::loadStyleSheet(kStyleSheet, t, t);      // dark surfaces + text
        jf::loadStyleSheet(kCommonSheet, t, t);               // heights, radii, tabs, the orange accent
        jf::JStyle::apply(std::move(t));
        jf::JSettings::instance().set("appearance.theme", jf::JVariant(idx));      // persist (debounced flush + exit save)
    };
    PreferencesDialog::onThemeChanged = applyTheme;
    applyTheme(settings.get<int>("appearance.theme", 0));                          // restore the saved theme

    // THE INTERFACE SCALE. One number for how big the whole interface is: it re-measures the style's
    // metrics and rebuilds the font atlas against the same figure, so controls and the text in them grow
    // together and every framework-laid-out region reflows around the result. 0 = follow the screen,
    // which is the only sensible default when the same build runs on a 1080p laptop and a 4K panel.
    //
    // Applied AFTER the theme, because installing a theme re-applies the scale over it — do it the other
    // way round and the first frame comes up at 1x.
    auto applyUiScale = [&win](double pick) {
        const float dpi = win.screenScale();
        win.setUiScale(pick > 0.0 ? float(pick) : dpi);
        JLOGC("ui.scale", jf::JLogLevel::Info)
            << "interface scale " << jf::JStyle::uiScale() << " (screen DPI " << dpi
            << (pick > 0.0 ? ", overridden by preference)" : ", automatic)");
    };
    PreferencesDialog::onUiScaleChanged = applyUiScale;
    applyUiScale(settings.get<double>("ui.scale", 0.0));

    // Every yes/no question the studio asks, so its buttons say so rather than OK / Cancel.
    static const jf::JDialogOptions kYesNo = [] {
        jf::JDialogOptions o; o.okLabel = "Yes"; o.cancelLabel = "No"; return o;
    }();
    // A META THAT NEEDS A NEWER STUDIO is refused by MetaModel::loadFile (min_studio) — this is what the
    // person is told, wherever the load was: connecting, opening a meta, starting a tune.
    MetaModel::setStudioVersion(STUDIO_VERSION);
    static const auto tellNeedsStudio = [](const std::string& need) {
        // `need` is a version, or "newer than X" when the firmware's format is newer than this studio
        // reads and its min_studio does not name a version above this one.
        const bool vague = need.rfind("newer than", 0) == 0;
        jf::JDialog::message("A newer studio is needed",
            (vague ? std::string("This ECU's firmware needs a newer jayecu Studio than this one (" STUDIO_VERSION ").")
                   : "This ECU's firmware needs jayecu Studio " + need + " or newer. This studio is " STUDIO_VERSION ".") +
                "\n\nUpdate the studio (Preferences > Updates > Check now), then connect again.");
    };
    // THE STUDIO'S OWN UPDATES (JAppUpdater): ask GitHub, offer, download with the release's SHA256SUMS,
    // check, and install as the studio closes. JAYECU_UPDATE_URL points it at a pretend release for testing
    // (tools/fake_release.py).
    // ONE RELEASE CARRIES BOTH the studio packages and the firmware kits, and its TAG IS THE STUDIO'S
    // VERSION: this compares the tag with STUDIO_VERSION, so every release bumps the studio's version,
    // even one that only changes firmware. The kits carry their own versions (FirmwareFetch.h).
    static jf::JAppUpdater s_updater(win, { "jayecu Studio", STUDIO_VERSION,
                                            "https://api.github.com/repos/jaytektas/jayecu/releases/latest",
                                            "JAYECU_UPDATE_URL" });

    // ECU FIRMWARE KITS (FirmwareKits.h): the ones shipped beside the studio and the ones downloaded
    // since. Scanned on demand — a handful of folders — so a kit fetched a minute ago counts at once.
    static const auto allKits = [] {
        std::vector<fwkits::Kit> kits = fwkits::scan(resources::exeDir() + "firmware", true);
        const std::vector<fwkits::Kit> dl = fwkits::scan(StudioPaths::dataDir("firmware"), false);
        kits.insert(kits.end(), dl.begin(), dl.end());
        return kits;
    };
    // Every kit's meta into the meta library, under the name connect looks for ("<board> <hash>.meta").
    // Then an ECU already running a kit's firmware is recognised at once, without reading its SD card.
    // Only ever ADDS: a meta already in the library for that hash is the same layout and is left alone.
    // …and every kit's DASHBOARD into the dashboard library, named for its layout the same way
    // ("<board> <hash>.gui"), so an ECU is given the pages drawn for the firmware it actually runs.
    static const auto installKitMetas = [] {
        namespace fs = std::filesystem;
        const fs::path lib = StudioPaths::dataDir("meta"), dlib = StudioPaths::dataDir("dashboards");
        for (const fwkits::Kit& k : allKits()) {
            if (k.layoutHash.empty()) continue;
            const fs::path dest = lib / (k.board + " " + k.layoutHash + ".meta");
            std::error_code ec;
            if (!fs::exists(dest, ec)) fs::copy_file(k.meta, dest, ec);
            if (!k.dashboard.empty() && fs::exists(k.dashboard, ec)) {
                fs::create_directories(dlib, ec);
                const fs::path dd = dlib / (k.board + " " + k.layoutHash + ".gui");
                if (!fs::exists(dd, ec)) fs::copy_file(k.dashboard, dd, ec);
            }
        }
    };
    installKitMetas();
    // THE DASHBOARD FOR ONE LAYOUT, and the newest one when there is none. A dashboard is drawn for a
    // firmware's settings; the exact one is always preferred, and the newest (the newest kit's, else the
    // board's own from `make studio-meta`) is the fallback, which still binds by name and says on connect
    // what it cannot find.
    static const auto exactDashboard = [](const std::string& board, const std::string& hash) {
        return std::filesystem::path(StudioPaths::dataDir("dashboards")) / (board + " " + hash + ".gui");
    };
    static const auto newestDashboard = [](const std::string& board) -> std::string {
        std::error_code ec;
        fwkits::Kit k;
        if (fwkits::newestFor(allKits(), board, STUDIO_VERSION, k) && !k.dashboard.empty() &&
            std::filesystem::exists(k.dashboard, ec))
            return k.dashboard;
        const auto generic = std::filesystem::path(StudioPaths::dataDir("dashboards")) / (board + ".gui");
        return std::filesystem::exists(generic, ec) ? generic.string() : std::string();
    };

    // Check the firmware releases for kits newer than the ones on hand, for every board this studio has
    // met OR has firmware for — the shipped kits are what recovery installs on a virgin board, so they are
    // kept current too. `manual` = the Check now button, which reports every outcome; at startup only a
    // download is worth mentioning, and only in the status line — nothing is put on an ECU from here.
    static std::function<void(bool)> s_checkFirmware = [&win](bool manual) {
        std::vector<std::string> boards;
        for (const Ecu::Summary& e : Ecu::list())
            if (!e.board.empty() && std::find(boards.begin(), boards.end(), e.board) == boards.end())
                boards.push_back(e.board);
        for (const fwkits::Kit& k : allKits())
            if (!k.board.empty() && std::find(boards.begin(), boards.end(), k.board) == boards.end())
                boards.push_back(k.board);
        if (boards.empty()) {
            if (manual) win.showStatus("No ECU has been connected yet, so there is no firmware to look for", 6000);
            return;
        }
        fwfetch::fetchLatest(boards, allKits(), STUDIO_VERSION, StudioPaths::dataDir("firmware"),
            jf::JSettings::instance().get<bool>("updates.firmwareBeta", false),
            [&win, manual](const fwfetch::Result& r) {
                JLOGC("updates", jf::JLogLevel::Info)
                    << "firmware check: " << fwfetch::releasesUrl() << " -> latest '" << r.latest << "', "
                    << r.installed.size() << " kit(s) fetched" << (r.error.empty() ? "" : ", error: " + r.error);
                for (const std::string& n : r.needsStudio)
                    JLOGC("updates", jf::JLogLevel::Warn) << "firmware " << n;
                if (!r.installed.empty()) {
                    installKitMetas();
                    std::string list;
                    for (const std::string& k : r.installed) list += (list.empty() ? "" : ", ") + k;
                    win.showStatus("Downloaded ECU firmware " + list, 8000);
                } else if (!r.needsStudio.empty()) {
                    win.showStatus("Newer ECU firmware needs a newer studio \xE2\x80\x94 update the studio first", 8000);
                } else if (manual) {
                    if (r.noReleases)           win.showStatus("No firmware releases have been published yet", 5000);
                    else if (!r.error.empty())  win.showStatus("Could not check for firmware: " + r.error, 8000);
                    else                        win.showStatus("ECU firmware is up to date", 5000);
                }
            });
    };
    PreferencesDialog::onCheckForUpdates = [] { s_updater.check(true); s_checkFirmware(true); };

    // THE CONNECTED ECU'S FIRMWARE, from its identity reply. Set on every jayecu connect; the offer
    // below compares it with the newest kit once the tune has been read.
    static std::string s_ecuBoard, s_ecuFwVersion, s_ecuFwBuild, s_ecuLayout;
    static bool s_firmwareAsked = false;           // this connection has been asked already (reset on open)
    // Straight after the ECU identifies itself: if the studio has newer firmware for this board, ask.
    // Returns false when there is nothing to ask — the caller just carries on. When it does ask, the
    // caller stops and `resume` carries the connect on once the question is answered, whatever the
    // answer. No means "not now": the next connect asks again.
    // Yes hands the ECU to the upgrade (assigned once the link exists, further down).
    static std::function<void(const fwkits::Kit&, const std::string& identity)> s_startUpgrade;
    static std::function<void()> s_offerRecovery;   // an ECU found waiting in its bootloader
    static std::function<bool(const std::string&, std::function<void()>)> s_askFirmware =
        [&win](const std::string& identity, std::function<void()> resume) {
        if (!jf::JSettings::instance().get<bool>("updates.firmwareOnConnect", true)) return false;
        if (s_ecuBoard.empty() || s_ecuFwVersion.empty()) return false;
        fwkits::Kit kit;
        if (!fwkits::newestFor(allKits(), s_ecuBoard, STUDIO_VERSION, kit)) return false;
        if (!fwkits::isNewerThan(kit.version, s_ecuFwVersion)) return false;
        // Already running that image. The build alone is not enough: every build from an uncommitted tree
        // is "<hash>-dirty", so two different images share it — the layout tells those apart.
        if (!kit.build.empty() && kit.build == s_ecuFwBuild && kit.layoutHash == s_ecuLayout) return false;
        JLOGC("updates", jf::JLogLevel::Info) << "ECU " << s_ecuBoard << " runs " << s_ecuFwVersion
            << "; kit " << kit.version << (kit.shipped ? " (shipped)" : " (downloaded)") << " at " << kit.dir;
        // NO BLIND QUESTION. The offer is the report of what the update changes (FirmwareUpgrade reads
        // the tune — a read, nothing written — and shows it); "Not now" there hands the connect back.
        win.showStatus("Newer firmware is available for this ECU \xE2\x80\x94 checking what it changes", 5000);
        (void)resume;
        if (s_startUpgrade) s_startUpgrade(kit, identity);
        return true;
    };

    auto& g = app.sceneGraph();
    bootMark("window + theme up");

    // --- ECU model + link (declared early so the toolbar can drive the connection) --------------
    // The meta descriptor is loaded up front (it drives the Dictionary + telemetry decode). The
    // serial link and these objects outlive win.run().
    static MetaModel meta;
    // THE STUDIO STARTS EMPTY. No definition, no project, no tune — because nothing has said which ECU
    // this session is about. A definition arrives with the thing that names one: connecting (the ECU's
    // identity or signature decides), opening a project (its tune names the layout), or creating one
    // (the user picks the schema). Loading a definition before any of that happened is how the dictionary
    // came to show the last ECU you touched while the studio was connected to nothing.
    Cache::instance().setMeta(&meta);

    // Register the sigil providers so expressions resolve [#config], [$telemetry] and [@widget] through
    // MathEvaluator. The channel ones are stateless (read the live Cache); the @ widget one gets its
    // surface-searching lookup wired once surfaceTabs + the registry exist (below).
    static TelemetrySigilResolver g_telemetrySigils;
    static ConfigSigilResolver    g_configSigils;
    static WidgetSigilResolver    g_widgetSigils;
    MathEvaluator::instance().registerResolver(&g_telemetrySigils);
    MathEvaluator::instance().registerResolver(&g_configSigils);
    MathEvaluator::instance().registerResolver(&g_widgetSigils);
    static AppStateSigilResolver g_appSigils;      // % — the studio's own status (link, logging)
    MathEvaluator::instance().registerResolver(&g_appSigils);
    // "$*" in a template resolves to the element's PRIMARY channel (provides[0]), which differs from its
    // config key for a few sensors (boost_pressure -> boost_kpa). Query the live meta each time so it tracks
    // whatever ECU is connected; unmapped keys fall through to "$<key>".
    MathEvaluator::instance().setPrimarySignalResolver([](const std::string& key) -> std::string {
        const MetaModel* m = Cache::instance().meta();
        return m ? m->primarySignalForKey(key) : key;
    });
    // "$~" — the RAW channel a sensor's INPUT publishes, which is a fact about its wiring rather than
    // about the sensor: the interface says which pin pool, `source` indexes it, and the meta's pool
    // names the channel HardwareInput publishes for that pin ("AV3" -> hw_av3). A page can then show
    // the number arriving on the pin beside what the calibration makes of it, which is the only way to
    // check that a calibration is right.
    MathEvaluator::instance().setRawSignalResolver([](const std::string& key) -> std::string {
        const MetaModel* m = Cache::instance().meta();
        if (!m) return {};
        const std::string base = "sensors.sensor[" + key + "]";
        const int iface = static_cast<int>(Cache::instance().configValue(base + ".interface"));
        const int src   = static_cast<int>(Cache::instance().configValue(base + ".source"));
        if (src < 0) return {};
        const auto& ifaces = m->enumIds("sensor_interface");
        if (iface < 0 || iface >= static_cast<int>(ifaces.size())) return {};
        const std::vector<std::string>* pool = m->hwPoolSignals(ifaces[static_cast<size_t>(iface)]);
        if (!pool || src >= static_cast<int>(pool->size())) return {};
        return (*pool)[static_cast<size_t>(src)];
    });

    // HOW MANY FRAMES A BUS CARRIES, for canframes() in a page's enable condition. Counted from the
    // TUNE rather than from GenericCanPanel: the CAN Setup page is not that panel, the panel may not
    // even be built when the page asks, and the frame pool is config either way.
    MathEvaluator::instance().setCanFrameCounter([](int bus) {
        const Cache& c = Cache::instance();
        int n = 0;
        for (int i = 0; i < 96; ++i) {                    // the gc_frame pool (GenericCanPanel::kMaxFrames)
            const std::string f = "can.gc_frame[" + std::to_string(i) + "].";
            if (!(int(std::lround(c.configValue(f + "flags"))) & 1)) continue;   // M_USED clear = free slot
            if (int(std::lround(c.configValue(f + "bus"))) == bus) n++;
        }
        return n;
    });

    static EcuLink link;
    // PUTTING NEW FIRMWARE ON THE ECU (FirmwareUpgrade.h). While it runs it has first claim on the link's
    // identity, config-image, telemetry and write-failure signals (routed below); when it is over it hands
    // the ECU back and the normal connect carries on from the identity.
    static std::function<void(const std::string&)> s_onIdentity;
    static FirmwareUpgrade s_upgrade(link, FirmwareUpgrade::Ui{
        [&win](const std::string& t) { win.showStatus(t, 8000); },
        [&win](const std::string& title, const std::string& what) {
            // A NEW STEP IS A NEW HEADLINE. This used to reuse the open window and put the step in its NOTE,
            // so "Writing firmware 0.4.0 — do not unplug the ECU" stayed as the headline through the tune
            // push and the SD copy, and each of their progress bars read as the firmware being written
            // again. The framework's progress window cannot change its headline, so the step gets a fresh
            // window (dismiss() lets go of active() at once, so the next open is the new one).
            if (auto* d = jf::JProgressDialog::active()) d->dismiss();
            win.openModal<jf::JProgressDialog>(title, what);
        },
        [](int pct, const std::string& note) {
            if (auto* d = jf::JProgressDialog::active()) { d->setProgress(pct, 100); if (!note.empty()) d->setNote(note); }
        },
        [] { if (auto* d = jf::JProgressDialog::active()) d->dismiss(); },
        [](const std::string& title, const std::string& body, std::function<void()> yes, std::function<void()> no) {
            jf::JDialog::confirm(title, body, std::move(yes), std::move(no), kYesNo);
        },
        [](const std::string& title, const std::string& body, std::function<void()> then) {
            jf::JDialog::message(title, body, std::move(then));
        },
        [&win](FirmwareUpgrade::Ui::Changes c, std::function<void(bool)> proceed) {
            FirmwareChangesDialog::Side goes, comes;
            goes.heading   = "Going out: what your tune sets that firmware " + c.toVersion + " no longer has";
            goes.emptyText = "Nothing you have set is going out.";
            goes.report    = std::move(c.changes.retired);
            goes.meta      = c.oldMeta;
            goes.image     = std::move(c.tune);
            goes.pages     = c.oldPages;
            goes.valuesOnLeft = true;
            comes.heading   = "New in firmware " + c.toVersion + ": settings to configure";
            comes.emptyText = "Firmware " + c.toVersion + " adds no new settings.";
            comes.report    = std::move(c.changes.added);
            comes.meta      = c.newMeta;
            comes.image     = std::move(c.migrated);
            comes.pages     = c.newPages;
            comes.valuesOnLeft = false;
            win.openModal<FirmwareChangesDialog>(
                std::string("Firmware ") + c.toVersion + " is available for this ECU (it runs " + c.fromVersion + ")",
                std::move(goes), std::move(comes), std::string("Update firmware"), std::string("Not now"),
                std::string("Updating needs the ignition OFF and the ECU on USB power only. Your tune is saved first "
                            "and put back afterwards."),
                std::move(c.notes), std::move(proceed));
        },
        [&win](const std::string& identity) {
            if (!identity.empty() && link.isOpen()) s_onIdentity(identity);
            else win.setStatusText("Disconnected");
        },
        [&win](const std::string& title, const std::string& body, const std::string& yesLabel,
               const std::string& noLabel, std::function<void(bool, bool)> answer) {
            win.openModal<ChoiceDialog>(title, body,
                std::vector<ChoiceDialog::Choice>{ { noLabel,  jf::JDialogButtonBox::Role::Reject },
                                                   { yesLabel, jf::JDialogButtonBox::Role::Accept } },
                std::string("Remember my choice"),
                std::function<void(int, bool)>([answer](int i, bool remember) { answer(i == 1, remember); }));
        },
    });
    s_startUpgrade = [](const fwkits::Kit& kit, const std::string& identity) {
        s_upgrade.start(kit, identity, link.portName());
    };
    // RECOVERY. The bootloader does not say which board it is on, and firmware for the wrong board would
    // drive the wrong pins — so the board is never guessed between two. The question is WHAT HARDWARE IT
    // IS, and the answers are the board types the studio has firmware for: every kit names one. NOT the
    // ECUs it has met. Those are not hardware types — an ECU met running rusEFI is filed under its rusEFI
    // signature, an .ini import under "ts" — and a virgin board is by definition one it has never met, so
    // listing them offered "rusEFI jaytek.2026.08.12…" as a board to install onto, and asked only about the
    // boards already seen. Every board type is listed, in the one dialog that also installs.
    s_offerRecovery = [&win] {
        // ONE DIALOG: what is happening, which board it is, and Install. It used to be a board picker and
        // then a confirm about the board just picked — two questions for one decision.
        std::vector<fwkits::Kit> installable;   // the newest kit per board type this studio can install
        std::vector<std::string> tooNew;        // board types whose only kits need a newer studio
        for (const fwkits::Kit& k : allKits()) {
            if (k.board.empty()) continue;
            const bool seen = std::any_of(installable.begin(), installable.end(),
                                          [&](const fwkits::Kit& i) { return i.board == k.board; });
            if (seen || std::find(tooNew.begin(), tooNew.end(), k.board) != tooNew.end()) continue;
            fwkits::Kit best;
            if (fwkits::newestFor(allKits(), k.board, STUDIO_VERSION, best)) installable.push_back(best);
            else tooNew.push_back(k.board);
        }
        if (installable.empty()) {
            jf::JDialog::message("ECU waiting in its bootloader",
                tooNew.empty()
                    ? "An ECU is waiting in its bootloader, but this studio has no firmware for any board. Update "
                      "the studio (Preferences > Updates), then connect again."
                    : "An ECU is waiting in its bootloader, but the firmware this studio has needs a newer studio. "
                      "Update the studio (Preferences > Updates), then connect again.");
            return;
        }
        std::vector<std::string> rows;
        for (const fwkits::Kit& k : installable) rows.push_back(k.board + "  \xE2\x80\x94  firmware " + k.version);
        win.openModal<ListPickerDialog>(
            std::string("ECU waiting in its bootloader"),
            std::string("An ECU is waiting in its bootloader \xE2\x80\x94 put there with its BOOT button, or by a "
                        "firmware update that did not finish. Choose the board it is, and Install puts that "
                        "board's firmware on it.\n\nChoose carefully: firmware for another board drives the "
                        "wrong pins."),
            rows,
            std::function<void(int, std::string)>([installable](int i, std::string) {
                if (i >= 0 && i < int(installable.size())) s_upgrade.recover(installable[size_t(i)]);
            }),
            std::function<void()>{}, std::string("Install"));
    };
    link.configReadProgress.connect([](int done, int total) { s_upgrade.onConfigProgress(done, total); });
    // The port the last open() was given — recorded so the identity handler can file the ECU under
    // the port it actually answered on. It used to save a hardcoded "/dev/ttyACM0" no matter which
    // port had been opened, which is wrong on a second bench ECU and meaningless on Windows.
    static std::string s_openedPort;
    static Ecu* ecu = nullptr;
    static std::string activeTuneName;   // the loaded calibration's tune name; the ECU tune auto-saves back to it
    static bool s_verifyPending = false; // "Verify ↔ ECU" armed: the next configImageReady byte-diffs + re-pushes
    static std::string s_connTitle;      // "<board> <ver> (<hash>)" while connected — folded into the window title
    static std::string s_imageUid;       // which ECU the cache's config image belongs to (guards cross-ECU compares)
    static std::function<void()> g_invalidatePageSurface;   // repaint the page open in the MDI
    static std::function<void()> g_syncProps;               // point the inspector at the page being edited
    static std::function<void()> g_loadProjectDoc;   // load the open ECU's dashboard.gui (bound once loadDoc exists)
    // The project's host variables (pcvars.json), re-appliable: a meta load replaces config_ and takes
    // them with it, so they go back on after one. See where it is defined for what that cost.
    static std::function<void()> g_applyProjectPcVars;
    // MAINTAINED CONSTANTS ([ConstantsExtensions] maintainConstantValue): config values the TUNER keeps up
    // to date from an expression, evaluated against live data. rusEFI's calibration buttons work entirely
    // this way — "Grab Idle/Up" makes the ECU measure the pedal and publish the reading on
    // calibrationMode/calibrationValue; writing it into the configuration is the tuner's job, and until
    // something did it the button appeared to do nothing at all.
    struct MaintainedConstant { std::string target, expr; };
    // Replace whole-token occurrences of `name` in `expr` with a number. Whole-token so that a path never
    // matches inside a longer one — ts.tpsMax must not rewrite the middle of ts.tpsMaxAdjustment.
    static const auto _substituteToken = [](const std::string& expr, const std::string& name, double v) {
        auto isTok = [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.'
                                       || ch == '[' || ch == ']'; };
        std::string out;
        char buf[40];
        std::snprintf(buf, sizeof buf, "%.10g", v);
        for (size_t i = 0; i < expr.size(); ) {
            const size_t at = expr.find(name, i);
            if (at == std::string::npos) { out += expr.substr(i); break; }
            const bool leftOk  = at == 0 || !isTok(expr[at - 1]);
            const bool rightOk = at + name.size() >= expr.size() || !isTok(expr[at + name.size()]);
            out += expr.substr(i, at - i);
            out += (leftOk && rightOk) ? buf : name;
            i = at + name.size();
        }
        return out;
    };
    static std::vector<MaintainedConstant> g_maintained;
    // Leave the landing page for the surfaces. Same late-binding as above: the import path runs before
    // surfaceTabs is constructed, and opening a project has to switch the centre just as Open ECU does.
    static std::function<void()> g_showSurfaces;
    // Seed the navigation tree from the meta. Separate from loading a project document because the two
    // arrive at different times: connect opens the ECU (and its document, if any) the moment it has an
    // identity, but the META can lag — on a first connect it is fetched off the ECU's own SD card — and
    // until it lands there is no navigation_tree to seed from.
    static std::function<void()> g_seedNavFromMeta;
    // The layout's bindings against the loaded schema — run when a document loads AND when a schema lands,
    // since on connect the document comes first and the schema after it.
    static std::string g_docBuiltForLayout;
    static std::function<void()> g_checkLayoutBindings;
    // Load a schema file by path (bound once loadSchema exists, which is after the connect handler). The
    // rusEFI probe needs it: when an ECU answers with a signature the loaded definition knows nothing
    // about, the studio switches to the imported definition that DOES match it.
    static std::function<void(const std::string&)> g_loadSchemaPath;
    // Open a project: its definition, then its layout and tune. Bound once openTune exists.
    static std::function<void(Ecu*, const std::string&)> g_openProject;
    // Adopt the ECU a connection just identified: its project document, its tune name, its tree — but NOT
    // its saved tune image, because a connected ECU's own bytes are the truth and arrive next. Asks first
    // when the outgoing layout is dirty, and does nothing at all when this ECU is already the one open.
    // `after` runs once the project is up, so a caller can continue a connect that a prompt interrupted.
    static std::function<void(Ecu*, std::function<void()>)> g_adoptConnectedEcu;
    // The TunerStudio fallback, reachable from the link's async failure signal (see tryTsConnect).
    static std::function<bool()> g_tryTsConnect;
    // Pick an .ini, import it, connect — what the "no definition for this ECU" prompt does when accepted.
    static std::function<void()> g_importIniThenConnect;
    // The same, for the definition rusEFI publishes for this firmware: fetch, import, connect.
    static std::function<void(const std::string&)> g_downloadIniThenConnect;
    // SD schema resolve: the canonical meta filename
    // awaited from the ECU's SD, plus the install / give-up continuations set by identityReceived.
    static std::string s_pendingMetaFetch;
    // The same three-part shape as the meta fetch below, for the DASHBOARD. A studio installed from
    // a release has no firmware tree to have installed a layout from, so the only copy that exists
    // is the one `make push` wrote to the ECU's own SD card. Without this the very first connect on
    // a clean machine lands on a tree seeded from the meta and no pages at all — everything correct
    // and nothing to look at.
    static std::string s_pendingDashFetch;
    // Called when the SCHEMA has landed, not when the document is loaded. On a first connect
    // g_loadProjectDoc runs before any meta exists — there is no layout_hash to name the file with
    // at that point, so asking there fetched nothing and said nothing about why.
    static std::function<bool()> g_fetchShippedDash;   // true = a fetch is now in flight
    static std::function<void(const std::string&, const std::vector<uint8_t>&)> s_dashFetchInstall;
    static std::function<void(const std::string&, const std::vector<uint8_t>&)> s_metaFetchInstall;
    static std::function<void()> s_metaFetchFallback;
    static std::function<void()> s_dashFallback;   // the SD card had no dashboard: the release, then the newest
    // TS-protocol interop (imported-ini schemas): the link + Cache bridge, and the loaded schema's
    // "tsProtocol" block (object ⇒ connectFn speaks TS to this ECU instead of the omni protocol).
    static TsLink tsLink;
    static TsCacheBridge tsBridge(tsLink);
    // …and the link-state banner asks it too, so a TS session is not reported as no ECU at all.
    g_altLinkOpen = [] { return tsLink.isOpen(); };
    static jf::JJson s_tsProto;
    // May the TS compatibility path be tried at all? THE PREFERENCE, and nothing else. What definition
    // happens to be loaded says nothing about what is on the end of the wire: a TS definition left open
    // from a previous session must not tip a connect attempt away from the native protocol, and a native
    // definition must not stop the studio recognising a rusEFI board. The protocol is decided by what
    // ANSWERS — native identity first, then, if the user has asked us to look, a TS signature query.
    static auto tsFallbackAllowed = [] {
        return jf::JSettings::instance().get<bool>("connection.rusefiProbe", false);
    };
    // Re-read the just-loaded schema file for its protocol facts (present only on TS imports).
    auto noteTsProto = [](const std::string& path) {
        s_tsProto = jf::JJson();
        if (auto j = jf::JJson::tryParseFile(path); j && j->isObject() && j->contains("tsProtocol"))
            s_tsProto = (*j)["tsProtocol"];
    };
    // Nothing is loaded yet, so there is no protocol block to note: it arrives with the definition, which
    // arrives with the ECU or project the user picks.

    // ── THE ONE WAY A SAVED TUNE BECOMES THE STUDIO'S IMAGE ──────────────────────────────────────
    // Open ECU, the TunerStudio connect and the native connect all go through these. There used to be
    // three copies of this handful of lines and they had DRIFTED: the TS connect read the file, the
    // native connect read only what was already in memory, and "which tune is active" was resolved
    // three different ways. So a cold Connect — the button, straight from a fresh launch — reconciled
    // a perfectly good saved tune against an EMPTY vector, concluded there was nothing to protect, and
    // adopted the device's image over it in silence. One reader, one name, one guard: the paths cannot
    // drift apart again because there is only one of each.
    enum class TuneLoad { Ok, Missing, Unreadable };

    // WHICH calibration is this ECU's. "current" is what a new ECU folder gets.
    auto activeTuneFor = [](const Ecu* e) -> std::string {
        if (!e) return {};
        if (!e->lastActiveTune().empty()) return e->lastActiveTune();
        const std::vector<std::string> names = e->tuneNames();
        return names.empty() ? std::string("current") : names.front();
    };

    // FILE → IMAGE. A jayecu-tune dictionary is migrated against the meta — that is what lets a tune
    // outlive the layout it was saved on — and a legacy raw image passes straight through.
    auto tuneImage = [](const Ecu* e, const std::string& name, const MetaModel& m,
                        MigrationReport* rep) -> std::vector<uint8_t> {
        if (!e || name.empty()) return {};
        const std::vector<uint8_t> raw = e->loadTune(name);
        if (raw.empty() || !TuneFile::isDictFormat(raw)) return raw;
        MigrationReport scratch;
        return TuneFile::deserialise(raw, m, rep ? *rep : scratch);
    };

    // IMAGE → THE STUDIO. Stamps which ECU the cache belongs to, so a cross-ECU compare cannot happen,
    // and sets the restore baseline — preferring the newest connect-point snapshot, so
    // Edit▸Restore-to-Connect-Point still reaches back across a restart.
    auto loadTuneIntoCache = [tuneImage](const Ecu* e, const std::string& name, const MetaModel& m,
                                         MigrationReport* rep) -> TuneLoad {
        if (!e || name.empty() || e->loadTune(name).empty()) return TuneLoad::Missing;
        const std::vector<uint8_t> img = tuneImage(e, name, m, rep);
        if (img.empty()) return TuneLoad::Unreadable;
        Cache::instance().setConfigImage(img);
        Cache::instance().setBaseline(img);
        s_imageUid = e->uid();
        namespace fs = std::filesystem;
        std::error_code ec; fs::path newest;
        for (const auto& f : fs::directory_iterator(e->restoreDir(), ec))
            if (f.path().extension() == ".tune" &&
                (newest.empty() || f.path().filename().string() > newest.filename().string()))
                newest = f.path();
        if (!newest.empty()) {
            std::ifstream rf(newest, std::ios::binary);
            std::vector<uint8_t> b((std::istreambuf_iterator<char>(rf)), std::istreambuf_iterator<char>());
            if (!b.empty()) Cache::instance().setBaseline(b);
        }
        return TuneLoad::Ok;
    };

    // THE STUDIO'S SIDE OF THE RECONCILE, guaranteed to be there before the question is asked. An image
    // already stamped with this ECU is KEPT — those are the session's live edits, and they are what the
    // user means by "mine". Otherwise the ECU's saved tune is read off disk. Only when there is
    // genuinely no tune on disk does the reconcile see an empty local side, which is the one case where
    // "adopt the ECU's image" is an answer rather than a silent loss.
    auto ensureLocalTuneLoaded = [activeTuneFor, loadTuneIntoCache](const Ecu* e, const MetaModel& m) {
        if (!e) return;
        if (s_imageUid == e->uid() && Cache::instance().hasConfig()) return;   // live edits: already ours
        const std::string name = activeTuneFor(e);
        const TuneLoad r = loadTuneIntoCache(e, name, m, nullptr);
        JLOGC("ui.connect", jf::JLogLevel::Info) << "reconcile local side: '" << name << "' "
            << (r == TuneLoad::Ok ? "loaded " + std::to_string(Cache::instance().configImage().size()) + "B from disk"
              : r == TuneLoad::Missing ? std::string("has no file — the ECU's image will be adopted")
                                       : std::string("FAILED to read — the ECU's image will be adopted"));
    };

    // Persist the working config image to the active .tune — the calibration auto-save.
    // Silent + idempotent: a no-op unless an ECU, a tune name, a meta, and a
    // config image are all present. The image already holds every edit, so this writes the current tune.
    auto saveActiveTune = [] {
        if (!ecu || activeTuneName.empty()) return;
        const MetaModel* m = Cache::instance().meta();
        if (!m || !Cache::instance().hasConfig()) return;
        JLOGC("ui.tunesave", jf::JLogLevel::Debug) << "saveActiveTune \xE2\x86\x92 " << ecu->uid() << "/" << activeTuneName
            << " (" << Cache::instance().configImage().size() << "B)";
        ecu->saveTune(activeTuneName, TuneFile::serialise(Cache::instance().configImage(), *m));
        ecu->setLastActiveTune(activeTuneName);
    };

    // The single "make this ECU + tune the active project" routine — the port's MainWindow::openTune. Every
    // offline entry point (Open ECU, Recent ECUs, New Tune) funnels through here, so the save-outgoing +
    // set-active + load-the-named-tune sequence lives in ONE place. (The connect
    // path is separate — it takes its image live from the device, not a named .tune.)
    auto openTune = [&win, saveActiveTune, loadTuneIntoCache](Ecu* e, const std::string& tuneName) {
        if (!e) return;
        noteRecentEcu(e->label(), e->uid());   // this is the project to reopen next launch
        if (ecu && ecu != e && !activeTuneName.empty()) saveActiveTune();   // switching ECUs → save the outgoing tune first
        ecu = e;
        activeTuneName = tuneName;
        e->setLastActiveTune(tuneName);
        // Load the named tune into the cache: a .tune is a jayecu-tune/1 JSON document decoded against the
        // meta into a binary image (legacy raw-byte tunes fall through unchanged).
        const MetaModel* m = Cache::instance().meta();
        if (!m) { win.showStatus("No meta loaded — can't read that tune", 3500); return; }
        MigrationReport rep;
        const TuneLoad r = loadTuneIntoCache(e, tuneName, *m, &rep);
        if (r == TuneLoad::Missing)    { win.showStatus("Tune: " + tuneName, 2500); return; }
        if (r == TuneLoad::Unreadable) { win.showStatus("Could not parse tune: " + tuneName, 3500); return; }
        std::string status = "Loaded tune: " + tuneName;
        if (rep.needed())
            status += "  (migrated: " + std::to_string(rep.migrated) + " kept, "
                    + std::to_string(rep.defaulted) + " defaulted, "
                    + std::to_string(rep.unmapped.size()) + " dropped)";
        if (g_loadProjectDoc) g_loadProjectDoc();   // the project layout travels with the ECU folder
        win.showStatus(status, status.size() > 24 ? 5000 : 2500);   // longer dwell when a migration note is appended
    };

    // OPEN TUNE… — the tune picker for the ECU that is ALREADY open.
    //
    // Every other route to a named tune goes through Open ECU first: pick a device, then one of its
    // tunes. That is the wrong shape when the device is not in question — recovering last night's
    // calibration, or the connect-point snapshot taken before a reflash wiped the bank, is a question
    // about tunes and not about which ECU you are sitting in front of. Every part of it was already
    // here (tuneNames, loadTune, openTune, the picker); nothing offered them.
    //
    // THE RESTORE POINTS ARE IN THE SAME LIST, because when somebody wants "the tune from before" that
    // is usually which one they mean. They are raw images rather than dict documents, so they cannot
    // migrate across a layout change — the size check below is the only guard the file itself allows,
    // and it is an honest one: a raw image of the wrong size belongs to another firmware.
    std::function<void()> openTuneFn = [&win, openTune, saveActiveTune] {
        if (!ecu) { win.showStatus("No ECU project is open", 3000); return; }
        namespace fs = std::filesystem;
        std::error_code ec;

        // THE OPEN TUNE IS SAVED FIRST. openTune only saves the outgoing tune when the ECU changes, and
        // here it never does — so without this, opening a tune on the ECU you are already on throws
        // away every edit made since the last auto-save.
        saveActiveTune();

        std::vector<std::string> labels, kinds, names;
        for (const std::string& n : ecu->tuneNames()) {
            const bool open = (n == activeTuneName);
            labels.push_back((open ? "\xE2\x97\x8F " : "   ") + n + (open ? "   (open)" : ""));
            kinds.push_back("tune");
            names.push_back(n);
        }
        std::vector<fs::path> rps;
        for (const auto& f : fs::directory_iterator(ecu->restoreDir(), ec))
            if (f.path().extension() == ".tune") rps.push_back(f.path());
        std::sort(rps.begin(), rps.end(), [](const fs::path& a, const fs::path& b) {
            return a.filename().string() > b.filename().string();          // newest first
        });
        for (const fs::path& f : rps) {
            labels.push_back("   " + f.stem().string() + "   (connect point)");
            kinds.push_back("restore");
            names.push_back(f.string());
        }
        // THE TUNES SAVED BEFORE A FIRMWARE UPDATE. FirmwareUpgrade writes one to backups/ before it
        // flashes, and that is the file to reach for when an update stops short of putting the tune
        // back. Unlike a restore point it is a named-settings document, so it migrates onto the new
        // layout exactly as a saved tune does.
        std::vector<fs::path> bks;
        for (const auto& f : fs::directory_iterator(fs::path(ecu->dir()) / "backups", ec))
            if (f.path().extension() == ".tune") bks.push_back(f.path());
        std::sort(bks.begin(), bks.end(), [](const fs::path& a, const fs::path& b) {
            return fs::last_write_time(a) > fs::last_write_time(b);          // newest first
        });
        for (const fs::path& f : bks) {
            labels.push_back("   " + f.stem().string() + "   (firmware backup)");
            kinds.push_back("backup");
            names.push_back(f.string());
        }
        if (labels.empty()) { win.showStatus("This ECU has no saved tunes yet", 3000); return; }

        win.openModal<ListPickerDialog>(
            std::string("Open Tune \xC2\xB7 " + ecu->board()), labels, false,
            std::function<void(int, std::string)>([&win, openTune, kinds, names](int i, std::string) {
                if (i < 0 || i >= static_cast<int>(names.size())) return;
                const std::string& sel = names[static_cast<size_t>(i)];
                if (kinds[static_cast<size_t>(i)] == "tune") {
                    openTune(ecu, sel);
                } else if (kinds[static_cast<size_t>(i)] == "backup") {
                    // Copied into tunes/ under its own name and opened from there, so it is an ordinary
                    // tune from here on: migrated on load, auto-saved, and the backup itself untouched.
                    const fs::path src(sel);
                    const std::string name = src.stem().string();
                    std::error_code ec2;
                    fs::create_directories(fs::path(ecu->dir()) / "tunes", ec2);
                    fs::copy_file(src, fs::path(ecu->dir()) / "tunes" / src.filename(),
                                  fs::copy_options::overwrite_existing, ec2);
                    if (ec2) { win.showStatus("Could not copy " + name + ": " + ec2.message(), 5000); return; }
                    openTune(ecu, name);
                } else {
                    // A connect point: this layout's raw bytes, straight in. No migration is possible
                    // and none is pretended — its size is the whole of what can be checked.
                    const MetaModel* m = Cache::instance().meta();
                    if (!m) { win.showStatus("No meta loaded \xE2\x80\x94 can't read that image", 3500); return; }
                    std::ifstream rf(sel, std::ios::binary);
                    std::vector<uint8_t> img((std::istreambuf_iterator<char>(rf)),
                                             std::istreambuf_iterator<char>());
                    if (img.size() != static_cast<size_t>(m->configSize())) {
                        win.showStatus("That connect point is " + std::to_string(img.size())
                                       + " B and this firmware's tune is " + std::to_string(m->configSize())
                                       + " B \xE2\x80\x94 not loaded", 6000);
                        return;
                    }
                    Cache::instance().setConfigImage(img);
                    Cache::instance().setBaseline(img);
                    win.showStatus("Loaded connect point: " + fs::path(sel).stem().string(), 4000);
                }
                // AND IT IS STILL ONLY LOCAL. setConfigImage fills the cache; nothing has reached the
                // ECU. Offered rather than done, because it overwrites whatever the ECU is holding —
                // the same question the connect-time reconcile asks, in the same words.
                //
                // NEXT FRAME, NOT THIS ONE. This runs inside the picker's own accept callback, and the
                // modal that is mid-close swallows a modal opened from under it: the load happened, the
                // prompt never appeared, and the ECU silently kept its old tune.
                if (!link.isOpen()) return;
                // THE IMAGE THE USER CHOSE, CAPTURED HERE. Not re-read from the cache when the prompt
                // is answered: the CRC watcher polls the ECU continuously, sees a config it does not
                // recognise, and reloads the DEVICE's image over the freshly-opened tune — which is
                // exactly what happened the first time this ran, on a bank the flash had just wiped.
                // The prompt cannot be opened in this frame (a closing modal swallows it), so the tune
                // has to be carried to it rather than looked up again a frame later.
                const std::vector<uint8_t> chosen = Cache::instance().configImage();
                jf::jPostToNextFrame([&win, chosen] {
                win.openModal<ListPickerDialog>(
                    std::string("Send it to the ECU?"),
                    std::vector<std::string>{ "Push to the ECU \xE2\x80\x94 overwrite the ECU's tune",
                                              "Keep it local for now" }, false,
                    std::function<void(int, std::string)>([&win, chosen](int c, std::string) {
                        if (c != 0) { win.showStatus("Kept local \xC2\xB7 the ECU is unchanged", 4000); return; }
                        // …and put it back in the cache too, so a watcher reload cannot leave the
                        // screen showing the device's old values while the ECU holds the new ones.
                        Cache::instance().setConfigImage(chosen);
                        link.writeConfigImage(chosen);
                        win.showStatus("Pushed to the ECU \xE2\x80\xA6 (burn to keep it)", 5000);
                    }),
                    std::function<void()>([&win] { win.showStatus("Kept local \xC2\xB7 the ECU is unchanged", 4000); }),
                    std::string("Apply"));
                });
            }));
    };

    // The connection indicator lives in the toolbar (chip-in-ring; grey/amber/green/red by state).
    ConnectButton connectBtn(g, 32.f);   // 32px chip

    // --- Menu bar --------------------------------------------------------------
    // Tear-off menus: the saved preference (Preferences ▸ Appearance) drives the global switch. Applied
    // here at startup; the checkbox applies live thereafter.
    jf::JMenuManager::instance().setTearOffEnabled(
        jf::JSettings::instance().get<bool>("ui.tearOffMenus", kTearOffMenusDefault));

    // Menu items with a keyboard chord (Ctrl+N/O/S/Q, Ctrl+Z/Y, Ctrl+X/C/V, F1).
    // The JMenuShortcut only RENDERS in the row; the live accelerator goes through jShortcuts(), NOT
    // JMenuManager — the manager's pass fires before focus routing and would steal Ctrl+C/V/Z from a
    // focused text field, while jShortcuts() runs after the focused widget had first refusal.
    auto addChordItem = [&g](JMenu& m, const std::string& label, const char* chord, std::function<void()> fn) {
        const JKeySequence seq = JKeySequence::parse(chord);
        auto* it = m.add(g, label, JMenuShortcut{seq.key, seq.hasCtrl(), seq.hasAlt(), seq.hasShift()});
        it->onTriggered.connect(fn);
        // The key does what the item does, and only when the item may: a greyed or hidden item's
        // shortcut must not reach round the gate (MenuGate) that took it off the menu.
        jShortcuts().registerShortcut(seq, [it, fn = std::move(fn)] { if (it->isEnabled() && !it->isHidden()) fn(); });
        return it;
    };
    // The menus route to the active surface (created below); the indirection is bound after it exists.
    std::function<Surface*()> activeSurf = [] { return static_cast<Surface*>(nullptr); };
    std::function<void()>     onNewSurface = []{};   // View▸New Surface Tab — bound after surfaceTabs exists
    std::function<void()>     onDeleteSurface = []{}; // View▸Delete Surface… — ditto
    std::function<void()>     onOpenTriggerDesigner = []{};
    // Bound after the library dock is constructed (it lives further down); the menu is built first.
    static std::function<int(const std::string&)>  g_importTriggerWheels;
    static std::function<bool(const std::string&)> g_exportTriggerWheels;   // View▸Trigger Designer — opens the centre tool tab (bound after surfaceTabs)
    std::function<void()>     onOpenEngineCycle = []{};       // View▸Engine Cycle — the same, for the cycle view
    std::function<void()>     onOpenTriggerLog  = []{};       // View▸Trigger Log — the RAW log, same tab, same model
    std::function<void()>     onOpenKnockScope  = []{};       // View▸Knock Scope — the classifier's last shot
    std::function<void()>     onOpenAutotune    = []{};       // Tools▸Auto Tune — the VE autotuner
    std::function<void()>     onReopenSurface = []{};   // View▸Reopen Surface… — ditto
    std::function<void(bool)> onSetEditMode = [](bool){};  // Locked/Editing toolbar toggle — bound after surfaceTabs/docks exist
    std::function<void(const std::string&)> onNodeRemoved = [](const std::string&){};  // a tree node deleted — drop its pages/tabs (bound after surfaceTabs)
    // Home-surface layout file (auto-loaded/saved; also File▸Save / Open / Save As target).
    auto activeModel = [&activeSurf]() -> PanelModel* { auto* s = activeSurf(); return s ? s->model() : nullptr; };
    std::function<void()> openSchemaFn;   // "Open Schema…" — bound after the dictionary tree exists (below)
    std::function<void()> reseedFn;       // Help▸"Add Missing Navigation Entries…" — bound with the tree (below)
    std::function<void()> newTuneFn;      // "New Tune…"    — ditto
    std::function<void()> importIniFn;    // "Import TunerStudio .ini…" — ditto
    std::function<void()> closeProjectFn; // "Close Project" — back to the landing (bound after landingView)
    std::function<void()> connectFn;      // toggle the ECU link (shared by the toolbar + landing)
    // Unsaved-changes gate for destructive project switches (Close Project / Open Layout): runs `then`
    // straight through when the document is clean; otherwise prompts Save/Discard/Cancel first. The
    // pass-through default is replaced by the real prompting body once saveAll exists.
    std::function<void(std::function<void()>)> maybeSaveThen = [](std::function<void()> then){ then(); };

    // WHICH DEFINITION DESCRIBES THIS PROJECT. A saved tune records the layout_hash it was written
    // against, and every .meta records the layout it was generated from, so opening a project offline can
    // find its own definition instead of relying on whatever happened to be loaded. That reliance is why
    // the studio used to come up holding the last ECU's definition with nothing connected.
    auto tuneLayoutHash = [](const std::vector<uint8_t>& raw) -> std::string {
        if (raw.empty() || !TuneFile::isDictFormat(raw)) return {};
        const auto j = jf::JJson::tryParse(std::string(raw.begin(), raw.end()));
        return (j && j->isObject()) ? (*j)["layout_hash"].str() : std::string();
    };
    auto metaForLayoutHash = [](const std::string& hash) -> std::string {
        if (hash.empty()) return {};
        namespace fs = std::filesystem;
        const fs::path lib = StudioPaths::dataDir("meta");
        std::error_code ec;
        // THE NAME CARRIES THE HASH. A meta is saved as "<board> <layout_hash>.meta", so the library can
        // be searched by looking at the directory. This used to PARSE every meta in it — 16 files, 23 MB
        // of JSON — to read one string out of each, on every launch, and that was most of the wait
        // between the studio's window appearing and its interface arriving with it.
        //
        // Parsing is still the answer for a file whose name does not name its hash: an older library, or
        // one somebody has renamed. It just is not the FIRST answer any more.
        std::vector<std::string> unnamed;
        for (const auto& f : fs::directory_iterator(lib, ec)) {
            if (f.path().extension() != ".meta") continue;
            const std::string stem = f.path().stem().string();
            if (stem.size() >= hash.size()
                && stem.compare(stem.size() - hash.size(), hash.size(), hash) == 0
                && (stem.size() == hash.size() || stem[stem.size() - hash.size() - 1] == ' '))
                return f.path().string();
            unnamed.push_back(f.path().string());
        }
        for (const std::string& path : unnamed) {
            const auto j = jf::JJson::tryParseFile(path);
            if (!j || !j->isObject()) continue;
            const jf::JJson& doc = *j;                     // const: the mutating operator[] would rewrite it
            if (doc["meta"]["layout_hash"].str() == hash) return path;
        }
        return {};
    };

    // OPEN A PROJECT: bring up the definition it was written against, then the project and its tune. Used
    // by Open ECU…, the Recent ECUs submenu and reopen-on-launch — each of which used to call openTune
    // directly and inherit whatever definition happened to be loaded.
    g_openProject = [&win, openTune, tuneLayoutHash, metaForLayoutHash](Ecu* e, const std::string& name) {
        if (!e) return;
        // The definition FIRST — a tune is a set of named values that means nothing without the layout it
        // was written for, and openTune needs it to decode the file at all.
        const std::string hash = tuneLayoutHash(e->loadTune(name));
        const MetaModel* cur = Cache::instance().meta();
        if (!cur || !cur->isValid() || (!hash.empty() && cur->layoutHash() != hash)) {
            const std::string found = metaForLayoutHash(hash);
            if (!found.empty()) {
                if (g_loadSchemaPath) g_loadSchemaPath(found);
            } else {
                // Nothing in the library describes it. Ask, rather than opening a project whose values
                // cannot be read — the alternative is a screen full of zeroes.
                win.showStatus("No definition for this project (layout " + hash + ") \xE2\x80\x94 choose its meta", 6000);
                jf::JDialog::openFile("Open meta for layout " + hash, { "meta", "json" },
                    [e, name, openTune](std::string path) {
                        if (g_loadSchemaPath) g_loadSchemaPath(path);
                        openTune(e, name);
                        if (g_showSurfaces) g_showSurfaces();
                    });
                return;
            }
        }
        bootMark("openProject: meta ready");
        openTune(e, name);
        bootMark("openProject: tune opened");
        if (g_showSurfaces) g_showSurfaces();
    };

    // WHICH LAYOUT IS THIS ECU? Asked of the library entry, without connecting to anything.
    //
    // Two places say so. A detached entry names it in its own uid — "offline:<hash>" for a tune started
    // against a schema, "tsimport:<hash>" for an imported definition — and any ECU that has been opened
    // before has a dashboard stamped with the layout its pages were built for. A real ECU that has never
    // been opened has neither, and the honest answer there is "unknown": the layout lives on the device.
    //
    // Scanned, not parsed: a native ECU's dashboard is 8 MB of JSON and this needs eight characters of it.
    auto ecuLayoutHash = [](Ecu* e) -> std::string {
        if (!e) return {};
        const std::string uid = e->uid();
        const size_t colon = uid.rfind(':');
        if (colon != std::string::npos && colon + 1 < uid.size()) return uid.substr(colon + 1);
        std::ifstream in(e->dashboardPath());
        if (!in) return {};
        std::string doc((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const size_t at = doc.find("\"builtFor\"");
        if (at == std::string::npos) return {};
        const size_t key = doc.find("\"layout_hash\"", at);
        if (key == std::string::npos) return {};
        const size_t open_q = doc.find('"', doc.find(':', key));
        const size_t close_q = open_q == std::string::npos ? std::string::npos : doc.find('"', open_q + 1);
        if (close_q == std::string::npos) return {};
        return doc.substr(open_q + 1, close_q - open_q - 1);
    };

    // Open an ECU by uid/board → pick one of its tunes → open that project.
    std::function<void(std::string, std::string)> openEcuByIdBoard =
        [&win, openTune, tuneLayoutHash, metaForLayoutHash, ecuLayoutHash](std::string uid, std::string board) {
        Ecu* opened = Ecu::openOrCreate(uid, board);
        if (!opened) { win.showStatus("Could not open ECU", 3000); return; }
        auto tunes = opened->tuneNames();
        // NO SAVED TUNE IS NOT A DEAD END. An entry can have none because its tunes were deleted, or
        // because it never had one — an imported definition is in the library the moment it is imported.
        // What the entry still has is its LAYOUT, and a layout's own defaults ARE a tune: it is exactly
        // what File ▸ New Tune builds against a schema. So build it here rather than refusing, under the
        // ECU the user picked, and carry on into the ordinary open.
        //
        // The defaults are a STARTING POINT and nothing more — they are one tune out of millions, and the
        // name says so rather than pretending this is the ECU's tune.
        if (tunes.empty()) {
            const std::string hash = ecuLayoutHash(opened);
            const std::string metaPath = metaForLayoutHash(hash);
            // Seed the ECU from a definition and open it. Shared by both routes below so the file picker
            // cannot drift from the library hit.
            auto seedAndOpen = [&win, opened](const std::string& metaPath) {
                if (g_loadSchemaPath) g_loadSchemaPath(metaPath);
                const MetaModel* m = Cache::instance().meta();
                if (!m || !m->isValid()) { win.showStatus("Could not load that definition", 4000); return; }
                const std::string seeded = "defaults";
                opened->saveTune(seeded, TuneFile::serialise(m->defaultImage(), *m));
                win.showStatus("No saved tune \xE2\x80\x94 opened this ECU's defaults", 5000);
                if (g_openProject) g_openProject(opened, seeded);
            };
            if (metaPath.empty()) {
                // NOTHING IN THE LIBRARY DESCRIBES IT. Ask for the file rather than stopping: the same
                // answer g_openProject gives when a tune's layout is one the library has never seen. An
                // entry whose layout is not even known (a real ECU nobody has opened) has no question to
                // ask — the layout is on the device, so connecting is the only way to learn it.
                if (hash.empty()) {
                    win.showStatus("That ECU has no saved tune, and nothing here says which definition it "
                                   "uses \xE2\x80\x94 connect to it, or start one with File \xE2\x96\xB8 New Tune", 7000);
                    return;
                }
                win.showStatus("No saved tune, and no definition for layout " + hash
                               + " \xE2\x80\x94 choose its meta", 6000);
                jf::JDialog::openFile("Open meta for layout " + hash, { "meta", "json" },
                                      [seedAndOpen](std::string path) { seedAndOpen(path); });
                return;
            }
            seedAndOpen(metaPath);
            return;
        }
        // NEXT FRAME. This runs inside the ECU picker's own accept callback, and a modal that is
        // mid-close swallows one opened from under it — so picking an ECU dismissed the list and
        // nothing else ever appeared. File ▸ Open ECU… could not open a project at all.
        jf::jPostToNextFrame([&win, opened, tunes, openTune] {
        win.openModal<ListPickerDialog>(std::string("Choose a tune"), tunes, false,
            std::function<void(int, std::string)>([&win, opened, tunes, openTune](int ti, std::string) {
                if (ti < 0 || ti >= static_cast<int>(tunes.size())) return;
                if (g_openProject) g_openProject(opened, tunes[ti]);
            }));
        });
    };
    // Open ECU…: browse the local ECU library → pick an ECU → open it.
    std::function<void()> openEcuFn = [&win, &openEcuByIdBoard]{
        auto ecus = Ecu::list();
        if (ecus.empty()) { win.showStatus("No ECUs in the library", 3000); return; }
        std::vector<std::string> labels;
        for (const auto& e : ecus)
            labels.push_back(e.board + "  \xC2\xB7  uid \xE2\x80\xA6" + (e.uid.size() > 6 ? e.uid.substr(e.uid.size() - 6) : e.uid)
                             + (e.lastSeen.empty() ? "" : "  \xC2\xB7  " + e.lastSeen));
        win.openModal<ListPickerDialog>(std::string("Open an existing ECU"), labels, false,
            std::function<void(int, std::string)>([&win, ecus, &openEcuByIdBoard](int idx, std::string) {
                if (idx >= 0 && idx < static_cast<int>(ecus.size())) openEcuByIdBoard(ecus[idx].uid, ecus[idx].board);
            }));
    };
    // Recent ECUs submenu — built from the persisted list (most-recent-first, "board\tuid" per line). Each
    // item reopens that ECU. The list is refreshed on connect (below) for the next launch.
    JMenu recentMenu("Recent ECUs");
    { const std::string rec = settings.get<std::string>("recent.ecus", "");
      size_t p = 0; int n = 0;
      while (p < rec.size() && n < 8) {
          const size_t nl = rec.find('\n', p);
          const std::string line = rec.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
          p = (nl == std::string::npos) ? rec.size() : nl + 1;
          const size_t tab = line.find('\t');
          if (tab == std::string::npos) continue;
          const std::string board = line.substr(0, tab), uid = line.substr(tab + 1);
          if (uid.empty()) continue;
          recentMenu.add(g, board + " \xC2\xB7 \xE2\x80\xA6" + (uid.size() > 6 ? uid.substr(uid.size() - 6) : uid))
              ->onTriggered.connect([&openEcuByIdBoard, uid, board]{ openEcuByIdBoard(uid, board); });
          ++n;
      } }

    // FILE is the open ECU's documents and nothing else:
    //   [New Tune · Open ECU · Recent ECUs · Open Tune] | [Save Tune] | [Open / Save / Save As Layout] | [Close ECU] | [Quit]
    // Definitions and trigger wheels are the Library; adding the definition's missing navigation entries is
    // under Help, in edit mode only.
    JMenu fileMenu("File");
    addChordItem(fileMenu, "New Tune…", "Ctrl+N", [&]{ if (newTuneFn) newTuneFn(); });
    addChordItem(fileMenu, "Open ECU…", "Ctrl+O", [&]{ if (openEcuFn) openEcuFn(); });
    addChordItem(fileMenu, "Open Tune…", "Ctrl+Shift+O", [&]{ if (openTuneFn) openTuneFn(); });
    fileMenu.add(g, "Recent ECUs", {}, &recentMenu);
    fileMenu.addSeparator(g);
    // SAVE SAYS WHAT IT SAVES. The studio keeps two kinds of document per ECU -- the tune (the
    // calibration) and the layout (the pages, tree and surfaces) -- and a bare "Save" wrote both without
    // saying so. Each now has its own item; the close/quit guard still offers to save whichever is dirty.
    addChordItem(fileMenu, "Save Tune", "Ctrl+S", [&]{
        if (!ecu || activeTuneName.empty()) { win.showStatus("No tune is open", 2000); return; }
        saveActiveTune();
        win.showStatus("Saved tune " + activeTuneName, 1500);
    });
    // SAVE AS: the open tune, under a new name, which then becomes the open one. The only other way to a
    // second named tune was New Tune, which starts from a firmware's DEFAULTS — so a track tune could not
    // be made from the road tune without copying files by hand in the data folder.
    fileMenu.add(g, "Save Tune As\xE2\x80\xA6")->onTriggered.connect([&win, saveActiveTune]{
        if (!ecu || activeTuneName.empty()) { win.showStatus("No tune is open", 2000); return; }
        jf::JDialog::input("Save Tune As", "Name for this copy of \"" + activeTuneName + "\"",
            [&win, saveActiveTune](std::string name) {
                while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))  name.pop_back();
                while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) name.erase(0, 1);
                if (name.empty()) return;
                // A file name, so nothing that names a folder or that Windows refuses.
                if (name.find_first_of("/\\:*?\"<>|") != std::string::npos) {
                    win.showStatus("A tune name cannot contain / \\ : * ? \" < > |", 5000); return;
                }
                if (!ecu) return;
                for (const std::string& n : ecu->tuneNames())
                    if (n == name) { win.showStatus("There is already a tune called \"" + name + "\"", 5000); return; }
                activeTuneName = name;
                saveActiveTune();
                win.showStatus("Saved as \"" + name + "\" \xE2\x80\x94 now the open tune", 4000);
            }, {}, activeTuneName + " copy");
    });
    fileMenu.addSeparator(g);
    fileMenu.add(g, "Open Layout…")->onTriggered.connect([&]{   // replaces the working document → guard unsaved edits
        maybeSaveThen([&]{ jf::JDialog::openFile("Open Layout", {"json", "gui"}, [&](std::string path) { openLayoutFn(path); }); });
    });
    addChordItem(fileMenu, "Save Layout", "Ctrl+Shift+S", [&]{ saveLayoutFn(); });
    fileMenu.add(g, "Save Layout As…")->onTriggered.connect([&]{
        jf::JDialog::saveFile("Save Layout", {"gui", "json"}, [&](std::string path) { saveLayoutAsFn(path); });
    });
    fileMenu.addSeparator(g);
    fileMenu.add(g, "Close ECU")->onTriggered.connect([&]{ if (closeProjectFn) closeProjectFn(); });   // back to the landing
    fileMenu.addSeparator(g);
    addChordItem(fileMenu, "Quit", "Ctrl+Q", [&win]{ win.requestClose(); });

    // The page behind every navigation node (one node → many viewports). Point the data-driven viewport
    // widget's global resolver at it, so a placed viewport can draw its node's page. Persisted per-ECU below.
    static PanelLibrary panelLibrary;
    g_pagesForDiff = &panelLibrary;          // the tune-difference report groups by page; see above
    nodeviewport::resolver() = [](const std::string& path) -> const PanelModel* { return panelLibrary.find(path); };

    // LIBRARY — what the studio KEEPS, as opposed to what it does: trigger wheels, and the ECU
    // definitions it reads tunes through. None of it belongs under File, which is the open ECU's
    // documents (tune, layout).
    JMenu libraryMenu("Library");
    jf::JMenuItem* libTriggerLibrary = libraryMenu.add(g, "Trigger Library");   // shows the panel; bound with the docks
    // Trigger wheels move between machines as a plain file — the same JSON the meta and the user
    // store use, so an exported wheel can also be pasted straight into a schema's trigger_wheels.
    libraryMenu.add(g, "Import Trigger Wheels\xE2\x80\xA6")->onTriggered.connect([&win]{
        jf::JDialog::openFile("Import Trigger Wheels", {"json"}, [&win](std::string path) {
            const int n = g_importTriggerWheels ? g_importTriggerWheels(path) : -1;
            win.setStatusText(n < 0 ? "could not read " + path
                                    : "imported " + std::to_string(n) + " trigger wheel(s)");
        });
    });
    libraryMenu.add(g, "Export Trigger Wheels\xE2\x80\xA6")->onTriggered.connect([&win]{
        jf::JDialog::saveFile("Export Trigger Wheels", {"json"}, [&win](std::string path) {
            const bool ok = g_exportTriggerWheels && g_exportTriggerWheels(path);
            win.setStatusText(ok ? "exported to " + path : "could not write " + path);
        });
    });
    libraryMenu.addSeparator(g);
    libraryMenu.add(g, "Load ECU Definition…")->onTriggered.connect([&]{ if (openSchemaFn) openSchemaFn(); });   // a firmware schema (raw meta)
    libraryMenu.add(g, "Import TunerStudio .ini…")->onTriggered.connect([&]{ if (importIniFn) importIniFn(); });

    JMenu editMenu("Edit");
    addChordItem(editMenu, "Undo", "Ctrl+Z", [&activeSurf]  {
        if (auto* s = activeSurf(); s && s->canUndo()) s->menuUndo();
        else if (g_treeUndo.canUndo()) g_treeUndo.undo(); });
    addChordItem(editMenu, "Redo", "Ctrl+Y", [&activeSurf]  {
        if (auto* s = activeSurf(); s && s->canRedo()) s->menuRedo();
        else if (g_treeUndo.canRedo()) g_treeUndo.redo(); });
    editMenu.addSeparator(g);
    addChordItem(editMenu, "Cut", "Ctrl+X",   [&activeSurf] { if (auto* s = activeSurf()) s->menuCut(); });
    addChordItem(editMenu, "Copy", "Ctrl+C",  [&activeSurf] { if (auto* s = activeSurf()) s->menuCopy(); });
    addChordItem(editMenu, "Paste", "Ctrl+V", [&activeSurf] { if (auto* s = activeSurf()) s->menuPaste(); });
    editMenu.addSeparator(g);
    // Restore the whole tune to the connect-point baseline (refresh controls + push the image to the ECU).
    editMenu.add(g, "Restore Tune to Connect Point")->onTriggered.connect([&win]{   // link is static — no capture
        Cache& c = Cache::instance();
        if (!c.hasBaseline()) { win.showStatus("No restore point — connect to an ECU first.", 4000); return; }
        c.setConfigImage(c.baseline());                         // refresh all controls from the baseline
        if (link.isOpen()) link.writeConfigImage(c.baseline()); // push the whole image to the ECU
        win.showStatus("Tune restored to the connect point", 4000);
    });
    editMenu.addSeparator(g);
    editMenu.add(g, "Preferences…")->onTriggered.connect([&win]{ win.openModal<PreferencesDialog>(); });
    // The table context menu's "Key Bindings…" item opens its modal here (the dialog edits the global
    // Keymap, so it needs no surface state). Set-and-forget: any table on any surface shares it. (global Keymap singleton)
    Surface::onEditTableKeyBindings = [&win]{ win.openModal<KeyBindingsDialog>(); };

    // The table context menu's "Table Axis Setup…" item opens its modal here, seeded with the table's path so
    // the dialog resolves that table and edits its axes (channel / enable / breakpoints) live through Cache.
    Surface::onAxisSetup = [&win](std::string path, std::vector<std::string> axisUnits, std::string cellUnit,
                                  std::function<void(std::string, std::string)> onUnitPicked) {
        win.openModal<AxisSetupDialog>(std::move(path), std::move(axisUnits), std::move(cellUnit),
                                       std::move(onUnitPicked));
    };
    // The axis dialog's own nested modals — it has no window of its own to open them from. openModal
    // parents to the top-of-stack modal, so these land above the axis editor rather than behind it.
    AxisSetupDialog::onAskValue = [&win](double seed, Cache::ChannelDomain dom, std::function<void(double)> cb) {
        win.openModal<InsertValueDialog>(seed, std::move(dom), std::move(cb));
    };
    AxisSetupDialog::onAskAxis = [&win](double s0, double e0, double inc, Cache::ChannelDomain dom,
                                        std::function<void(double, double, double)> cb) {
        win.openModal<AxisWizardDialog>(s0, e0, inc, std::move(dom), std::move(cb));
    };

    // A wiring widget's Assign button opens the Select Connection dialog, seeded with the field it binds;
    // the dialog lists that field's capability-gated board resources and writes the chosen pool value.
    WiringWidget::onAssign = [&win](std::string bind){ win.openModal<SelectConnectionDialog>(std::move(bind)); };

    // …and a wizard button opens the output slot's personality dialog, seeded with the slot it binds.
    // Same shape: the widget layer names a target, the app owns the window.
    WizardButtonWidget::onOpen = [&win](std::string bind){ win.openModal<OutputWizardDialog>(std::move(bind)); };

    // The edit-mode "Condition…" item opens the visibility-condition editor for the selected control, seeded
    // with the current condition + the full channel list; the apply callback writes it back as one undo step.
    Surface::onEditCondition = [&win](std::string cur, std::string element,
                                      std::function<void(std::string)> apply){
        win.openModal<ExpressionEditor>(std::move(cur),
            buildSigilTree(meta, g_widgetSigils.lister ? g_widgetSigils.lister() : std::vector<std::string>{}),
            std::move(apply), ExpressionEditor::FirmwareTarget{},    // host-evaluated: live value feedback
            std::move(element));                                     // …against the page's own element
    };
    // The SAME editor, told the expression will run on the ECU: the feedback line reports whether it
    // compiles, how much of the program block it uses, and what the firmware will actually run, and
    // OK refuses a program the ECU would reject. One editor, one grammar — see ExprAst.h.
    Surface::onEditExpression = [&win](std::string cur, uint16_t blockSize, std::string element,
                                       std::function<void(std::string)> apply){
        win.openModal<ExpressionEditor>(std::move(cur),
            buildSigilTree(meta, g_widgetSigils.lister ? g_widgetSigils.lister() : std::vector<std::string>{},
                           /*firmware=*/true),
            std::move(apply), ExpressionEditor::FirmwareTarget{&meta, blockSize}, std::move(element));
    };
    Surface::onEditPresets = [&win](std::string cur, std::function<void(std::string)> apply){
        // A preset names CONFIG fields, so the dialog gets the same source picker the Properties dock uses
        // rather than asking anyone to type a path and a raw value from memory.
        win.openModal<PresetEditorDialog>(std::move(cur), std::move(apply),
            [&win](std::string current, std::function<void(std::string)> onPick){
                win.openModal<PopupSignalPicker>(buildConfigTree(meta),
                                                 std::move(current), std::move(onPick), "Select Field");
            });
    };
    Surface::onEditLines = [&win](std::string cur, std::function<void(std::string)> apply){
        win.openModal<LineEditorDialog>(std::move(cur), channelPaths(meta), std::move(apply));
    };
    Surface::onEditPanel = [&win](std::string cur, std::function<void(std::string)> apply){
        win.openModal<PanelContentsDialog>(std::move(cur), widgetTypes(), channelPaths(meta), std::move(apply));
    };
    // Run-mode enum/signal selection: a *_src signal selector opens the same
    // searchable signal picker as the Properties source field; a plain enum opens a compact option list.
    enumpick::signal() = [&win](std::vector<std::string> paths, std::string current, std::function<void(std::string)> onPick) {
        // No clear here: unassigning is the combo's own ✕ (JComboBox::setClearable), on the control.
        win.openModal<PopupSignalPicker>(buildSourceTree(paths, meta), std::move(current), std::move(onPick),
                                         "Select Source");
    };
    // The CHANNEL SET of a multi-channel control — the two-list picker, off the control's own context menu
    // ("Channel Setup" on a watch list, "Select Channels" on a trace view). The catalogue is the telemetry
    // descriptor: every channel the ECU actually sends, with the name, group and units it declares, so the
    // columns say what a channel IS rather than what the bus calls it.
    enumpick::channels() = [&win](std::string title, std::vector<std::string> current,
                                  std::function<void(std::vector<std::string>)> onPick) {
        std::vector<ChannelPickerDialog::Chan> all;
        for (const auto& [key, f] : meta.telemetry())
            all.push_back({ key, f.label.empty() ? key : f.label, f.module, f.units });
        win.openModal<ChannelPickerDialog>(std::move(title), std::move(all), std::move(current), std::move(onPick));
    };
    // The PROPERTIES of the channels a control is showing. Unit choices are the ones the channel can
    // actually be read in (its quantity's units), so picking one converts rather than relabels.
    enumpick::channelProps() = [&win](std::string title, std::vector<std::string> channels,
                                      std::function<void()> onChanged) {
        std::vector<ChannelPropsDialog::Row> rows;
        for (const std::string& ch : channels) {
            ChannelPropsDialog::Row r; r.channel = ch; r.label = ch;
            const auto it = meta.telemetry().find(ch);
            if (it != meta.telemetry().end()) {
                if (!it->second.label.empty()) r.label = it->second.label;
                r.unit = it->second.units;
            }
            if (!r.unit.empty()) {
                r.units.push_back(r.unit);                       // first entry = the channel's own
                const std::string q = UnitManager::instance().findQuantityForUnit(r.unit);
                for (const auto& u : UnitManager::instance().getQuantity(q).units)
                    if (u.id != r.unit) r.units.push_back(u.id);
            }
            rows.push_back(std::move(r));
        }
        win.openModal<ChannelPropsDialog>(std::move(title), std::move(rows), std::move(onChanged));
    };
    enumpick::menu() = [&win, &g](std::string /*title*/, std::vector<std::string> labels, int /*cur*/,
                                  int sx, int sy, std::function<void(int)> onPick,
                                  std::vector<uint8_t> disabled) {
        // Plain enum → a lightweight floating popup MENU at the click (a compact option list),
        // not a modal dialog. The menu outlives the popup (static, rebuilt each open); each item commits its
        // index. Screen coords = window origin + the widget-local click.
        static std::unique_ptr<JMenu> s_enumMenu;
        s_enumMenu = std::make_unique<JMenu>("");
        for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
            jf::JMenuItem* mi = s_enumMenu->add(g, labels[i]);
            mi->onTriggered.connect([onPick, i]{ onPick(i); });
            // Greyed = shown but not choosable (a pin another sensor holds). JMenuItem's Disabled state
            // both dims it and swallows the activation, so the entry explains itself and cannot be picked.
            if (i < static_cast<int>(disabled.size()) && disabled[i]) mi->setEnabled(false);
        }
        if (JMenuManager::instance().onOpenMenu)
            // Anchored UNDER the enum control (like a combo popup), not at the cursor — so an off-screen
            // one slides back inside rather than flipping a whole menu-width away from its field.
            JMenuManager::instance().onOpenMenu(s_enumMenu.get(), win.windowX() + sx, win.windowY() + sy,
                                                false, /*pointAnchored=*/false);
    };
    // Command button's showMessageOnClick → the framework message dialog (a descriptor has no window).
    appmsg::show() = [](std::string title, std::string text) { jf::JDialog::message(title, text); };
    // Raw controller commands from an IMPORTED project's buttons (bench tests, resets, Lua triggers). They
    // are the controller's own instructions, so they go out over the TS link exactly as given, in order —
    // no envelope of ours, no acknowledgement required (several of these deliberately do not reply).
    tscmd::send() = [&win](const std::vector<std::vector<uint8_t>>& payloads) {
        if (payloads.empty()) return false;
        if (!tsLink.isOpen()) { win.showStatus("Not connected to a TunerStudio-protocol ECU", 3000); return false; }
        // These include "Reboot ECU" and "Reset to DFU". A burn scheduled moments ago has not reached the
        // flash yet, and rebooting on top of it throws the edit away — wait for the ECU to say it is done.
        tsBridge.awaitBurnComplete();
        for (const auto& p : payloads) {
            tsLink.sendCommand(p);
            AppStateSigilResolver::linkError = !tsLink.lastError().empty();
            if (AppStateSigilResolver::linkError) {
                win.showStatus("Command failed: " + tsLink.lastError(), 4000);
                return false;
            }
        }
        win.showStatus("Command sent", 1500);
        return true;
    };

    // BURN — the dialog footer's button, for both kinds of ECU, because both draw the same line: an edit
    // reaches RAM at once and flash only on a deliberate commit. Native ECUs have had EcuLink::burn() all
    // along with nothing in the UI to call it, so until now their tunes could not be committed at all.
    static bool s_burnPending = false;   // an edit has gone out that flash does not have yet
    Cache::instance().writeRequested.connect([](int, const std::vector<uint8_t>&) { s_burnPending = true; });
    // WHAT COUNTS AS PENDING. The ECU answers this itself: config_dirty is packed each telemetry frame
    // as (g_config_generation != g_config_generation_saved), so it counts every write the tune has taken
    // — including the ones the studio never made. A findlimits or autotune sweep writes calibration on
    // the ECU, and s_burnPending stayed false through all of it: the Burn button sat greyed out over a
    // freshly measured cal that only existed in RAM. The studio's own flag remains the fallback for an
    // ECU (or an older definition) that publishes no such channel.
    tuneburn::pending() = [] {
        if (tsLink.isOpen()) return tsBridge.burnPending();
        if (!link.isOpen())  return false;
        const Cache& c = Cache::instance();
        if (c.has("config_dirty")) return c.value("config_dirty") != 0.0;
        return s_burnPending;
    };
    tuneburn::burn() = [&win] {
        if (tsLink.isOpen()) {
            const bool ok = tsBridge.burnNow();
            win.showStatus(ok ? "Burned to flash" : "Burn FAILED \xC2\xB7 " + tsLink.lastError(), ok ? 2500 : 6000);
            if (ok) s_burnPending = false;
            return ok;
        }
        if (!link.isOpen()) { win.showStatus("Not connected \xC2\xB7 nothing to burn", 3000); return false; }
        link.burn();                       // native: persist g_config to flash
        s_burnPending = false;
        win.showStatus("Burned to flash", 2500);
        return true;
    };


    JMenu viewMenu("View");
    // Dock-visibility toggles (checkable). Each starts
    // checked (docks are visible); the onTriggered handlers are bound below once the docks + areas exist.
    // Grouping: [Tree·Properties·Dictionary·Controls] |
    // [Diagnostics·DTC·Lua Script] | [New Surface Tab]. (Grid/snap toggles live in Preferences▸Editor.)
    auto* tgTree    = viewMenu.add(g, "Navigation");                  tgTree->setCheckable(true);    tgTree->setChecked(true);
    auto* tgProps   = viewMenu.add(g, "Properties");                  tgProps->setCheckable(true);   tgProps->setChecked(true);
    auto* tgDict    = viewMenu.add(g, "Dictionary");                  tgDict->setCheckable(true);    tgDict->setChecked(true);
    auto* tgWidgets = viewMenu.add(g, "Controls");                    tgWidgets->setCheckable(true); tgWidgets->setChecked(true);
    viewMenu.addSeparator(g);
    auto* tgStatus  = viewMenu.add(g, "Status Lamps");                tgStatus->setCheckable(true);  tgStatus->setChecked(true);
    auto* tgDiag    = viewMenu.add(g, "Diagnostics");                 tgDiag->setCheckable(true);    tgDiag->setChecked(true);
    auto* tgDtc     = viewMenu.add(g, "Diagnostic Trouble Codes");    tgDtc->setCheckable(true);     tgDtc->setChecked(true);
    auto* tgTrig    = viewMenu.add(g, "Trigger Library");             tgTrig->setCheckable(true);    tgTrig->setChecked(true);
    viewMenu.addSeparator(g);
    // EDIT MODE SHOWS THE PAGE AS IT RUNS, and this is how you get at the rest of it. A page states its
    // alternatives at the same coordinates — only ever one of them on screen — so drawing every one while
    // authoring made a pile of overprinted captions. They are hidden while editing now, like they are
    // while tuning; this brings them back, outlined, for the times one of them is the thing being fixed.
    auto* tgHidden = viewMenu.add(g, "Show Hidden Widgets");
    tgHidden->setCheckable(true);
    tgHidden->setChecked(settings.get<bool>("editor.showHidden", false));
    Surface::setShowHiddenInEdit(tgHidden->isChecked());
    tgHidden->onTriggered.connect([tgHidden]{
        Surface::setShowHiddenInEdit(tgHidden->isChecked());
        jf::JSettings::instance().set("editor.showHidden", tgHidden->isChecked());
        if (g_invalidatePageSurface) g_invalidatePageSurface();   // set below, where the surfaces exist
    });
    viewMenu.addSeparator(g);
    viewMenu.add(g, "New Surface Tab")->onTriggered.connect([&onNewSurface]{ onNewSurface(); });
    viewMenu.add(g, "Delete Surface\xE2\x80\xA6")->onTriggered.connect([&onDeleteSurface]{ onDeleteSurface(); });
    viewMenu.add(g, "Reopen Surface…")->onTriggered.connect([&onReopenSurface]{ onReopenSurface(); });   // closed-but-pooled surfaces

    // TOOLS — instruments, not views. View shows and hides parts of THIS window; these four open a
    // thing that measures something, and reading a trigger log has nothing to do with whether the
    // Properties dock is visible. They were in View because there was nowhere else to put them.
    // The ECU reset action: Tools▸Reset ECU and the toolbar button below.
    static std::function<void()> s_resetEcu;
    JMenu toolsMenu("Tools");
    toolsMenu.add(g, "Trigger Designer")->onTriggered.connect([&onOpenTriggerDesigner]{ onOpenTriggerDesigner(); });
    toolsMenu.add(g, "Engine Cycle")->onTriggered.connect([&onOpenEngineCycle]{ onOpenEngineCycle(); });
    toolsMenu.add(g, "Trigger Log")->onTriggered.connect([&onOpenTriggerLog]{ onOpenTriggerLog(); });
    toolsMenu.add(g, "Knock Scope")->onTriggered.connect([&onOpenKnockScope]{ onOpenKnockScope(); });
    toolsMenu.add(g, "Auto Tune")->onTriggered.connect([&onOpenAutotune]{ onOpenAutotune(); });
    g_openTool = [&](const std::string& name) {
        if      (name == "Trigger Designer") onOpenTriggerDesigner();
        else if (name == "Engine Cycle")     onOpenEngineCycle();
        else if (name == "Trigger Log")      onOpenTriggerLog();
        else if (name == "Knock Scope")      onOpenKnockScope();
        else if (name == "Auto Tune")        onOpenAutotune();
        else JLOGC("ui.script", jf::JLogLevel::Warn) << "--tool " << name << ": no such tool";
    };
    toolsMenu.addSeparator(g);
    // Pick up nodes the definition has gained since this tree was written. A saved tree stops tracking
    // the meta by design (it is yours), so this is how a new sensor/module reaches an existing project.
    toolsMenu.add(g, "Reset ECU")->onTriggered.connect([]{ if (s_resetEcu) s_resetEcu(); });
    // A FIRMWARE KIT BY HAND: a test build sent to one person, without publishing anything. Pick the
    // kit's kit.json; the folder is checked as a kit (every file it names present), copied in beside the
    // downloaded kits, and from then on it is offered like any other — newest wins.
    toolsMenu.add(g, "Install Firmware Kit\xE2\x80\xA6")->onTriggered.connect([&win]{
        jf::JDialog::openFile("Choose the kit's kit.json", { "json" }, [&win](std::string path) {
            namespace fs = std::filesystem;
            const fs::path dir = fs::path(path).parent_path();
            fwkits::Kit kit;
            bool found = false;
            for (const fwkits::Kit& k : fwkits::scan(dir.parent_path().string(), false))
                if (std::error_code eq; fs::equivalent(fs::path(k.dir), dir, eq)) { kit = k; found = true; break; }
            if (!found || fs::path(path).filename() != "kit.json") {
                jf::JDialog::message("Not a firmware kit",
                    "That is not a complete firmware kit: choose the kit.json inside a kit folder, beside the "
                    "firmware, descriptor and dashboard it names.");
                return;
            }
            std::error_code ec;
            const fs::path dest = fs::path(StudioPaths::dataDir("firmware")) / (kit.board + "-" + kit.version);
            if (!fs::equivalent(dest, dir, ec)) {
                fs::remove_all(dest, ec);
                fs::create_directories(dest.parent_path(), ec);
                fs::copy(dir, dest, fs::copy_options::recursive, ec);
            }
            if (ec) { win.showStatus("Could not install the kit: " + ec.message(), 8000); return; }
            installKitMetas();
            win.showStatus("Installed firmware " + kit.board + " " + kit.version +
                           " \xE2\x80\x94 it is offered the next time a " + kit.board + " ECU connects", 8000);
        });
    });
    // ALWAYS AVAILABLE, like the logging editor and for the same reason: the frames are tune data,
    // and a bus map is worked out at a desk long before the car is in front of you. Nothing here
    // asks the ECU for anything — the edits land in the config image like every other edit, and the
    // burn stays deliberate.

    // RESET THROUGH THE FIRMWARE'S OWN `reset` COMMAND, which WAITS for a burn in flight before
    // rebooting (CliCommands.cpp:172, up to 4 s). The studio had no reset at all, so the only way was
    // the board's button or the power switch, and neither has that interlock: reset within ~2 s of a
    // burn and the save task never finishes, the ECU comes back on the PREVIOUS tune, and nothing
    // anywhere says so. That cost an afternoon — one changed trigger input that would not stick across
    // three burn/reset cycles.
    //
    // The reply never arrives (cmd_reset does not return), so the timeout that follows is expected and
    // is not an error worth showing. Defined here, where win and link are in scope; the toolbar button
    // that calls it is built later.
    s_resetEcu = [&win] {
        if (!link.isOpen()) { win.showStatus("Not connected \xC2\xB7 nothing to reset", 3000); return; }
        // An unburned tune is about to be discarded — that is what a reset IS (edits live in RAM until
        // a burn), but say so rather than let it happen silently.
        const bool dirty = tuneburn::pending() && tuneburn::pending()();
        link.sendCli("reset");
        win.showStatus(dirty ? "Resetting \xC2\xB7 UNBURNED changes will be lost"
                             : "Resetting \xC2\xB7 waiting for any burn to land first", 4000);
    };

    // Help menu (Lua API Reference + About). About uses the framework's
    // message dialog (a real WM-managed window). The Lua reference isn't bundled in this build yet, so it
    // says so rather than dead-ending. (Menu bar was File/Edit/View only — Help was the missing menu.)
    // LOGGING — recording is a thing you DO, so it lives where the doing lives. It was on a
    // preferences page, which is where a SETTING goes: a page cannot say "record now", it made the
    // channel set look like an application preference when it is written to the TUNE, and it put the
    // one control a tuner reaches for mid-session three clicks inside a settings dialog.
    //
    // REBUILT rather than mutated. JMenuItem has no setLabel and JMenu has no about-to-show, so
    // "Start"/"Stop" is produced by building the menu again when the recorder changes state.
    // clear() invalidates the items, so nothing here holds a pointer to one.
    static JMenu logMenu("Logging");
    static std::function<void()> refreshLogUi = []{};   // assigned once the toolbar button exists
    static auto rebuildLogMenu = [&g, &win] {
        logMenu.clear();

        const bool on = DatalogRecorder::instance().recording();
        logMenu.add(g, on ? "Stop Recording" : "Start Recording")->onTriggered.connect([&win]{
            DatalogRecorder& rec = DatalogRecorder::instance();
            if (rec.recording()) win.showStatus("Recording saved \xE2\x80\x94 " + rec.stop(), 6000);
            else {
                std::string why;
                if (rec.start(&why)) win.showStatus("Recording \xE2\x80\x94 " + rec.path(), 4000);
                else                 win.showStatus("Cannot record: " + why, 6000);
            }
            refreshLogUi();
        });
        // THE STUDIO'S OWN CHANNELS -- what a recording on this PC carries. Its own list, not the card's
        // (DatalogRecorder::recordingSelection): choosing what the ECU logs never narrows what the laptop
        // keeps. Left empty, a recording carries every channel. A recording already running keeps the
        // columns it started with -- a log whose columns change halfway has lied about the rows above.
        logMenu.add(g, "Recording Channels\xE2\x80\xA6")->onTriggered.connect([&win]{
            enumpick::channels()("Recording Channels \xE2\x80\x94 leave empty to record every channel",
                                 DatalogRecorder::recordingSelection(),
                                 [&win](std::vector<std::string> picked) {
                const size_t n = picked.size();
                DatalogRecorder::setRecordingSelection(picked);
                std::string msg = n ? "Recording " + std::to_string(n) + " channel(s)" : std::string("Recording every channel");
                if (DatalogRecorder::instance().recording()) msg += " \xE2\x80\x94 from the next recording";
                win.showStatus(msg, 4000);
            });
        });
        logMenu.addSeparator(g);

        // ONE ITEM, not a submenu of templates beside a dialog that does the same thing. The channel
        // set, the profiles that name one and the condition that starts a log are the same setup —
        // "log these channels, while this is true" — and it is the ECU'S logger, which is what the
        // name now says. A menu that offered half of it and a dialog that offered all of it left the
        // tuner deciding which copy was the real one.
        // ALWAYS AVAILABLE. The dialog is a profile editor first and a way to write to the car
        // second, and a tuner works out a logging setup at a desk with no ECU in the room. The card
        // gates ACTIVATE, inside — which is the only part of it that needs one.
        logMenu.add(g, "Onboard Logging\xE2\x80\xA6")->onTriggered.connect([&win]{
            win.openModal<OnboardLoggingDialog>();
        });
        // THE CARD, WHEN THE PC HAS IT. Nothing here asks the ECU for anything: at key-off the card
        // becomes a USB drive on its own, and from the studio's side collecting logs is a directory
        // listing and a copy. The item stays enabled with no card so it can SAY there is no card —
        // an item that vanishes when it is not ready is an item nobody learns exists.
        // ALWAYS AVAILABLE, for the same reason as onboard logging: the dialog's own empty state
        // says there is no card, and its Find card… is the escape hatch for a machine that does not
        // automount — which is unreachable if the item that opens it is greyed out.
        logMenu.add(g, "Logs on Card\xE2\x80\xA6")->onTriggered.connect([&win]{
            win.openModal<CardLogsDialog>();
        });
        logMenu.addSeparator(g);
        logMenu.add(g, "Open Logs Folder")->onTriggered.connect([&win]{
            const std::string dir = DatalogRecorder::resolvedDirectory();
            if (!jf::JDesktop::openUrl(dir)) win.showStatus("Logs are in " + dir, 6000);
        });
    };
    rebuildLogMenu();

    JMenu helpMenu("Help");
    // THE MANUAL FIRST, on F1: it is the answer to "how do I…", and the Lua reference is one part of it.
    addChordItem(helpMenu, "User Manual", "F1", [&win]{
        std::string why;
        if (helppages::openUserManual(why)) win.showStatus("User manual opened in your browser", 3000);
        else                                win.showStatus(why, 8000);
    });
    helpMenu.add(g, "Lua API Reference")->onTriggered.connect([&win]{
        // HTML, in the browser (HelpPages): built live from the meta's Lua API catalog, the same source
        // the firmware exports, so it always describes the connected schema.
        std::string why;
        if (helppages::openLuaReference(meta, why)) win.showStatus("Lua API reference opened in your browser", 3000);
        else                                        win.showStatus(why, 8000);
    });
    helpMenu.addSeparator(g);
    // Where people look for it. The same check as Preferences ▸ Check now — a newer studio and newer ECU
    // firmware — so the two can never answer differently.
    helpMenu.add(g, "Check for Updates…")->onTriggered.connect([]{
        if (PreferencesDialog::onCheckForUpdates) PreferencesDialog::onCheckForUpdates();
    });
    // NAMED FOR WHAT IT DOES. It was "Tools ▸ Update Navigation from Definition…", which reads like an
    // update — it is not one. It offers the entries the definition's navigation tree has and yours does
    // not, and adds the ones you pick; nothing is changed or removed. It edits the layout, so it is shown
    // in Editing mode only (gated below).
    helpMenu.add(g, "Add Missing Navigation Entries\xE2\x80\xA6")->onTriggered.connect([&]{ if (reseedFn) reseedFn(); });
    helpMenu.addSeparator(g);
    helpMenu.add(g, "About jayecu Studio…")->onTriggered.connect([]{
        // THE FRAMEWORK'S OWN MESSAGE DIALOG, with a picture — the case JDialogRequest::imageRgba exists
        // for, and what jscope's About box uses. It draws the image at its native size, one texel per
        // pixel, which is what keeps the mark sharp; a hand-rolled dialog scaling a big PNG down to fit
        // hands the same job to the rasteriser, which has one sample per output pixel to do it with.
        jf::JDialogRequest req;
        req.kind  = jf::JDialogRequest::JKind::Message;
        req.title = "About jayecu Studio";
        aboutlogo::attach(req);
        req.body  =
            "jayECU Studio\n"
            "Cross-ECU tuning studio for jayecu and rusEFI-based hardware.\n"
            "\n"
            "Version " STUDIO_VERSION "\n"
            "\n"
            "Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>\n"
            "\n"
            "Built on JFramework.";
        jf::JDialogManager::instance().push(std::move(req));
    });

    win.menuBar().addMenu(&fileMenu);
    win.menuBar().addMenu(&editMenu);
    win.menuBar().addMenu(&viewMenu);
    win.menuBar().addMenu(&libraryMenu);
    win.menuBar().addMenu(&toolsMenu);
    win.menuBar().addMenu(&logMenu);
    win.menuBar().addMenu(&helpMenu);

    // --- Toolbar + status bar --------------------------------------------------
    auto& tb = win.toolBar();
    win.setToolbarHeight(47.f * jf::JStyle::uiScale());   // toolbar height (hosts the 32px connect icon)
    // Locked/Editing toggle — the single edit-mode switch:
    // unchecked = "Locked" (run mode: surfaces read-only, authoring docks hidden); checked = "Editing"
    // (surfaces editable, Properties/Dictionary/Controls docks shown). The label reflects the current state.
    static jf::JToggleButton lockBtn(g, "Locked");   // sizes itself from its label, at whatever the interface scale is
    lockBtn.onToggled.connect([&win, &onSetEditMode](bool editing){
        lockBtn.setLabel(editing ? "Editing" : "Locked");
        Cache::instance().setReadOnly(editing);    // Locked/run OPERATES the live instrument (controls + value
                                                   // edits write to the ECU); Editing DESIGNS the layout, tune locked
        onSetEditMode(editing);                     // switch surfaces + authoring docks to match
        win.showStatus(editing ? "Editing mode \xE2\x80\x94 surfaces editable"
                               : "Locked \xE2\x80\x94 read-only", 2000);
    });
    // Toolbar layout: Locked | separator | Connect (icon-only) | Verify ↔ ECU.
    tb.addWidget(&lockBtn);          // 0 = ask the widget what it needs
    tb.addSeparator();
    // The connect indicator: click toggles the link. Its colour/animation tracks the state,
    // driven by the link signals wired below. 44px slot holds the 32px chip icon centred.
    tb.addWidget(&connectBtn, 44.f);
    tb.addSeparator();
    // BURN + the dirty indicator. An edit reaches ECU RAM at once and flash only when asked, so "there
    // are changes flash does not have" is a state that must be visible without opening a dialog — the
    // per-dialog footer button says it only for the page you happen to be on. Wired below, once the
    // link and the burn hooks exist.
    // Burn-in-flight bookkeeping for the indicator; see the onClicked/poll pair below.
    static bool    s_awaitingBurn   = false;
    static int64_t s_awaitBurnUntil = 0;
    static const auto s_msNow = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    static BurnButton burnBtn(g);
    tb.addWidget(&burnBtn, 90.f);
    burnBtn.onClicked.connect([]{
        // Flush the debounced write queue FIRST: burning before the last edit has left the studio
        // commits the tune as it was one edit ago, which is the one bug this button must not have.
        Cache::instance().flushWrites();
        // NO GREEN YET. The ECU acks 'b' on receipt and hands the write to its save task, which takes
        // the better part of two seconds; showing "burned" here said so while the tune was still only
        // in RAM. The poll below turns it green when the ECU's OWN config_dirty clears, which is the
        // only evidence that anything reached flash.
        if (tuneburn::burn() && tuneburn::burn()()) {
            s_awaitingBurn   = true;
            s_awaitBurnUntil = s_msNow() + 8000;   // give up waiting, never hang the indicator
            burnBtn.setBurning(true);
        }
    });
    // POLLED, not signalled: the state belongs to the ECU (see tuneburn::pending), so the only honest
    // way to hold it is to keep asking. Four times a second is well under the telemetry rate that feeds
    // it and setDirty() early-outs when nothing changed, so a clean tune costs nothing but the question.
    static jf::JTimer burnPoll;
    burnPoll.onTick.connect([]{
        const bool dirtyNow = tuneburn::pending() && tuneburn::pending()();
        // THE BURN LANDED WHEN THE ECU SAYS IT DID. config_dirty is (g_config_generation !=
        // g_config_generation_saved) packed every telemetry frame, so its falling edge IS the save task
        // finishing. Waiting for that edge is what makes the green mean "on the flash" rather than
        // "the ECU heard me".
        if (s_awaitingBurn) {
            if (!dirtyNow) {
                s_awaitingBurn = false;
                burnBtn.setBurning(false);
                burnBtn.showBurned();
            } else if (s_msNow() > s_awaitBurnUntil) {
                // Never landed. Say nothing green: leave it amber, which is the truth.
                s_awaitingBurn = false;
                burnBtn.setBurning(false);
            }
        }
        burnBtn.setDirty(dirtyNow);
        // …AND KEEP THE RECORD CONTROLS HONEST. The menu item and the toolbar button both say
        // Start or Stop, and a recording can end without either of them being pressed — autologging
        // stops on disconnect. Polled rather than wired to a signal because that is one comparison
        // against one bool, and it early-outs when nothing changed.
        static bool lastRecording = false;
        const bool  r = DatalogRecorder::instance().recording();
        if (r != lastRecording) { lastRecording = r; refreshLogUi(); }
    });
    burnPoll.start(std::chrono::milliseconds(250), jf::JTimer::JMode::Repeating);

    // RESET, BESIDE BURN, because they are the pair: a burn takes ~2 s to reach flash and a reset
    // inside that window silently discards it. This one goes through the firmware's `reset` command,
    // which waits for the save to land first — the board's button and the power switch cannot.
    static jf::JButton resetBtn(g, "Reset ECU", 0.f, 22.f);   // a widget, so the gate can grey or hide it
    resetBtn.onClicked.connect([]{ if (s_resetEcu) s_resetEcu(); });
    tb.addWidget(&resetBtn);

    // RECORD. A log is one start to one stop, and this button is one of the two things that can press
    // either — the other is the connection itself, when autologging is on (below). They act on the same
    // recorder, so stopping a session the app started ends THAT file and pressing record again begins a
    // new one; a connection can therefore produce several logs when somebody marked it up that way.
    tb.addSeparator();
    static jf::JButton recBtn(g, "Record", 82.f, 22.f);
    // ONE refresh for both surfaces. The toolbar button and the Logging menu say the same thing about
    // the same recorder, so they are updated together — two places that can disagree about whether a
    // recording is running is two places to look and one of them wrong.
    static auto refreshRecBtn = [] {
        recBtn.setLabel(DatalogRecorder::instance().recording() ? "Stop" : "Record");
        rebuildLogMenu();
    };
    refreshLogUi = []{ refreshRecBtn(); };
    tb.addWidget(&recBtn, 86.f);
    recBtn.onClicked.connect([&win]{
        DatalogRecorder& rec = DatalogRecorder::instance();
        if (rec.recording()) {
            const std::string done = rec.stop();
            win.showStatus("Recording saved \xE2\x80\x94 " + done, 6000);
        } else {
            std::string why;
            if (rec.start(&why)) win.showStatus("Recording \xE2\x80\x94 " + rec.path(), 4000);
            else                 win.showStatus("Cannot record: " + why, 6000);
        }
        refreshRecBtn();
    });
    // ONE ROW PER FRAME, at whatever rate the link delivers. Resampling would invent data points that
    // the ECU never reported, which is precisely what a log exists to rule out.
    Cache::instance().frameUpdated.connect([]{ DatalogRecorder::instance().onFrame(); });
    // Open a chosen port + reflect the connecting state. Shared by every connect path below.
    auto openPort = [&](const std::string& port) {
        s_openedPort = port;
        JLOGC("ui.connect", jf::JLogLevel::Info) << "opening port " << port << " \xE2\x86\x92 identity handshake";
        connectBtn.setConnState(ConnectButton::State::Connecting);
        win.showStatus("Opening " + port + " …", 3000);
        link.open(port);
    };
    // A TS link that stops answering has to SHOW as broken. The bridge gives up polling after five missed
    // frames, and until now that was the whole of it: the gauges froze at their last reading, the connect
    // chip stayed green, and a held value is indistinguishable from a steady one.
    tsBridge.configReloadedFromEcu.connect([&win](int bytes) {
        JLOGC("comms", jf::JLogLevel::Info) << "ECU changed its own tune (CRC moved) — re-read " << bytes << " B";
        win.showStatus("The ECU changed its configuration \xE2\x80\x94 re-read " + std::to_string(bytes) + " B", 5000);
    });
    tsBridge.linkLost.connect([&]{
        AppStateSigilResolver::linkError = true;
        tsLink.close();
        connectBtn.setConnState(ConnectButton::State::Error);
        s_connTitle.clear();
        win.showStatus("TS ECU stopped answering \xE2\x80\x94 link dropped", 6000);
        win.setStatusText("Link lost");
    });
    // The imported-definition library, searched by SIGNATURE: every .meta an .ini import wrote carries the
    // signature of the firmware it describes, which is exactly what the ECU answers an 'S' query with.
    auto metaForSignature = [](const std::string& sig) -> std::string {
        namespace fs = std::filesystem;
        const fs::path lib = StudioPaths::dataDir("meta");
        std::error_code ec;
        for (const auto& f : fs::directory_iterator(lib, ec)) {
            if (f.path().extension() != ".meta") continue;
            const auto j = jf::JJson::tryParseFile(f.path().string());
            if (!j || !j->isObject()) continue;
            const jf::JJson& doc = *j;                    // const: the mutating operator[] would rewrite it
            if (doc["tsProtocol"]["signature"].str() == sig) return f.path().string();
        }
        return {};
    };
    connectFn = [&, openPort, metaForSignature]{
        const auto s = connectBtn.connState();
        // AN ECU WAITING IN ITS BOOTLOADER has no serial port to connect to — it is there because an
        // update did not finish, or because someone held BOOT and pressed RESET to recover it. Offer to
        // install firmware, rather than report that no ECU was found.
        if ((s == ConnectButton::State::Idle || s == ConnectButton::State::Error) && !s_upgrade.active() &&
            dfu::devicePresent() && s_offerRecovery) { s_offerRecovery(); return; }
        if (!(s == ConnectButton::State::Idle || s == ConnectButton::State::Error)) {
            Cache::instance().flushWrites(); saveActiveTune();                  // persist the calibration before dropping the link
            // The link going away ends the recording whatever started it: rows after this point would
            // be a stalled cache repeated at frame rate, which reads as a running engine holding still.
            if (DatalogRecorder::instance().recording()) {
                const std::string done = DatalogRecorder::instance().stop();
                refreshRecBtn();
                win.showStatus("Recording saved \xE2\x80\x94 " + done, 6000);
            }
            if (tsLink.isOpen()) {                                              // TS session teardown
                tsBridge.awaitBurnComplete();                                   // don't drop the link mid-burn
                tsBridge.stopTelemetry(); tsLink.close();
                connectBtn.setConnState(ConnectButton::State::Idle);
                win.setStatusText("Disconnected"); s_connTitle.clear();
                return;
            }
            link.close(); return;
        }
    // The TunerStudio path — the COMPATIBILITY one, and never the first thing tried. This studio is
    // for jayecu ECUs: connect looks for one of those first and only speaks TS when that search comes
    // up empty. Returns true when it connected.
    //
    // It is also reachable through a hook, because the native attempt fails ASYNCHRONOUSLY — the
    // identity request is answered (or not) well after connectFn has returned — so the fallback has to
    // be driven from the link's failure signal rather than from a line below the call.
    auto tryTsConnect = [&, metaForSignature]() -> bool {
        // CONST reference, deliberately. JJson's non-const operator[] INSERTS a null for a missing key
        // and its non-const arr() then throws on it — so reading `proto["pages"]` out of a
        // definition imported before page geometry existed took the whole studio down with
        // bad_variant_access. The const overloads return empty defaults instead.
        const jf::JJson& proto = s_tsProto;
        std::vector<std::string> cands;
        if (const std::string pick = jf::JSettings::instance().get<std::string>("connection.port", std::string());
            !pick.empty()) {
            cands.push_back(pick);              // the chosen port answers for this path too
        } else {
            cands = DeviceScanner::usbSerialPorts();
            if (cands.empty() && *DeviceScanner::fallbackPort()) cands.emplace_back(DeviceScanner::fallbackPort());
        }
        connectBtn.setConnState(ConnectButton::State::Connecting);
        // ANY signature will do — we are asking the ECU what it is, not checking it against an assumption.
        std::string sig;
        for (const auto& pt : cands) {
            if (!tsLink.open(pt)) continue;
            sig = tsLink.querySignature();
            if (!sig.empty()) break;
            tsLink.close(); sig.clear();
        }
        if (!tsLink.isOpen() || sig.empty()) {
            connectBtn.setConnState(ConnectButton::State::Error);
            win.showStatus("No ECU answered a TunerStudio signature query", 5000);
            return false;
        }
        // THE SIGNATURE PICKS THE DEFINITION. Whatever is loaded is irrelevant — it describes some other
        // firmware until proven otherwise, and reading a rusEFI ECU through the wrong map is worse than
        // not connecting at all. Load the definition whose signature IS the one that answered.
        if (proto["signature"].str() != sig) {
            const std::string found = metaForSignature(sig);
            if (found.empty()) {
                // Nothing describes this ECU yet. It named itself, so say so and offer the way forward
                // rather than a dead end — the definition for a rusEFI board is its .ini.
                tsLink.close();
                connectBtn.setConnState(ConnectButton::State::Idle);
                win.setStatusText("No definition for " + sig);
                // Two ways forward, and the ECU named itself so both are actionable: rusEFI publishes a
                // definition per firmware at an address derived from that very signature, and failing
                // that the user has the file. No third option is offered because there is no third place
                // a definition legitimately comes from.
                const std::string url = rusefiIniUrl(sig);
                std::vector<std::string> choices;
                if (!url.empty()) choices.push_back("Download the definition from rusefi.com");
                choices.push_back("Choose a local .ini file\xE2\x80\xA6");
                win.openModal<ListPickerDialog>(
                    std::string("No definition for \xE2\x80\x9C") + sig + "\xE2\x80\x9D", choices, false,
                    [url](int idx, std::string) {
                        const bool download = !url.empty() && idx == 0;
                        if (download) { if (g_downloadIniThenConnect) g_downloadIniThenConnect(url); }
                        else if (idx >= 0) { if (g_importIniThenConnect) g_importIniThenConnect(); }
                    });
                return false;
            }
            if (g_loadSchemaPath) g_loadSchemaPath(found);
            if (!s_tsProto.isObject()) {
                tsLink.close();
                connectBtn.setConnState(ConnectButton::State::Error);
                win.showStatus("Could not load the definition for \"" + sig + "\"", 6000);
                return false;
            }
        }
        // THE PROJECT for this definition — found, and its saved tune decoded ON THE SIDE, but NOTHING
        // adopted yet. That tune is what the ECU gets compared against, and until the comparison is
        // answered we do not know whose bytes win. A connection the user then cancels must leave the
        // studio holding exactly what it held before Connect was pressed, so the switch-the-project step
        // waits behind the answer rather than running ahead of it.
        Ecu* const target = Ecu::openOrCreate("tsimport:" + meta.layoutHash(), meta.board());
        if (target) target->setLabel(meta.tsSignature());   // "ts" names the protocol; the signature names the ECU
        const std::string targetTune =
            activeTuneFor(target);
        // The project's tune as a config image — the SAME loader the native connect and Open ECU use,
        // so both protocols ask the reconcile the same question about the same bytes.
        ensureLocalTuneLoaded(target, meta);
        const std::vector<uint8_t> localImg =
            (target && s_imageUid == target->uid()) ? Cache::instance().configImage()
                                                    : std::vector<uint8_t>{};
        // PAGE GEOMETRY. The flat config image the widgets address is several ECU pages laid end to
        // end; ask for it as one page-0 read and the ECU answers OUT_OF_RANGE the moment the request
        // passes the end of page 0. Hand the link the ini's map and let it read (and later write and
        // burn) page by page.
        std::vector<TsLink::Page> pages;
        for (const auto& p : proto["pages"].arr()) {
            TsLink::Page pg;
            pg.id   = static_cast<int>(p["id"].number(0));
            pg.base = static_cast<int>(p["base"].number(0));
            pg.size = static_cast<int>(p["size"].number(0));
            pg.burn = p["burn"].boolean(true);
            if (pg.size > 0) pages.push_back(pg);
        }
        tsLink.setPages(pages);
        tsLink.setBurnSettleMs(static_cast<int>(proto["pageActivationDelay"].number(0)));
        int flatSize = 0;
        for (const auto& pg : pages) flatSize = std::max(flatSize, pg.base + pg.size);
        if (flatSize == 0) flatSize = static_cast<int>(proto["configSize"].number(proto["pageSize"].number(meta.configSize())));
        const int chunk = std::max(16, static_cast<int>(proto["blockingFactor"].number(256)));
        win.showStatus("TS: reading " + std::to_string(flatSize) + " B config over "
                       + std::to_string(std::max<size_t>(1, pages.size())) + " page(s) \xE2\x80\xA6", 3000);
        tsLink.setBlockSize(chunk);
        const std::vector<uint8_t> img = tsLink.readImage(chunk, flatSize);
        if (static_cast<int>(img.size()) != flatSize) {
            connectBtn.setConnState(ConnectButton::State::Error);
            AppStateSigilResolver::linkError = true;
            win.showStatus("TS config read failed: " + tsLink.lastError(), 5000);
            tsLink.close();
            return true;
        }
        // ADOPT AND GO LIVE: switch the studio to this ECU's project, take `image` as the tune, and start
        // the session. Everything that makes the connection real lives here so that it can be held back
        // behind the reconcile — and simply never run when the answer is "neither".
        auto goLive = [&win, &connectBtn, target, targetTune, chunk, sig, openTune, saveActiveTune]
                      (const std::vector<uint8_t>& image, bool saveAfter) {
            if (target && g_adoptConnectedEcu) g_adoptConnectedEcu(target, nullptr);   // THE shared adopt
            if (target && !targetTune.empty()) openTune(target, targetTune);           // project's own tune…
            Cache::instance().setConfigImage(image);                                   // …then the winner
            Cache::instance().setBaseline(image);
            if (target) s_imageUid = target->uid();
            if (saveAfter) saveActiveTune();
            // WHOSE BYTES WON. "I chose the ECU's tune and the studio's showed up" is unanswerable after
            // the fact without this: a fingerprint of what the cache actually ended up holding, next to
            // the two candidates it was chosen between.
            {
                auto fp = [](const std::vector<uint8_t>& v) {
                    return v.empty() ? 0u : crc32_ieee::compute(reinterpret_cast<const char*>(v.data()), int(v.size()));
                };
                JLOGC("ui.connect", jf::JLogLevel::Info)
                    << "adopted image crc=" << std::hex << fp(Cache::instance().configImage())
                    << " (ecu=" << fp(image) << ")" << std::dec
                    << " tune='" << activeTuneName << "' ecu=" << (target ? target->uid() : std::string("?"));
            }
            tsBridge.attach();
            // Telemetry block: the ini's ochBlockSize, else the span of the converted telemetry fields.
            const jf::JJson& p = s_tsProto;                       // const: see the connect path
            int och = static_cast<int>(p["ochBlockSize"].number(0));
            if (och <= 0) { for (const auto& [n, tf] : meta.telemetry()) och = std::max(och, tf.offset + tf.size); }
            if (och > 0) tsBridge.startTelemetry(100, och);
            // An ECU that edits its own configuration — a pedal calibration grabbing ADC values, an ETB
            // autotune storing what it learned, a Lua script setting something — publishes nothing to say
            // so, except its tune CRC, which moves. Watch that and re-read: otherwise the studio shows
            // stale bytes and the next burn writes them back over what the ECU worked out. rusEFI calls the
            // channel tuneCrc16; a definition that has no such channel is simply not watched.
            tsBridge.watchTuneCrc("tuneCrc16", chunk);
            connectBtn.setConnState(ConnectButton::State::Connected);
            AppStateSigilResolver::linkError = false;   // a fresh link clears the error lamp a lost one lit
            s_connTitle = sig + " (TS)";
            win.setStatusText("Connected (TS): " + sig);
        };
        // …and its opposite: drop the link and go back to where we started. No session, no project switch,
        // the studio's tune as it was and the ECU's as it was.
        auto abandon = [&win, &connectBtn](const std::string& why) {
            tsLink.close();
            connectBtn.setConnState(ConnectButton::State::Idle);
            s_connTitle.clear();
            win.setStatusText(why);
        };

        // RECONCILE — the shared routine, so this path asks exactly what the native one asks.
        reconcileTune(win, targetTune, localImg, img, &meta, target,
            [goLive, abandon, img, localImg, targetTune, &win](JTuneSync d) {
                switch (d) {
                    case JTuneSync::NothingLocal:                 // first connect to this ECU
                        goLive(img, /*saveAfter=*/false);
                        break;
                    case JTuneSync::InSync:
                        goLive(img, /*saveAfter=*/false);
                        win.showStatus("\xE2\x97\x8F In sync \xC2\xB7 " + targetTune, 4000);
                        break;
                    case JTuneSync::Pull:
                        goLive(img, /*saveAfter=*/true);
                        win.showStatus("Pulled the ECU image into \"" + targetTune + "\"", 4000);
                        break;
                    case JTuneSync::Push: {
                        const bool wrote  = tsLink.writeFlat(0, localImg);   // whole image, split across the pages
                        const bool burned = wrote && tsLink.burnAll();
                        JLOGC("ui.connect", burned ? jf::JLogLevel::Info : jf::JLogLevel::Error)
                            << "TS push: write=" << wrote << " burn=" << burned << " " << tsLink.lastError();
                        // A half-written ECU is worse than either tune winning: say so and drop the link
                        // rather than going live over bytes of unknown provenance.
                        if (!burned) { abandon("Push FAILED \xC2\xB7 " + tsLink.lastError()); break; }
                        goLive(localImg, /*saveAfter=*/false);
                        win.showStatus("Pushed \"" + targetTune + "\" to the ECU", 4000);
                        break;
                    }
                    case JTuneSync::PushDefaults:   // asked only of a native ECU (it needs a layout hash)
                    case JTuneSync::Cancel:
                        abandon("Connection cancelled \xC2\xB7 nothing changed");
                        break;
                }
            });
        return true;
    };
    g_tryTsConnect = [tryTsConnect] { return tryTsConnect(); };

        // A NAMED PORT IS AN INSTRUCTION, not a hint: Preferences ▸ Connection ▸ Serial port. Automatic
        // is the default and is right whenever the studio can tell an ECU from anything else, but it
        // cannot always — a port with no usable descriptor looks like every other port, and the studio
        // then picks one. Saying which one ends that, and it is also how you choose between two ECUs
        // that are the same hardware.
        if (const std::string pick = jf::JSettings::instance().get<std::string>("connection.port", std::string());
            !pick.empty()) {
            openPort(pick);
            return;
        }

        // Enumerate USB-CDC candidates. ONE → open it directly (the common bench case; no scan, no UI freeze).
        // SEVERAL → poll each with an identity request and let the user pick.
        // Prefer ports whose USB descriptor is the jayecu CDC (ST VCP 0x0483:0x5740): a co-plugged foreign
        // serial adapter (a CH340 dongle, etc.) then can't tip us out of the direct-open path or get polled.
        std::vector<std::string> cands, jayecuCands;
        for (const auto& info : jf::JSerialPort::availablePorts()) {
            if (!DeviceScanner::isUsbSerial(info)) continue;
            cands.push_back(info.port);
            if (info.hasVidPid && info.vendorId == 0x0483 && info.productId == 0x5740)
                jayecuCands.push_back(info.port);
        }
        if (!jayecuCands.empty()) cands.swap(jayecuCands);   // narrow to jayecu ports when any advertise it
        if (cands.empty() && *DeviceScanner::fallbackPort())
            cands.emplace_back(DeviceScanner::fallbackPort());
        if (cands.empty()) {   // nothing enumerated and nothing to guess at — say so, do not open ""
            if (tsFallbackAllowed() && g_tryTsConnect && g_tryTsConnect()) return;
            connectBtn.setConnState(ConnectButton::State::Idle);
            win.showStatus("No serial ports found", 4000);
            return;
        }
        if (cands.size() == 1) { openPort(cands.front()); return; }
        win.showStatus("Scanning for ECUs \xE2\x80\xA6", 3000);
        auto devs = DeviceScanner::scan(cands, 1200);
        if (devs.empty()) {
            if (tsFallbackAllowed() && g_tryTsConnect && g_tryTsConnect()) return;   // no jayecu → try TS
            connectBtn.setConnState(ConnectButton::State::Idle);
            win.showStatus("No ECU found on the bus", 4000);
            return;
        }
        if (devs.size() == 1)  { openPort(devs.front().port); return; }
        // More than one ECU on the bus → let the user pick which to connect to (label = board · uid (port)).
        std::vector<std::string> labels, ports;
        for (const auto& d : devs) {
            labels.push_back(d.board + "  \xC2\xB7  " + (d.uid.size() > 6 ? d.uid.substr(d.uid.size() - 6) : d.uid) + "  (" + d.port + ")");
            ports.push_back(d.port);
        }
        win.openModal<ListPickerDialog>(std::string("Select an ECU"), labels, false,
            [ports, openPort](int idx, std::string) { if (idx >= 0 && idx < (int)ports.size()) openPort(ports[idx]); });
    };
    connectBtn.onClicked.connect([&]{ if (connectFn) connectFn(); });
    static jf::JButton verifyBtn(g, "Verify \xE2\x86\x94 ECU", 0.f, 22.f);   // a widget, so the gate can grey it
    tb.addWidget(&verifyBtn);
    verifyBtn.onClicked.connect([&]{   // read back + byte-diff against the cache + re-push divergent runs
        // The TS link answers synchronously (no configImageReady signal to wait on), so it does the whole
        // round here. Without this the button said "Not connected" to a perfectly connected TS ECU — the
        // one check that tells a tuner the ECU really holds what the project says it does.
        if (tsLink.isOpen()) {
            int flatSize = 0;
            for (const auto& pg : tsLink.pages()) flatSize = std::max(flatSize, pg.base + pg.size);
            if (flatSize == 0) flatSize = Cache::instance().configImage().size();
            const jf::JJson& proto = s_tsProto;                       // const: see the connect path
            const int chunk = std::max(16, static_cast<int>(proto["blockingFactor"].number(256)));
            tsLink.setBlockSize(chunk);
            const std::vector<uint8_t> img = tsLink.readImage(chunk, flatSize);
            if (static_cast<int>(img.size()) != flatSize) {
                AppStateSigilResolver::linkError = true;
                win.showStatus("Verify: read failed \xE2\x80\x94 " + tsLink.lastError(), 5000);
                return;
            }
            const std::vector<uint8_t>& local = Cache::instance().configImage();
            size_t repushed = 0;
            for (size_t i = 0; i < local.size() && i < img.size();) {
                if (local[i] == img[i]) { ++i; continue; }
                size_t j = i;
                while (j < local.size() && j < img.size() && local[j] != img[j]) ++j;
                tsLink.writeFlat(static_cast<int>(i), { local.begin() + i, local.begin() + j });
                repushed += j - i;
                i = j;
            }
            if (repushed) tsLink.burnAll();
            win.showStatus(repushed ? "Verify: re-pushed " + std::to_string(repushed) + " byte(s) to the ECU"
                                    : "Verify: ECU matches the project tune", 5000);
            return;
        }
        if (link.isOpen()) { s_verifyPending = true; link.readConfigImage(); win.showStatus("Verifying against the ECU \xE2\x80\xA6", 2500); }
        else win.showStatus("Not connected", 2000);
    });
    win.setStatusText("Ready");   // no definition yet is the intended startup state, not a failure

    // Right-side live readout — the status bar's built-in self-refreshing label (the SDK owns the
    // 4 Hz timer, the right-aligned layout, and the repaint dedupe; the app just supplies the text).
    // "• <hash> · rpm · clt · gen [· Lua …]", blank when the link is down. '•' (U+2022) and
    // '·' (U+00B7) are both in the font atlas.
    win.setLiveStatus([&]() -> std::string {
        auto& c = Cache::instance();
        // IS TELEMETRY ARRIVING? The studio's own answer, first, for every ECU. It used to be a channel a
        // definition happened to publish (the native firmware's "1kHz Frame Count", with a gauge bound to
        // it), so connecting to anything else left no way to tell a working ECU from a dead one. Frames
        // land in Cache::ingestTelemetry whatever the protocol, so this reads the same on both.
        std::string live;
        if (c.telemetryFrames()) {
            static uint64_t lastN = 0;
            static std::chrono::steady_clock::time_point lastT = std::chrono::steady_clock::now();
            static double hz = 0.0;
            const uint64_t n = c.telemetryFrames();
            const auto now = std::chrono::steady_clock::now();
            if (const double dt = std::chrono::duration<double>(now - lastT).count(); dt >= 0.20) {
                hz = double(n - lastN) / dt; lastN = n; lastT = now;
            }
            const long long stale = c.msSinceTelemetry();
            char b[96];
            if (stale > 1500) std::snprintf(b, sizeof b, "\xE2\x97\x8B no telemetry for %.1fs \xC2\xB7 %llu frames",
                                            double(stale) / 1000.0, static_cast<unsigned long long>(n));
            else              std::snprintf(b, sizeof b, "\xE2\x97\x8F %llu frames \xC2\xB7 %.0f Hz",
                                            static_cast<unsigned long long>(n), hz);
            live = b;
        }
        if (!link.isOpen()) return live;    // TunerStudio link, or offline: liveness is the whole story
        // LIVE state, not a startup constant: the studio starts with no definition by design, and one
        // arrives later (connect / open / New Tune). Reading a `const bool metaOk = false` here meant the
        // bar said "no meta" for the whole session, including with a definition loaded and the dictionary
        // full of its channels.
        if (!meta.isValid()) return live.empty() ? std::string("\xE2\x80\xA2 connected (no meta)")
                                                 : live + "  \xC2\xB7  no meta";
        // Lua error-count bump → immediate debug pull, so the error text reaches the console/errors tab
        // right away (edge-triggered; the 1 Hz poll is the fallback).
        static int lastLuaErrs = -1;
        const int luaErrs = static_cast<int>(c.value("lua_error_count"));
        if (lastLuaErrs != -1 && luaErrs != lastLuaErrs) link.requestDebugLog();
        lastLuaErrs = luaErrs;
        std::string lua;
        if (meta.hasTelemetry("lua_state"))                      // a TCU has no Lua: say nothing, not "Lua OK"
        switch (static_cast<int>(c.value("lua_state"))) {
            case 0: lua = "  \xC2\xB7  Lua OK"; break;
            case 1: lua = "  \xC2\xB7  Lua Disabled"; break;
            case 2: lua = "  \xC2\xB7  Lua Load Err (L" +
                          std::to_string(static_cast<int>(c.value("lua_error_line"))) + ")"; break;
            case 3: lua = "  \xC2\xB7  Lua Run Err (L" +
                          std::to_string(static_cast<int>(c.value("lua_error_line"))) + ")"; break;
            default: break;
        }
        // Engine readouts only when the definition has them (a TCU's frame has no rpm / clt).
        std::string eng;
        if (meta.hasTelemetry("rpm")) { char e[40]; std::snprintf(e, sizeof e, "  \xC2\xB7  rpm %.0f", c.value("rpm")); eng += e; }
        if (meta.hasTelemetry("clt")) { char e[40]; std::snprintf(e, sizeof e, "  \xC2\xB7  clt %.1f", c.value("clt")); eng += e; }
        char buf[224];
        std::snprintf(buf, sizeof(buf), "%s%s%s%s%s",
                      live.c_str(), live.empty() ? "" : "  \xC2\xB7  ",
                      meta.layoutHash().c_str(), eng.c_str(), lua.c_str());
        return buf;
    }, 250);

    // --- Dock space: protected centre framed by Left / Right / Bottom areas ----
    auto& space = win.dockSpace();
    space.setLeftWidth(settings.get<int>("dock.leftW", 300));
    space.setRightWidth(settings.get<int>("dock.rightW", 300));
    space.setBottomHeight(settings.get<int>("dock.bottomH", 200));

    // Left area: Dictionary + Controls + Tree (tabbed).
    JDockWidget dictionary("Dictionary", 0.f, 0.f, 240.f, 160.f);
    dictionary.setMinSize(120.f, 70.f);
    space.left().addDock(&dictionary);
    JDockWidget widgets("Controls", 0.f, 0.f, 240.f, 160.f);
    widgets.setMinSize(120.f, 70.f);
    space.left().addDock(&widgets);
    JDockWidget tree("Navigation", 0.f, 0.f, 240.f, 160.f);
    tree.setMinSize(120.f, 70.f);
    space.left().addDock(&tree);
    JDockWidget trigger("Trigger Library", 0.f, 0.f, 240.f, 220.f);
    trigger.setMinSize(120.f, 90.f);
    space.left().addDock(&trigger);
    static TriggerLibraryDock triggerLib(g);
    trigger.setContent(&triggerLib);

    // Right area: Properties + Diagnostic Trouble Codes (tabbed).
    JDockWidget properties("Properties", 0.f, 0.f, 240.f, 160.f);
    properties.setMinSize(120.f, 70.f);
    space.right().addDock(&properties);
    JDockWidget dtc("Diagnostic Trouble Codes", 0.f, 0.f, 240.f, 160.f);
    dtc.setMinSize(120.f, 70.f);
    space.right().addDock(&dtc);
    static DtcDock dtcView(g);
    dtcView.setMeta(&meta);   // code → description, so each row explains itself on hover
    dtc.setContent(&dtcView);
    dtcView.onRefresh = [] { if (link.isOpen()) link.readDtcImage(); };
    dtcView.onClear   = [] { if (link.isOpen()) link.clearDtcs(); };
    link.dtcImageReady.connect([](const std::vector<uint8_t>& img) { dtcView.setImage(img); });
    link.openedChanged.connect([](bool open) {
        dtcView.setConnected(open);                 // clear the table on disconnect
        AppStateSigilResolver::connected = open;    // [%connected] tracks the live link (the flag was declared but never set)
        refreshLinkOverlay(open);
    });
    g_noticeWin = &win;
    refreshLinkOverlay(link.isOpen());              // and state the truth before the first frame arrives

    // 1 Hz background poll while connected — drain the ECU text console + refresh the DTC table.
    // WITHOUT it the DTC dock was only ever fed by its manual Refresh
    // button, so a freshly-connected ECU sat on "No DTC data" (the symptom the user hit) instead of the
    // live "N active · M stored" table. Fires on the main thread via the JMainThreadDispatcher drain.
    static jf::JTimer dtcPollTimer;
    dtcPollTimer.onTick.connect([]{ if (link.isOpen()) { link.requestDebugLog(); link.readDtcImage(); } });
    dtcPollTimer.start(std::chrono::milliseconds(1000), jf::JTimer::JMode::Repeating);


    // THE STATUS LAMPS, in a dock of their own. An imported definition's [FrontPage] strip used to be laid
    // on the tab canvas under the viewport: always there, unhideable, and charging every page in the
    // document the height it took. Hosted here it is sized, hidden and closed like any other dock, the
    // pages get that height back, and the lamp grid re-wraps to whatever width the dock is given. Its
    // content is a Surface over the "%Status" page the importer files in the library — nothing is
    // reimplemented, it is the same lamps the same widget draws anywhere else.
    // The dock's own furniture above the lamps: title bar and the padding the host puts round content.
    static constexpr int kStatusDockChrome = 34;
    JDockWidget statusDock("Status Lamps", 0.f, 0.f, 240.f, 120.f);
    statusDock.setMinSize(120.f, 48.f);
    space.bottom().addDock(&statusDock);
    static std::unique_ptr<Surface> statusSurface;

    // Bottom area: Diagnostics + Lua Script (tabbed). Diagnostics = ECU Console / Lua Errors, fed live.
    JDockWidget log("Diagnostics", 0.f, 0.f, 240.f, 160.f);
    log.setMinSize(120.f, 70.f);
    space.bottom().addDock(&log);
    static DiagnosticsDock diag(g);
    log.setContent(&diag);
    link.debugTextReceived.connect([](const std::string& t) {
        diag.appendConsole(t);
        // Tagged Lua errors ("[lua] compile|runtime error L<n>: …") also land on the Lua Errors tab,
        // which pops to front so a failed script can't scroll away unseen (original DiagnosticsDock parity).
        size_t p = 0; bool any = false;
        while (p < t.size()) {
            const size_t nl = t.find('\n', p);
            const std::string line = t.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
            p = (nl == std::string::npos) ? t.size() : nl + 1;
            if (line.rfind("[lua] ", 0) == 0 && line.find(" error L") != std::string::npos) { diag.appendLua(line + "\n"); any = true; }
        }
        if (any) diag.setActiveTab(1);
    });
    link.errorOccurred.connect([](const std::string& e) { diag.appendConsole("[error] " + e + "\n"); });
    link.openedChanged.connect([](bool open) { diag.appendConsole(open ? "[link] opened\n" : "[link] closed\n"); });

    // Wire the View dock-visibility toggles now that the docks + areas exist. The menu item flips its own
    // checked state before onTriggered, so isChecked() is the new desired state: checked = visible (addDock),
    // unchecked = hidden (removeDock — the same primitive the dock close button uses, which prunes the leaf).
    // Where each panel LIVES, tracked continuously (see the housekeeping tick below), so hiding one and
    // showing it again puts it back rather than dumping it in its default area. A dock closed by its × is
    // removed from its host by the framework BEFORE onDockClosed fires, so there is nothing to read at that
    // point — the position has to be already known, hence the running record rather than a capture-on-hide.
    struct DockHome { JDockHost* host{nullptr}; jf::JDockNodeId leaf{}; int tab{-1}; bool floating{false}; };
    static std::map<JDockWidget*, DockHome> dockHomes;
    auto dockHosts = [&space] { return std::array<JDockHost*, 4>{ &space.left(), &space.right(), &space.bottom(), &space.top() }; };
    auto trackHome = [dockHosts, &win](JDockWidget* d) {
        if (win.isDockFloating(d)) { DockHome h; h.floating = true; dockHomes[d] = h; return true; }
        for (JDockHost* host : dockHosts()) {
            const jf::JDockNodeId leaf = host->findDock(d);
            if (leaf == jf::InvalidDockNodeId) continue;
            DockHome h; h.host = host; h.leaf = leaf;
            if (const jf::JDockNode* n = host->node(leaf)) {
                const auto it = std::find(n->tabs.begin(), n->tabs.end(), d);
                h.tab = (it == n->tabs.end()) ? -1 : static_cast<int>(std::distance(n->tabs.begin(), it));
            }
            dockHomes[d] = h;
            return true;
        }
        return false;   // in no host and not floating: hidden, so the last recorded home stands
    };
    // Put a panel back where it was: re-map its floating window, or re-insert into the exact host+leaf+tab
    // it occupied. Falls back to its default area when that leaf is gone (the layout changed since).
    auto showDockHome = [&win](JDockWidget* d, JDockHost& fallback) {
        const auto it = dockHomes.find(d);
        if (it != dockHomes.end()) {
            const DockHome& h = it->second;
            if (h.floating && win.setFloatingDockVisible(d, true)) {
                JLOGC("studio.dock", jf::JLogLevel::Debug) << "show '" << d->title() << "' -> re-mapped its float";
                return;
            }
            if (h.host && h.host->node(h.leaf) && h.host->insertDock(d, h.leaf, h.tab)) {
                JLOGC("studio.dock", jf::JLogLevel::Debug) << "show '" << d->title() << "' -> remembered host, leaf "
                                                           << h.leaf.v << " tab " << h.tab;
                return;
            }
        }
        fallback.addDock(d);
        JLOGC("studio.dock", jf::JLogLevel::Debug) << "show '" << d->title() << "' -> default area (no usable home)";
    };
    // Dock -> its View menu toggle. The toggle IS the record of intent: unchecked means the user closed
    // or hid that panel. Nothing else can tell a panel the layout LOST from one the user put away — both
    // are simply in no host — so the orphan check below would otherwise cry wolf every time someone closed
    // a dock, which is worse than not checking at all: it trains you to ignore the one case it exists for.
    static std::map<JDockWidget*, jf::JMenuItem*> dockToggles;
    auto bindDockToggle = [showDockHome, &win](auto* item, auto& area, JDockWidget& dock) {
        dockToggles[&dock] = item;
        item->onTriggered.connect([item, &area, &dock, showDockHome, &win] {
            JLOGC("studio.dock", jf::JLogLevel::Debug) << "View toggle '" << dock.title() << "' -> "
                                                       << (item->isChecked() ? "SHOW" : "HIDE");
            if (item->isChecked()) showDockHome(&dock, area);
            // Hide it WHEREVER it lives. A sole-occupant float just unmaps (it keeps its size and place);
            // anything else is removed from the host holding it — which is not necessarily this menu item's
            // default area. It used to remove from `area` unconditionally, so a panel that had been moved
            // elsewhere (or shared a float) stayed on screen with its tick saying hidden.
            else if (!win.setFloatingDockVisible(&dock, false)) {
                if (jf::JDockHost* h = dock.placedIn()) h->removeDock(&dock);
            }
        });
    };
    bindDockToggle(tgTree,    space.left(),   tree);
    bindDockToggle(tgProps,   space.right(),  properties);
    // "Properties…" on a run-mode control menu brings the dock back if it is away — the menu item promises
    // a properties view, and a tick left unchecked would make it a no-op for anyone who had closed it.
    // A table's Save/Load needs a file dialog, which is the app's to open.
    Surface::onPickFile = [](std::string title, bool save, std::function<void(std::string)> then) {
        if (save) jf::JDialog::saveFile(title, {"csv", "txt"}, then);
        else      jf::JDialog::openFile(title, {"csv", "txt"}, then);
    };
    // OPERATOR-CONFIRMED actions. Surface has no window, so it asks here.
    //
    // A CONFIRMATION, NOT A LIST. This was a ListPickerDialog: a scrolling list box holding two rows
    // ("Cancel - change nothing" / "Apply") with OK and Cancel buttons underneath it — two ways to
    // cancel, two ways to accept, and the question itself nowhere in the dialog at all, flashed past
    // as a status toast instead. It also said "Apply" on a Reset, from a ternary whose two branches
    // were the same string. A confirm dialog is a title, the question, and a button that names what
    // pressing it does.
    Surface::onConfirmAction = [&win](std::string title, std::string detail, std::string verb,
                                      std::function<void()> go) {
        // THE STUDIO'S OWN DIALOG, not the framework's. JDialog::confirm draws its body as ONE LINE and
        // clips it, and sizes its buttons to a fixed width — so the question arrived cut off
        // mid-sentence and "Apply to Base Table" was clipped at both ends inside a box too small for
        // it. ChoiceDialog wraps the body, sizes each button to its own label, and puts the answer on
        // the button; it was written for the connect reconcile and this is the same question shape.
        if (!go) {                                  // nothing to do: say so and take no answer
            win.openModal<ChoiceDialog>(title, detail,
                std::vector<ChoiceDialog::Choice>{ { "OK", jf::JDialogButtonBox::Role::Accept } },
                std::function<void(int)>([](int) {}));
            return;
        }
        win.openModal<ChoiceDialog>(title, detail,
            std::vector<ChoiceDialog::Choice>{
                { "Cancel",                     jf::JDialogButtonBox::Role::Reject },
                { verb.empty() ? "OK" : verb,   jf::JDialogButtonBox::Role::Accept } },
            std::function<void(int)>([go, &win](int idx) {
                if (idx == 1) { go(); win.showStatus("Done - burn to keep it", 6000); }
                else          win.showStatus("Nothing changed", 3000);
            }));
    };

    // The page buttons ask the same question the table menu does, through the same dialog — one wording,
    // one two-row picker, whichever way the operation was reached.
    LearnedActionWidget::onConfirmAction = Surface::onConfirmAction;

    // A widget with something to report (the script editor's Apply) reaches the one status bar.
    Surface::onWidgetStatus = [&win](const std::string& m) { win.showStatus(m, 3500); };

    Surface::onShowProperties = [showDockHome, &space, &properties, tgProps] {
        if (tgProps && !tgProps->isChecked()) tgProps->setChecked(true);
        if (!properties.placedIn()) showDockHome(&properties, space.right());
    };
    bindDockToggle(tgDict,    space.left(),   dictionary);
    bindDockToggle(tgWidgets, space.left(),   widgets);
    bindDockToggle(tgStatus,  space.bottom(), statusDock);
    bindDockToggle(tgDiag,    space.bottom(), log);
    bindDockToggle(tgDtc,     space.right(),  dtc);
    bindDockToggle(tgTrig,    space.left(),   trigger);
    libTriggerLibrary->onTriggered.connect([tgTrig] {   // Library▸Trigger Library: bring the panel up
        if (!tgTrig->isChecked()) { tgTrig->setChecked(true); tgTrig->onTriggered.emit(); }
    });

    // The View menu's ticks must describe what is ON SCREEN, whatever moved it there. A mode flip strips and
    // restores whole layouts behind the menu's back, so a toggle left over from the other mode read as
    // "visible" for a panel that had been stripped (and vice versa). Rather than patch each path, derive the
    // ticks from the live placement — docked in some host, or floating and mapped — on the housekeeping tick
    // and after every mode change. Set via setChecked, which does not fire onTriggered, so this can't loop.
    {
        struct TogRow { jf::JMenuItem* item; JDockWidget* dock; };
        const std::vector<TogRow> togRows = {
            {tgTree, &tree}, {tgProps, &properties}, {tgDict, &dictionary}, {tgWidgets, &widgets},
            {tgStatus, &statusDock},
            {tgDiag, &log},  {tgDtc, &dtc},          {tgTrig, &trigger},
        };
        g_trackDockHomes = [togRows, trackHome] { for (const auto& r : togRows) trackHome(r.dock); };
        g_syncViewToggles = [togRows, dockHosts, &win] {
            for (const auto& r : togRows) {
                bool visible = win.isDockFloating(r.dock) ? win.isFloatingDockVisible(r.dock) : false;
                if (!visible)
                    for (JDockHost* h : dockHosts())
                        if (h->findDock(r.dock) != jf::InvalidDockNodeId) { visible = true; break; }
                if (r.item->isChecked() != visible) r.item->setChecked(visible);
            }
        };
    }

    // Reverse sync: closing a dock via its title-bar × must uncheck the matching View-menu toggle,
    // so the menu always reflects actual visibility (not just menu-driven hides).
    win.onDockClosed = [&, tgTree, tgProps, tgDict, tgWidgets, tgStatus, tgDiag, tgDtc, tgTrig](jf::JDockWidget* d) {
        if      (d == &tree)       tgTree->setChecked(false);
        else if (d == &properties) tgProps->setChecked(false);
        else if (d == &dictionary) tgDict->setChecked(false);
        else if (d == &widgets)    tgWidgets->setChecked(false);
        else if (d == &statusDock) tgStatus->setChecked(false);
        else if (d == &log)        tgDiag->setChecked(false);
        else if (d == &dtc)        tgDtc->setChecked(false);
        else if (d == &trigger)    tgTrig->setChecked(false);
    };

    // Restore saved dock visibility (default: visible). A dock saved hidden is removed + its toggle unchecked.
    auto applyDockVis = [&settings](auto* item, auto& area, JDockWidget& dock, const char* key) {
        if (settings.get<int>(key, 1) == 0) { item->setChecked(false); area.removeDock(&dock); }
    };
    applyDockVis(tgTree,    space.left(),   tree,        "dock.tree");
    applyDockVis(tgProps,   space.right(),  properties,  "dock.properties");
    applyDockVis(tgDict,    space.left(),   dictionary,  "dock.dictionary");
    applyDockVis(tgWidgets, space.left(),   widgets,     "dock.widgets");
    applyDockVis(tgStatus,  space.bottom(), statusDock,  "dock.statusLamps");
    applyDockVis(tgDiag,    space.bottom(), log,         "dock.diagnostics");
    applyDockVis(tgDtc,     space.right(),  dtc,         "dock.dtc");
    applyDockVis(tgTrig,    space.left(),   trigger,     "dock.trigger");

    // The protected centre is a plain central widget (the surface tabs), set below — not a dock host.

    // Per-dock-host preferences (Appearance ▸ Docks): each tool dock's tab-strip EDGE (0=Top 1=Bottom
    // 2=Left 3=Right) and an ENABLE toggle (a disabled host collapses to zero reserved size, so the centre
    // reclaims it). Applied at startup and re-applied live. Defaults keep the current look: side tabs face
    // the centre, bottom on top, all enabled.
    space.bottom().setTabFill(JTabFill::Fill);
    auto applyDocks = [&space, &settings]() {
        using E = JTabBarEdge;
        auto edge = [](int i){ return i == 1 ? E::Bottom : i == 2 ? E::Left : i == 3 ? E::Right : E::Top; };
        space.left().setTabEdge  (edge(settings.get<int>("ui.dockEdge.left",   3)));
        space.right().setTabEdge (edge(settings.get<int>("ui.dockEdge.right",  2)));
        space.bottom().setTabEdge(edge(settings.get<int>("ui.dockEdge.bottom", 0)));
        space.setLeftWidth   (settings.get<bool>("dock.leftEnabled",   true) ? settings.get<int>("dock.leftW",   300) : 0);
        space.setRightWidth  (settings.get<bool>("dock.rightEnabled",  true) ? settings.get<int>("dock.rightW",  300) : 0);
        space.setBottomHeight(settings.get<bool>("dock.bottomEnabled", true) ? settings.get<int>("dock.bottomH", 200) : 0);
    };
    applyDocks();
    PreferencesDialog::onDocksChanged = applyDocks;

    // --- Dock content: the Tree + Dictionary tree views (framework-hosted) ----------------------
    // Both trees sit under a search box (the studio's "Filter…" / "Filter bindings…"). nodeTree/dictTree
    // are references to the inner JTreeViews, so all existing wiring (setRootNode/rename/context menu) holds.
    FilterTree nodeFT(g, "Filter\xE2\x80\xA6");
    JTreeView& nodeTree = nodeFT.tree();
    tree.setContent(&nodeFT);
    FilterTree dictFT(g, "Filter bindings\xE2\x80\xA6");
    JTreeView& dictTree = dictFT.tree();
    // Match the BINDING PATH as well as the label. The ids are what you see in bindings and in the
    // firmware, so typing "egt" should find the EGT sensors — whose labels read "Exhaust Gas
    // Temperature", so a label-only filter returned nothing for the name you actually know them by.
    dictTree.setFilterMatchesUserData(true);
    // Right-click the dictionary: the tree is deep (122 sensors, each with its own fields, tables and
    // diagnostics groups), so getting back to a readable state after a rummage needed a way to shut it all
    // at once. Branch-level items act on the row under the cursor, which is what you actually want after a
    // search has opened a dozen paths.
    static jf::JMenu dictMenu{ "Dictionary" };
    {
        jf::JTreeView* dt = &dictTree;
        dictMenu.add(g, "Expand All")->onTriggered.connect([dt] { dt->expandAll(); });
        dictMenu.add(g, "Collapse All")->onTriggered.connect([dt] { dt->collapseAll(); });
        dictMenu.addSeparator(g);
        dictMenu.add(g, "Expand Branch")->onTriggered.connect([dt] {
            if (jf::JTreeViewNode* n = const_cast<jf::JTreeViewNode*>(dt->hoveredNode())) dt->setBranchExpanded(*n, true);
        });
        dictMenu.add(g, "Collapse Branch")->onTriggered.connect([dt] {
            if (jf::JTreeViewNode* n = const_cast<jf::JTreeViewNode*>(dt->hoveredNode())) dt->setBranchExpanded(*n, false);
        });
        dictTree.setContextMenu(&dictMenu);
    }

    // Hovering a dictionary row explains it: the field's own help from the definition, with the binding
    // path underneath so the person laying out a page can see exactly what a widget will bind to. Same
    // text a placed widget shows on hover, so the layout author and the tuner read the same words.
    dictFT.setRowTooltipResolver([](const jf::JTreeViewNode& n) -> std::string {
        if (n.userData.empty()) return {};                       // a category row binds nothing
        const std::string help = Cache::instance().help(n.userData);
        return help.empty() ? n.userData : help + "\n" + n.userData;
    });
    dictionary.setContent(&dictFT);
    // Bind File▸"Open Schema…": load a different firmware-schema meta, re-point the Cache at it, and rebuild
    // the dictionary from it (the offline core of New Tune / Open ECU). meta is
    // static, so the lambda reaches it directly.
    // Shared: point the app at a schema meta and rebuild the dictionary from it (the loadMeta core).
    auto loadSchema = [&win, &dictTree, &activeSurf, openTune, noteTsProto](const std::string& path, const std::string& tuneName) {
        if (meta.loadFile(path)) {
            noteTsProto(path);
            EditorSettings::instance().setLastSchema(path);   // come up on this one next launch
            Cache::instance().setMeta(&meta);
            // The SHIPPED wheel library travels with the firmware, in the meta. Refresh it on every
            // meta load so the wheels on offer are the ones THIS ECU can decode — they used to be
            // compiled into the studio, which made them a property of the build instead.
            triggerLib.setShippedWheels(wheelsFromMetaFile(meta.path()));
            dictTree.setRootNode(buildDictionary(meta));
            if (!tuneName.empty()) {
                // New Tune: a detached ECU keyed by layout_hash,
                // seeded with the schema's default calibration, written to disk NOW (no device needed), then
                // opened through the shared openTune choke point.
                if (Ecu* e = Ecu::openOrCreate("offline:" + meta.layoutHash(), meta.board())) {
                    e->saveTune(tuneName, TuneFile::serialise(meta.defaultImage(), meta));
                    openTune(e, tuneName);
                    // A PROJECT IS OPEN NOW, so leave the landing — and only now. The landing used to be
                    // torn down by the button that opened the New Tune dialog, which meant cancelling it
                    // left a blank centre. Loading a schema WITHOUT a tune opens no project and rightly
                    // leaves the landing where it is.
                    if (g_showSurfaces) g_showSurfaces();
                }
            }
            if (auto* s = activeSurf()) s->invalidate();
            win.showStatus(tuneName.empty() ? ("Schema loaded: " + path) : ("New tune '" + tuneName + "'"), 2500);
        } else if (!meta.needsStudio().empty()) tellNeedsStudio(meta.needsStudio());
        else win.showStatus("Failed to load schema: " + path, 3000);
    };
    g_loadSchemaPath = [loadSchema](const std::string& p) { loadSchema(p, ""); };
    g_newDemoTune    = [loadSchema](const std::string& p) { loadSchema(p, "Demo"); };
    openSchemaFn = [&win, loadSchema]{
        jf::JDialog::openFile("Open Schema", {"json", "meta"}, [loadSchema](std::string path) { loadSchema(path, ""); });
    };
    // Import a TunerStudio/rusEFI .ini: parse → native meta JSON → save to the meta library → load it.
    // THE import, by path — shared by the File menu and by --import, so a scripted run exercises exactly
    // the code a user does rather than a parallel copy that can drift from it.
    g_doImport = [&win, &dictTree, &activeSurf, noteTsProto, openTune](std::string path) {
        {
            std::string err;
            auto j = TsIniImporter::importFile(path, &err);
            if (!j) { win.showStatus("Import failed: " + err, 3000); return; }
            namespace fs = std::filesystem;
            const fs::path lib = StudioPaths::dataDir("meta");
            const std::string out = (lib / (fs::path(path).stem().string() + ".meta")).string();
            // writeMetaFile, NOT dumpToFile: a .meta is the JSON body plus its CRC32 footer, and the
            // loader checks that before it parses anything. Writing plain JSON meant every import got as
            // far as "parsed" and then failed to load — the feature never worked end to end.
            TsIniImporter::writeMetaFile(*j, out);
            if (meta.loadFile(out)) {
                noteTsProto(out);
                EditorSettings::instance().setLastSchema(out);   // the next launch comes up on what we imported
                Cache::instance().setMeta(&meta);
            // The SHIPPED wheel library travels with the firmware, in the meta. Refresh it on every
            // meta load so the wheels on offer are the ones THIS ECU can decode — they used to be
            // compiled into the studio, which made them a property of the build instead.
            triggerLib.setShippedWheels(wheelsFromMetaFile(meta.path()));
                dictTree.setRootNode(buildDictionary(meta));

                // The UI half. An import produces a WHOLE PROJECT, not just a dictionary: the [Menu]
                // becomes the navigation tree and every dialog becomes a page of native widgets, written
                // once into dashboard.gui. Nothing reads the ini again — this is a conversion, not a
                // viewer, and the pages behave like anything else you drew.
                std::string iniText;
                { std::ifstream f(path); std::stringstream ss; ss << f.rdbuf(); iniText = ss.str(); }
                const tsdash::Dashboard dash = tsdash::parse(iniText);
                tsconvert::Report rep;
                const jf::JJson doc = tsconvert::buildDashboard(dash, meta, &rep);

                // Keyed by the definition's own layout hash, exactly as New Tune keys an offline project,
                // so re-importing the same ini reopens the same project instead of accumulating copies.
                if (Ecu* e = Ecu::openOrCreate("tsimport:" + meta.layoutHash(), meta.board())) {
                    e->setLabel(meta.tsSignature());
                    doc.dumpToFile(e->dashboardPath(), 2);
                    // A tune to hold the values: the config image starts at the definition's defaults.
                    const std::string tuneName = fs::path(path).stem().string();
                    auto existing = e->tuneNames();
                    if (existing.empty()) {
                        // An ini declares no calibration, so there is no default image to seed from —
                        // start at a zeroed page of the declared size and let the first ECU read fill it.
                        std::vector<uint8_t> seed = meta.defaultImage();
                        if (seed.empty() && meta.configSize() > 0) seed.assign(size_t(meta.configSize()), 0);
                        e->saveTune(tuneName, TuneFile::serialise(seed, meta));
                        existing.push_back(tuneName);
                    }
                    openTune(e, existing.front());
                    if (g_loadProjectDoc) g_loadProjectDoc();      // tree + pages into the live app
                    // An import OPENS a project, so leave the landing page behind exactly as Open ECU
                    // does — otherwise the tree and 273 pages load behind a landing screen that gives no
                    // hint anything happened.
                    if (g_showSurfaces) g_showSurfaces();
                }
                JLOGC("ui.import", jf::JLogLevel::Info)
                    << "ini -> " << rep.pages << " pages, " << rep.widgets << " widgets ("
                    << rep.tables << " table pages, " << rep.emptyPages << " empty, "
                    << rep.skippedItems << " items skipped)";
                if (auto* s = activeSurf()) s->invalidate();
                // SAY WHAT IT WAS LAID OUT TO. The dialog width comes from Preferences ▸ Editor ▸ New
                // page width, and it is fixed at THIS moment — changing it afterwards re-flows nothing,
                // because the pages are already written. Naming it here is the one place a reader can
                // connect the preference to the thing it decided.
                win.showStatus("Imported " + std::to_string(rep.pages) + " pages, "
                               + std::to_string(rep.widgets) + " widgets, laid out to "
                               + std::to_string(EditorSettings::instance().canvasWidth()) + " px wide"
                               + " - re-import to change that", 6000);
            } else win.showStatus("Parsed the .ini but the meta failed to load", 3500);
        }
    };
    importIniFn = []{
        jf::JDialog::openFile("Import TunerStudio .ini", {"ini"},
                              [](std::string path) { if (g_doImport) g_doImport(path); });
    };
    // The prompt's follow-through: import the .ini the user picks, then connect to the ECU that asked for
    // it. Same import the File menu runs — the definition arrives the one way definitions arrive.
    // Fetch the definition rusEFI publishes for this firmware, import it, connect. The transfer runs off
    // the UI thread (JHttpClient::get delivers on the main thread), so the window stays live while a
    // ~700 KB definition comes down. A definition that is not published — a private branch, say — answers
    // 404, and that is reported as what it is rather than as a broken link.
    g_downloadIniThenConnect = [&win](const std::string& url) {
        static jf::JHttpClient http;                 // outlives the callback; one client is plenty
        http.setTimeout(30000);
        win.showStatus("Downloading the definition \xE2\x80\xA6", 30000);
        http.get(url, [&win, url](const jf::JHttpResponse& r) {
            if (!r.error.empty()) { win.showStatus("Download failed: " + r.error, 8000); return; }
            if (r.status == 404) {
                win.showStatus("rusefi.com has no definition for this firmware (404) \xE2\x80\x94 "
                               "choose the .ini yourself", 9000);
                return;
            }
            if (!r.ok()) { win.showStatus("Download failed: HTTP " + std::to_string(r.status), 8000); return; }
            // Land it beside the other definitions, under the name the URL gave it, and import from there
            // — the import reads a path, and a downloaded definition is worth keeping.
            namespace fs = std::filesystem;
            const fs::path dir = StudioPaths::dataDir("ini");
            const std::string name = url.substr(url.rfind('/') + 1);
            const std::string dest = (dir / name).string();
            { std::ofstream f(dest, std::ios::binary);
              if (!f) { win.showStatus("Cannot write " + dest, 6000); return; }
              f.write(reinterpret_cast<const char*>(r.body.data()), static_cast<std::streamsize>(r.body.size())); }
            win.showStatus("Downloaded " + name + " (" + std::to_string(r.body.size()) + " B) \xE2\x80\x94 importing", 5000);
            if (g_doImport) g_doImport(dest);
            if (s_tsProto.isObject() && g_tryTsConnect) g_tryTsConnect();
        });
    };
    g_importIniThenConnect = []{
        jf::JDialog::openFile("Import TunerStudio .ini for this ECU", {"ini"}, [](std::string path) {
            if (!g_doImport) return;
            g_doImport(path);
            if (s_tsProto.isObject() && g_tryTsConnect) g_tryTsConnect();
        });
    };
    // New Tune…: pick a firmware schema from the meta library + name the tune, then load it (offline).
    newTuneFn = [&win, loadSchema]{
        namespace fs = std::filesystem;
        const std::string lib = StudioPaths::dataDir("meta");
        if (NewTuneDialog::scanPool(lib).empty()) { win.showStatus("No schemas in the meta library", 3000); return; }
        win.openModal<NewTuneDialog>(lib,
            std::function<void(std::string, std::string)>([loadSchema](std::string name, std::string metaPath) {
                loadSchema(metaPath, name.empty() ? "untitled" : name);
            }));
    };
    // Dictionary leaves are multi-selectable (Ctrl/Shift-click). Dragging a leaf carries EVERY selected
    // leaf's binding path (or just the dragged one if it isn't part of the selection) → a DictBinding drag.
    // The Surface binds the first path onto a dropped-on control, or tiles one bound control per path on
    // empty canvas.
    dictTree.setMultiSelect(true);
    dictTree.onNodeDragStarted.connect([&dictTree](jf::JTreeViewNode* n) {
        if (!n) return;
        std::vector<std::string> paths;
        const auto sel = dictTree.selectedNodes();
        const bool nInSel = std::find(sel.begin(), sel.end(), n) != sel.end();
        if (nInSel && sel.size() > 1) {                          // drag the whole selection (leaves only)
            for (auto* s : sel) if (s && !s->userData.empty()) paths.push_back(s->userData);
        } else if (!n->userData.empty()) {                       // drag just this leaf
            paths.push_back(n->userData);
        }
        if (!paths.empty()) {
            const std::string label = paths.size() > 1 ? (std::to_string(paths.size()) + " fields") : n->label;
            jf::JDragDrop::start<DictBinding>(DictBinding{std::move(paths)}, 0.f, 0.f, label);
        }
    });

    // Tree context menu (right-click a node). Reuses the framework's floating-menu machinery.
    // --- Navigation-tree editing -------------------------------------------------------------------------
    bool treeEditing = false;             // set by the Locked/Editing toggle; structural ops are edit-only
    static JTreeViewNode treeClip;        // cut/copy clipboard (a whole subtree)
    static bool treeClipFull = false;
    std::string treeSelPath;              // path of the selected node, captured on selection (rename needs the OLD path)
    std::set<std::string> treePaths;      // all node paths as of the last commit (diff a reorder to recover old→new)
    // Relocate the selection inside a fresh copy of the tree, run fn on it; on true, commit + persist.
    // Applying a tree snapshot (undo/redo/mutate all route through this one place).
    // Single funnel: every tree rebuild re-normalises the edit-mode "New node…" placeholders (added while
    // editing, stripped otherwise) before it hits the view. Load/visibility/expand rebuilds route through
    // setTree; undo/redo/mutate route through g_applyTree; onSetEditMode re-syncs on the mode flip.
    // A rebuilt tree arrives UNFILTERED: every node visible, whatever the tune says. Re-apply the condition
    // filter right after, or an imported project shows menu entries the settings rule out until something
    // else happens to trigger it.
    auto setTree = [&](JTreeViewNode r) {
        syncPlaceholders(r, treeEditing);
        nodeTree.setRootNode(std::move(r));
        if (g_refilterTree) g_refilterTree();
    };
    g_applyTree = [&](const JTreeViewNode& t) {
        JTreeViewNode copy = t;
        syncPlaceholders(copy, treeEditing);
        nodeTree.setRootNode(std::move(copy));
        if (g_refilterTree) g_refilterTree();
        g_docDirty = true;
        treePaths.clear(); collectPaths(nodeTree.root(), "", treePaths);   // keep the reorder-diff index current
    };
    auto treeMutate = [&](const std::function<bool(JTreeViewNode&, JTreeViewNode*, const std::vector<int>&)>& fn) {
        std::vector<int> path;
        JTreeViewNode before = nodeTree.root();
        JTreeViewNode copy = nodeTree.root();
        // Mutate on a CLEAN tree: strip ghosts so fn's index math (Move Down bounds, sibling insert positions)
        // never counts a trailing placeholder; g_applyTree re-adds them on apply. Snapshots stay ghost-free too.
        syncPlaceholders(before, false);
        syncPlaceholders(copy, false);
        JTreeViewNode* sel = nullptr;
        if (const JTreeViewNode* s = nodeTree.selectedNode()) {
            if (!s->placeholder && findIndexPath(nodeTree.root(), s, path)) sel = nodeAtPath(copy, path);   // ghosts are never operands
        }
        if (fn(copy, sel, path))
            g_treeUndo.push(new TreeSwap(std::move(before), std::move(copy)));   // push applies via redo()
    };
    auto requireEdit = [&](const char* what) -> bool { if (!treeEditing) { win.showStatus(std::string(what) + " — enable Editing first", 2500); return false; } return true; };

    JMenu treeMenu("Tree");
    treeMenu.add(g, "Expand All")  ->onTriggered.connect([&]{ nodeTree.expandAll();   });
    treeMenu.add(g, "Collapse All")->onTriggered.connect([&]{ nodeTree.collapseAll(); });
    treeMenu.addSeparator(g);
    treeMenu.add(g, "Expand")  ->onTriggered.connect([&]{ treeMutate([](JTreeViewNode&, JTreeViewNode* s, const std::vector<int>&){ if (!s || s->children.empty()) return false; s->expanded = true;  return true; }); });
    treeMenu.add(g, "Collapse")->onTriggered.connect([&]{ treeMutate([](JTreeViewNode&, JTreeViewNode* s, const std::vector<int>&){ if (!s || s->children.empty()) return false; s->expanded = false; return true; }); });
    treeMenu.addSeparator(g);
    // Add Child — new child under the selected node (top level when nothing is selected).
    treeMenu.add(g, "Add Child")->onTriggered.connect([&]{ if (!requireEdit("Add Child")) return;
        std::string np;
        treeMutate([&np](JTreeViewNode& root, JTreeViewNode* s, const std::vector<int>& path){
            JTreeViewNode* parent = s ? s : &root; parent->expanded = true;
            parent->children.push_back(JTreeViewNode{"New node", false, false, {}});
            std::vector<int> ip = path; ip.push_back(static_cast<int>(parent->children.size()) - 1);
            np = joinLabels(root, ip); return true; });
        if (!np.empty()) nodeTree.selectByPath(np);   // new node becomes the active view (see promote-path note)
    });
    // Add Sibling — new node after the selected one under the same parent (top level if none).
    treeMenu.add(g, "Add Sibling")->onTriggered.connect([&]{ if (!requireEdit("Add Sibling")) return;
        std::string np;
        treeMutate([&np](JTreeViewNode& root, JTreeViewNode*, const std::vector<int>& path){
            if (path.empty()) { root.children.push_back(JTreeViewNode{"New node", false, false, {}});
                np = joinLabels(root, {static_cast<int>(root.children.size()) - 1}); return true; }
            std::vector<int> pp(path.begin(), path.end() - 1);
            JTreeViewNode* parent = nodeAtPath(root, pp); if (!parent) return false;
            const int at = path.back() + 1;
            parent->children.insert(parent->children.begin() + at, JTreeViewNode{"New node", false, false, {}});
            std::vector<int> ip = pp; ip.push_back(at);
            np = joinLabels(root, ip); return true; });
        if (!np.empty()) nodeTree.selectByPath(np);
    });
    treeMenu.add(g, "Rename")->onTriggered.connect([&]{ if (requireEdit("Rename")) nodeTree.beginRename(); });
    treeMenu.addSeparator(g);
    // Cut / Copy / Paste — a whole subtree (deep copy). Copy works locked; Cut/Paste need Editing.
    treeMenu.add(g, "Cut")->onTriggered.connect([&]{ if (!requireEdit("Cut")) return;
        if (!nodeTree.selectedNode()) { win.showStatus("Cut — select a node first", 2500); return; }
        treeClip = *nodeTree.selectedNode(); treeClipFull = true;
        treeMutate([](JTreeViewNode& root, JTreeViewNode*, const std::vector<int>& path){
            if (path.empty()) return false; std::vector<int> pp(path.begin(), path.end() - 1);
            JTreeViewNode* parent = nodeAtPath(root, pp); if (!parent) return false;
            parent->children.erase(parent->children.begin() + path.back()); return true; });
        win.showStatus("Cut", 1200);
    });
    treeMenu.add(g, "Copy")->onTriggered.connect([&]{
        if (!nodeTree.selectedNode()) { win.showStatus("Copy — select a node first", 2500); return; }
        treeClip = *nodeTree.selectedNode(); treeClipFull = true; win.showStatus("Copied", 1200);
    });
    treeMenu.add(g, "Paste")->onTriggered.connect([&]{ if (!requireEdit("Paste")) return;
        if (!treeClipFull) { win.showStatus("Paste — clipboard is empty", 2500); return; }
        std::string np;
        treeMutate([&np](JTreeViewNode& root, JTreeViewNode* s, const std::vector<int>& path){
            JTreeViewNode* parent = s ? s : &root; parent->expanded = true;
            parent->children.push_back(treeClip);
            std::vector<int> ip = path; ip.push_back(static_cast<int>(parent->children.size()) - 1);
            np = joinLabels(root, ip); return true; });
        if (!np.empty()) nodeTree.selectByPath(np);
        win.showStatus("Pasted", 1200);
    });
    treeMenu.addSeparator(g);
    // Visibility Condition — the same editor the surface controls use; stored on the node + persisted.
    treeMenu.add(g, "Visibility Condition\xE2\x80\xA6")->onTriggered.connect([&]{ if (!requireEdit("Visibility Condition")) return;
        const JTreeViewNode* s = nodeTree.selectedNode();
        if (!s) { win.showStatus("Visibility Condition — select a node first", 2500); return; }
        std::vector<int> path; if (!findIndexPath(nodeTree.root(), s, path)) return;
        win.openModal<ExpressionEditor>(s->userData,
            buildSigilTree(meta, g_widgetSigils.lister ? g_widgetSigils.lister() : std::vector<std::string>{}), [&, path](std::string cond){
            JTreeViewNode copy = nodeTree.root();
            if (JTreeViewNode* n = nodeAtPath(copy, path)) { n->userData = cond; n->icon = cond.empty() ? 0 : 1;
                setTree(std::move(copy)); g_docDirty = true; }
        }, ExpressionEditor::FirmwareTarget{}, std::string());   // a nav node is not on a page: no element
    });
    treeMenu.addSeparator(g);
    // Delete — remove the selected node (and its subtree) from its parent, then persist.
    treeMenu.add(g, "Delete")->onTriggered.connect([&]{ if (!requireEdit("Delete")) return;
        const JTreeViewNode* sel = nodeTree.selectedNode();
        if (!sel) { win.showStatus("Delete — select a node first", 2500); return; }
        std::string delPath; nodePathOf(nodeTree.root(), sel, "", delPath);
        treeMutate([](JTreeViewNode& root, JTreeViewNode*, const std::vector<int>& path){
            if (path.empty()) return false; std::vector<int> pp(path.begin(), path.end() - 1);
            JTreeViewNode* parent = nodeAtPath(root, pp); if (!parent) return false;
            parent->children.erase(parent->children.begin() + path.back()); return true; });
        onNodeRemoved(delPath);                            // drop the node's pages + close any node tab
        win.showStatus("Deleted node", 1500);
    });
    // Run-mode tree menu: NAVIGATION ONLY. Structural edits + Visibility Condition are edit-only and must
    // not appear when locked, so the context menu SWAPS with the mode.
    // onSetEditMode does the swap; startup is locked → run menu.
    JMenu treeMenuRun("Tree");
    treeMenuRun.add(g, "Expand All")  ->onTriggered.connect([&]{ nodeTree.expandAll();   });
    treeMenuRun.add(g, "Collapse All")->onTriggered.connect([&]{ nodeTree.collapseAll(); });
    treeMenuRun.addSeparator(g);
    treeMenuRun.add(g, "Expand")  ->onTriggered.connect([&]{ treeMutate([](JTreeViewNode&, JTreeViewNode* s, const std::vector<int>&){ if (!s || s->children.empty()) return false; s->expanded = true;  return true; }); });
    treeMenuRun.add(g, "Collapse")->onTriggered.connect([&]{ treeMutate([](JTreeViewNode&, JTreeViewNode* s, const std::vector<int>&){ if (!s || s->children.empty()) return false; s->expanded = false; return true; }); });
    nodeTree.setContextMenu(treeEditing ? &treeMenu : &treeMenuRun);

    // --- Centre: dynamic surface tabs (a permanent Main + surfaces opened on demand) ------------
    // Widget types are data (a registry), not subclasses. The centre hosts a dynamic set of surface
    // tabs (a permanent "Main" plus surfaces opened on demand); each tab is its own Surface, rendered
    // under a derived aspect-preserving transform — no camera.
    SurfaceTabs surfaceTabs(g, Cache::instance(), panelLibrary);
    // Preferences offers the size a page is actually SHOWN at. Its canvas presets are display resolutions,
    // and a page is never shown at display size — the docks, tabs and bars take their share — so on a
    // fixed-size surface a canvas picked from that list is larger than the view and the page scrolls instead
    // of fitting. This is the number that fits, and only the app knows it.
    PreferencesDialog::surfaceAreaFn = [&surfaceTabs]() -> std::pair<int, int> {
        Surface* s = surfaceTabs.activeSurface();
        if (!s) return { 0, 0 };
        const auto bb = s->getBoundingBox();
        return { int(bb.width), int(bb.height) };
    };

    // Wire the @ widget-state sigil to the live surface. A widget is addressed by its "name" prop
    // (falling back to "<type>_<id>"); the lookup finds it on the active surface and calls its
    // descriptor's sigilProp getter. lister enumerates every "<name>.<prop>" for the sigil picker.
    // A widget's stable uid is "<type>_<id>" (id is persisted); its display name is the optional "name"
    // prop (defaults to the uid). Either addresses it in a sigil.
    // A widget is addressed in a sigil by its UID — the persisted "Widget ID" (an RFC-4122 UUID) — or by the
    // legacy "<type>_<id>" key. lookup resolves "<addr>.<prop>" on the active surface: geometry (x/y/w/h) and
    // id read straight off the element; "value" evaluates the widget's bound source (so even a plain
    // value/label widget resolves); any other prop comes from the descriptor's sigilProp getters.
    auto widgetElemValue = [](const PanelElement& e) -> double {
        const std::string src = e.prop("signalName");
        return src.empty() ? 0.0 : MathEvaluator::instance().evaluate(src);
    };
    // Resolve "<addr>.<prop>" against the active surface's LIVE widget tree — top-level AND nested children —
    // so a panel/viewport child is addressable too (widgetByUid walks the whole persistent tree). Geometry /
    // id read straight off the resolved widget's owned element; "value" evaluates its bound source (so even a
    // plain value/label resolves); any other prop comes from the widget's own sigilValue().
    g_widgetSigils.lookup = [&surfaceTabs, widgetElemValue](const std::string& addr, const std::string& prop) -> double {
        Surface* s = surfaceTabs.activeSurface();
        if (!s) return 0.0;
        CanvasWidget* w = s->widgetByUid(addr);
        if (!w || !w->element()) return 0.0;
        const PanelElement& e = *w->element();
        if (prop == "x") return e.x;
        if (prop == "y") return e.y;
        if (prop == "w" || prop == "width")  return e.w;
        if (prop == "h" || prop == "height") return e.h;
        if (prop == "id") return static_cast<double>(e.id);
        const double sv = w->sigilValue(prop, e, Cache::instance());   // class-declared live props win
        if (!std::isnan(sv)) return sv;
        if (prop == "value") return widgetElemValue(e);                // fallback: bound-source value for any widget
        return 0.0;
    };
    // "@<uid>.units" -> the display unit THAT widget is showing. Separate from the sigil lookup above
    // because that one answers with a double and a unit is a string. Resolves through the widget's own
    // element, so it reports the control's displayUnit pin -- or Auto following the global preference,
    // which is what makes a caption track a C/F change at the same instant the gauge does.
    widgetsigil::unitOf() = [&surfaceTabs](const std::string& addr) -> std::optional<std::string> {
        Surface* s = surfaceTabs.activeSurface();
        if (!s) return std::nullopt;
        CanvasWidget* w = s->widgetByUid(widgetsigil::normalizeAddr(addr));
        if (!w || !w->element()) return std::nullopt;          // no such widget — an authoring error
        return CanvasWidget::displayUnitLabelOf(*w->element()); // may be "" — Raw shows no unit, by design
    };
    g_widgetSigils.owns = [&surfaceTabs](const std::string& uid) -> bool {
        Surface* s = surfaceTabs.activeSurface();
        return s && s->hasUid(uid);
    };
    g_widgetSigils.lister = [&surfaceTabs]() -> std::vector<std::string> {
        std::vector<std::string> out;
        if (Surface* s = surfaceTabs.activeSurface()) s->collectSigilTokens(out);   // whole-tree UID-addressed tokens
        return out;
    };
    // The centre starts on the landing card (no project); picking an action
    // swaps the centre to the surface tabs. A backdrop widget — NOT a modal dialog over the window.
    LandingView landingView(g);
    landingView.onChoice[0] = [&]{ if (connectFn) connectFn(); };   // stay on the landing until the ECU's layout loads (identityReceived) — no seed-surface flash
    // THE CENTRE IS AN MDI AREA. A page is a window in it — opened maximised, restorable, movable and
    // sizable — rather than a viewport pinned to a tab canvas. One at a time:
    // choosing a node replaces what is open, because a tuner navigating a tree is reading pages, not
    // collecting them. The surface itself is unchanged; it is simply hosted in a frame now.
    static jf::JMdiArea mdi(g);
    // THE LIVE INSTRUMENTATION STAYS ON SCREEN BEHIND THE WINDOWS. The surfaces carry the readout strip,
    // the channel list and the live graph, and a tuner reads those WHILE editing a page — tuning against a
    // running engine is the whole point. Moving pages into windows had made the centre the MDI area alone,
    // so all of that went off screen the moment a project opened. It is the background of the area now:
    // painted under every window, and it takes the input no window wanted, exactly as it did when it was
    // the central widget itself.
    mdi.setBackground(&surfaceTabs);
    // WHAT MAXIMISE MEANS, from Preferences ▸ Surface. Filling the whole tab is what a maximise button
    // is expected to do and is the default; the other answer stops at the room left beside the live
    // strip and the channel rail, keeping them readable while a page is maximised. The framework holds
    // no opinion — it asks this.
    mdi.setMaximiseFillsArea(EditorSettings::instance().maximiseFillsTab());
    // Live, so the toggle shows what it means while the Preferences dialog is still open: refit() puts a
    // maximised child wherever the policy now says, on the next frame. `mdi` is a function-local static,
    // which is what makes capturing it by reference here safe.
    EditorSettings::instance().changed.connect([] {
        mdi.setMaximiseFillsArea(EditorSettings::instance().maximiseFillsTab());
    });
    // WHERE A WINDOW MAY LAND: the area less the bands the live instruments occupy — the readout strip
    // along the top, the channel rail down the right. Measured ON SCREEN through the surface's own
    // transform, because between page coordinates and this area sit the surface's tab strip and the
    // interface scale. Asked at the moment of placing: before the first frame the answer is all zeroes,
    // and zeroes put the window straight over the readouts it is supposed to keep clear of.
    // WHERE A PAGE WINDOW GOES, DECLARED BY THE TAB IT OPENS OVER (PanelModel::pageArea, in that
    // surface's page units). A tab knows what it leaves beside its own instruments; it does not have to be
    // reverse-engineered from where they landed, which is what this did before — a "band" grown by chained
    // adjacency from the topmost widget anywhere on the surface, and a "rail" only recognised past 70% of
    // the width. On the idle tab that shoved a window a third of the way down a screen with nothing up
    // there (its readouts are on the RIGHT) and then failed to keep it off them anyway.
    //
    // THE CONVERSION IS A TRANSLATION, NOT THE CAMERA. These surfaces reflow — the canvas IS the viewport,
    // widgets keep the coordinates they were authored in — so a page unit is a pixel times the interface
    // scale, offset by where the area starts. Running it through the surface's own transform is what put
    // the top-left in the wrong place last time: that transform carries centring and fit offsets meant for
    // drawing a canvas INTO a viewport, which is not the question being asked here.
    mdi.workArea = [&surfaceTabs](const jf::JRect& a) -> jf::JRect {
        Surface* bg = surfaceTabs.activeSurface();
        const PanelModel* m = bg ? bg->model() : nullptr;
        if (!m) return a;
        const jf::JRect pa = m->pageArea();
        if (pa.width <= 0.f || pa.height <= 0.f) return a;      // a tab that reserves nothing gives all of it
        // FROM WHERE THE TAB WIDGET DRAWS ITS PAGE, not the area's corner and not the page widget's own
        // bounds. The area's corner is wrong because the tab strip sits inside the area, so a surface's
        // unit 0 is a strip-height below it. The page widget's bounds are wrong because they are stale
        // until it has been laid out at least once — a tab shown for the FIRST time still carries whatever
        // it had before, so the same tab placed its window 34 px higher on the first visit than on every
        // visit after. The tab widget knows the answer before anything is painted, and gives one answer.
        const jf::JRect pr = surfaceTabs.pageRect();
        const float ox = (pr.width  > 0.f) ? pr.x : a.x;
        const float oy = (pr.height > 0.f) ? pr.y : a.y;
        const float sc = jf::JStyle::uiScale() > 0.f ? jf::JStyle::uiScale() : 1.f;
        jf::JRect w{ ox + pa.x * sc, oy + pa.y * sc, pa.width * sc, pa.height * sc };
        // THE RECTANGLE DESCRIBES WHERE THE PAGE GOES, and the window's chrome sits OUTSIDE it: a 26 px
        // title bar above, a 6 px border down each side and along the bottom. Placing the FRAME on those
        // numbers puts the page a title lower and 12 px narrower than the tab asked for — which is a
        // horizontal scroll bar on a page that was authored to exactly the width the tab reserves. The
        // frame is grown outwards by its own chrome, so the page lands on the lines the tab drew.
        w.x      -= jf::JMdiChild::kBorder;
        w.width  += 2.f * jf::JMdiChild::kBorder;
        w.y      -= jf::JMdiChild::kTitleH;
        w.height += jf::JMdiChild::kTitleH + jf::JMdiChild::kBorder;
        if (w.x < a.x) { w.width  -= (a.x - w.x); w.x = a.x; }   // never outside the area it lives in
        if (w.y < a.y) { w.height -= (a.y - w.y); w.y = a.y; }
        // …and never outside the area it lives in, however the tab was authored.
        w.width  = std::max(120.f, std::min(w.width,  a.x + a.width  - w.x));
        w.height = std::max(80.f,  std::min(w.height, a.y + a.height - w.y));
        return w;
    };
    static std::unique_ptr<Surface> pageClosed;
    static std::string pageOpen;
    // WHICH MODE A PAGE OPENS IN, and the one every open page is switched to when the Locked/Editing
    // toggle flips. The toggle drove surfaceTabs and nothing else, so once pages moved into the MDI the
    // page actually on screen was never told: the docks changed, the tree became editable, and the page
    // itself stayed in run mode — edit mode looked like it simply did not work.
    static Surface::Mode pageMode = Surface::Mode::Run;
    // WHICH WINDOW IS THE PAGE. The centre holds tools as well now, so "the page went away" has to mean
    // this one window and not whatever else is open: closing the Trigger Designer is not closing the page.
    static jf::JMdiChild* pageChild = nullptr;
    // WHICH PAGE EACH TAB IS SHOWING. A tab is a workspace: the idle tab has the idle table up, the main
    // tab whatever was last opened there, the boost tab its boost map — so the page window belongs to the
    // TAB, not to the centre. Switching tabs swaps the window over (tabChanged, below); it does not carry
    // one workspace's page into another's.
    static std::map<const Surface*, std::string> g_tabPage;
    // …and WHICH TAB the open window belongs to. "pageOpen" alone answers "which page is up", which is
    // not the same question: select Idle Control while on Main and the idle tab's own page is that same
    // path, so a path comparison says "already open" and the idle tab is left showing the window that
    // belongs to Main — i.e. nothing, since Main is no longer in front.
    static const Surface* g_pageTab = nullptr;

    mdi.onChildClosed = [&](jf::JMdiChild* c) {
        if (c != pageChild) return;                    // a tool window closing says nothing about the page
        // The owner goes with the window. Leaving a stale one behind makes the next tab change compare
        // against a tab whose window no longer exists, and decide it has nothing to do.
        //
        // CLOSING A PAGE IS A DECISION ABOUT THIS TAB. The tab forgets what it had up, so coming back to
        // it shows nothing — rather than reopening the window you just closed, which would make the ✕
        // look broken. Opening any page on it starts it remembering again.
        if (g_pageTab) g_tabPage.erase(g_pageTab);
        pageChild = nullptr; pageClosed = std::move(pageSurface); pageOpen.clear(); g_pageTab = nullptr;
    };
    g_invalidatePageSurface = [&surfaceTabs]{
        surfaceTabs.invalidate();                      // the tab surfaces…
        if (pageSurface) pageSurface->invalidate();    // …and the page open in the MDI
    };
    auto openPage = [&](const std::string& path) {
        // Already showing this page ON THIS TAB is the only "nothing to do" case. Showing it on another
        // tab is not: the window belongs to that tab and goes when it does.
        if (path.empty() || (path == pageOpen && surfaceTabs.activeSurface() == g_pageTab)) return;
        PanelModel* pm = panelLibrary.find(path);
        if (!pm) return;                       // a category: leave what is open, as before
        // NOT OVER A TAB THAT IS ALL INSTRUMENT. Diagnostics fills its surface with the channel table it
        // exists to show; a page window there covers the reading you opened the tab for. Such a tab
        // declares a zero page area, and a page opened while it is in front moves to a tab that takes one.
        if (!surfaceTabs.showTabTakingPages()) return;
        // THE OUTGOING PAGE IS PARKED, NOT DESTROYED. A link on a page opens another page from INSIDE that
        // page's own press handler (LabelWidget → hyperlink::navigator → selectByPath → here), so the
        // surface being replaced is still on the call stack: assigning over pageSurface freed it mid-click,
        // and the handler went on reading the label's link string and returning through the dead panel
        // and surface — a use-after-free that corrupted the heap and crashed later somewhere unrelated
        // (malloc_consolidate, from the title bar). It waits in pageClosed until the NEXT page opens, by
        // which time nothing is running inside it; whatever was parked before goes now, and that one
        // cannot be on the stack. mdi.close() does not fire onChildClosed, so it does not touch this — nor
        // does it cut the content's parent edge (a dying widget used to cut its own), so that goes here,
        // as the ✕ reaper does, or the parked page would stay in the tree under the MDI.
        pageClosed = std::move(pageSurface);
        if (pageChild) {                       // …the previous page, and only it
            if (pageChild->content()) mdi.removeChild(pageChild->content());
            mdi.close(pageChild); pageChild = nullptr;
        }
        win.setCentralWidget(&mdi);            // a tool may have taken the centre; the page needs it back
        pageSurface = std::make_unique<Surface>(g, Cache::instance(), pm);
        // KEEP CLEAR OF THE LIVE BAND. The surface behind the windows carries its readouts along the top
        // and its channel rail down the right; a window placed at the area's own corner covers exactly
        // what the reader wants to watch while they work. Measured from that surface rather than assumed,
        // so a document that arranges its instruments differently is still respected: the top band is
        // whatever starts at the very top, the right rail whatever ends at the very right.
        pageSurface->setMode(pageMode);   // as above: opened to be read unless the app is in edit mode
        // The window's title bar already says which page this is — one header, not two.
        pageSurface->setPageTitleShown(false);
        const size_t slash = path.rfind('/');
        // THE INSPECTOR FOLLOWS THIS PAGE. Everything the Properties dock reads was bound to the active
        // TAB surface, and a page has not lived in a tab since it moved into the MDI: selecting a widget
        // left the inspector showing "Surface canvas", and its undo, repaint and apply-edit hooks all
        // pointed at a surface the author was not looking at. Edit mode was a mode you could enter and
        // not use.
        pageSurface->selectionChanged.connect([]{ if (g_syncProps) g_syncProps(); });
        pageChild = mdi.open(slash == std::string::npos ? path : path.substr(slash + 1), pageSurface.get());
        if (g_syncProps) g_syncProps();
        pageOpen = path;
        g_pageTab = surfaceTabs.activeSurface();      // the tab this window belongs to
        if (g_pageTab) g_tabPage[g_pageTab] = path;  // …and the page it is showing
    };
    // …and the window follows the tab. Closing what the old tab had up and opening what the new one had
    // is the whole of it: a page window is cheap to build, and keeping several alive to hide and show
    // would mean four surfaces listening to every telemetry frame to draw nothing.
    surfaceTabs.tabChanged.connect([&](Surface* tab) {
        const auto it = tab ? g_tabPage.find(tab) : g_tabPage.end();
        // What this tab had up, or — the first time it is shown — the page it opens on. A workspace is
        // named for what you do in it and should arrive showing that: the idle tab on Target RPM, the
        // boost tab on Boost Target. Without the second half a tab not yet visited came up blank while
        // every other tab remembered its page, which reads as that tab being broken.
        std::string want = (it != g_tabPage.end()) ? it->second : std::string();
        if (want.empty() && tab) want = surfaceTabs.defaultPageOf(tab);
        if (want == pageOpen && tab == g_pageTab) return;   // this tab's own window is already up
        if (pageChild) { mdi.close(pageChild); pageChild = nullptr; }
        pageOpen.clear();
        if (want.empty()) return;
        openPage(want);
        // AND THE TREE POINTS AT IT. A tab is a workspace, so arriving on one means the tree should show
        // which page that workspace is on — otherwise the highlight says one thing, the window shows
        // another, and the next click in the tree looks like it did nothing (it re-selects the node that
        // was already highlighted). Selecting fires the tree's own handler, which calls openPage with the
        // path just opened and returns at its first line: nothing loops.
        nodeTree.selectByPath(want);
    });
    // A TOOL IS A WINDOW TOO. These used to open by making the tab area the central widget — which is
    // what the centre WAS before a page became a window. surfaceTabs is the MDI's BACKGROUND now, so
    // handing it to the window as the centre pulled it out from under the area and took every page window
    // with it: open the Trigger Designer once and no page was ever visible again, whatever you clicked.
    // The centre is the MDI, always; a tool opens in it like a page does, and opening it twice raises the
    // window that is already there.
    // A TOOL IS A TAB. The designer, the engine-cycle view, the knock scope, the autotuner and the trigger
    // log are each a whole workspace of their own — they are not a page you read beside the instruments,
    // they are the thing you are doing — so they get a tab, and the tab strip is where you leave them and
    // come back to them.
    //
    // The centre stays the MDI. That is the whole of what went wrong when these were tabs before: they
    // opened with win.setCentralWidget(&surfaceTabs), which was right while the tab area WAS the centre
    // and became fatal when a page moved into a window — surfaceTabs is the MDI's background now, so
    // handing it to the window pulled it out from under the area and took every page window with it.
    // Opening a tool tab must not touch what the central widget is.
    //
    // And a tool tab shows nothing over it: page windows belong to tabs (g_tabPage), a tool tab has no
    // page, so arriving on one closes whatever window the previous tab had up and leaves the tool clear.
    auto openToolTab = [&](const std::string& title, jf::JWidget* content) {
        win.setCentralWidget(&mdi);
        surfaceTabs.openTool(title, content);
    };
    // THE LANDING STAYS UNTIL A PROJECT IS ACTUALLY OPEN. These used to reveal the surfaces first and
    // ask afterwards, so cancelling either picker — or a picker that never appeared, because the library
    // is empty or the ECU has no tunes — left the user staring at a blank centre with no landing, no
    // project and no way back except File ▸ Close Project. Nothing here changes what is on screen:
    // whoever opens the project calls g_showSurfaces once it IS open (g_openProject for an existing ECU,
    // loadSchema for a new tune), which is the same rule "Connect" already followed.
    landingView.onChoice[1] = [&]{ if (newTuneFn)  newTuneFn(); };
    landingView.onChoice[2] = [&]{ if (openEcuFn)  openEcuFn(); };
    g_showSurfaces = [&win] { win.setCentralWidget(&mdi); };
    win.setCentralWidget(&landingView);   // the centre is a widget, not a dock host
    // File▸Close ECU is bound further down, once the document loader exists (it empties the document).
    activeSurf = [&surfaceTabs] { return surfaceTabs.activeSurface(); };   // bind the Edit menu now
    // A NAME OF ITS OWN. Every one of these used to be called "Surface", which is how two of them ended
    // up indistinguishable in the reopen list — and, before the index fix, how one of them became
    // unreachable. Numbered from what is already in the pool, so the name says which one it is.
    onNewSurface = [&surfaceTabs] {
        const auto have = surfaceTabs.surfaces();
        std::string name = "Surface";
        for (int n = 2; std::any_of(have.begin(), have.end(),
                                    [&name](const SurfaceTabs::SurfaceRef& s){ return s.name == name; }); ++n)
            name = "Surface " + std::to_string(n);
        surfaceTabs.newSurface(name);
    };
    static TriggerDesignerView triggerView(g);   // the centre editor-tab content (persists; caller-owned)
    g_importTriggerWheels = [](const std::string& p) { return triggerLib.importFrom(p); };
    g_exportTriggerWheels = [](const std::string& p) { return triggerLib.exportTo(p); };
    // New wheel: a blank design, named so it can be saved. Nothing is written until Save.
    triggerLib.onNew = [&win] {
        Wheel w;
        w.name = "New wheel";
        w.sync = "CRANK";
        w.streams.push_back(gapStream(0, 36, 2, {0}));   // a plain 36-1 is the least surprising start
        triggerView.loadWheel(w);
        win.setStatusText("new wheel \xE2\x80\x94 name it and press Save to library");
    };
    triggerLib.onDeleted        = [&win](const std::string& n){ win.setStatusText("deleted '" + n + "'"); };
    triggerLib.onDeleteRefused  = [&win](const std::string& why){ win.setStatusText(why); };
    // Loading a wheel REPLACES the working copy, so unsaved edits die with it. Ask first — the
    // designer is an editor and every drag and keystroke lands in that working copy, so picking
    // another library entry used to throw the work away in silence.
    static std::function<void(std::function<void()>)> guardTriggerEdits;
    guardTriggerEdits = [&win](std::function<void()> then) {
        if (!triggerView.isDirty()) { then(); return; }
        const std::string nm = triggerView.workingName();
        win.openModal<SaveChangesDialog>(
            std::string("Save changes to '" + (nm.empty() ? std::string("this wheel") : nm) + "' first?"),
            std::function<void(int, bool)>([&win, then](int result, bool) {
                if (result == 0) return;                      // Cancel — the load is off
                if (result == 1) {                            // Save, then carry on
                    if (triggerView.workingName().empty()) {
                        win.setStatusText("name the wheel before saving it");
                        return;
                    }
                    if (!triggerLib.addUserWheel(triggerView.workingWheel()))
                        { win.setStatusText("could not save the wheel"); return; }
                    win.setStatusText("saved '" + triggerView.workingName() + "' to the wheel library");
                }
                triggerView.discardEdits();
                then();
            }));
    };
    triggerLib.onSelect = [](const Wheel& w) {                 // library pick → preview/edit (no config write)
        guardTriggerEdits([w]{ triggerView.loadWheel(w); });
    };
    // Double-click / Enter: load it AND bring the designer up, so the library visibly does something.
    triggerLib.onActivate = [&onOpenTriggerDesigner](const Wheel& w) {
        auto open = &onOpenTriggerDesigner;
        guardTriggerEdits([w, open]{ triggerView.loadWheel(w); if (*open) (*open)(); });
    };
    // Install the working wheel into the LIVE config. Deliberate, and separate from saving it to the
    // library: a wheel can be worth keeping without being the one this ECU runs. RAM only — burning
    // is still the per-dialog footer's job.
    triggerView.settings().onApplyToEcu = [&win](const Wheel& w) {
        const int n = applyWheelToConfig(w, Cache::instance());
        // SAY WHAT IT DID, not what state the ECU is in. "(not burned)" was a claim about
        // persistence written into a PERMANENT status line, while the burn that falsifies it only
        // shows a 2.5 s transient — so burning made the line wrong and then handed the screen back
        // to it. The Burn button already answers the state question, and answers it properly: it
        // reads the ECU's own config_dirty rather than anything the studio remembers doing.
        win.setStatusText("applied '" + (w.name.empty() ? std::string("wheel") : w.name) +
                          "' \xE2\x80\x94 " + std::to_string(n) +
                          " settings written to the ECU's live config");
    };
    // "Save to library" was emitted and connected NOWHERE, so an authored wheel was built and
    // dropped on the floor. It now persists to the user's own store and appears in the list
    // immediately — saving must not depend on the app exiting cleanly.
    triggerView.settings().onSaveFailed = [&win](const std::string& why) { win.setStatusText(why); };
    triggerView.settings().onSaveToLibrary = [&win](const Wheel& w) {
        if (!triggerLib.addUserWheel(w))
            win.setStatusText("could not save '" + w.name + "' to the wheel library");
        else
            win.setStatusText("saved '" + w.name + "' to the wheel library");
    };
    onOpenTriggerDesigner = [openToolTab] {
        JLOGC("ui.lazy", jf::JLogLevel::Info) << "View\xE2\x96\xB8Trigger Designer \xE2\x86\x92 opening centre tool tab";
        openToolTab("Trigger Designer", &triggerView);
    };
    // The engine-cycle view: one cycle in DEGREES, a lane per coil/injector/trigger input. A centre
    // tool tab like the designer above — the only area wide enough for 720 (or a rotary's 1080)
    // against a dozen-plus lanes without compressing it into uselessness.
    static EngineCyclePanel engineCyclePanel(g);
    // Ring size is a preference: how much history is worth holding depends on what you are chasing.
    // A drifting spark needs a handful of cycles; an intermittent misfire needs a long tail.
    engineCyclePanel.setCapacity(static_cast<size_t>(
        jf::JSettings::instance().get<int>("engineCycle.frames", 60)));
    // A capture is REQUESTED, never streamed: opening (or re-picking) View▸Engine Cycle asks the ECU
    // for exactly one cycle. That is the whole gate — the ECU records only during the single cycle we
    // asked for, so the cost is the same at 20k rpm as at idle, and re-picking the menu item is the
    // re-capture gesture.
    // The engine cycle a rusEFI ECU is running in. It never reports a cycle span in its tooth log,
    // so it has to come from the connected tune — its `twoStroke` bit (1 = two-stroke). Absent, a four-stroke is
    // the right default, and it is the ONLY guess in this path: everything else is measured.
    auto foreignCycleAngle = [] {
        const auto& cache = Cache::instance();
        return (cache.has("twoStroke") && cache.value("twoStroke") != 0.0) ? 360.0 : 720.0;
    };

    // Capture one cycle from a rusEFI ECU.
    //
    // Its tooth logger fills a buffer on trigger activity and only hands one over when it is full
    // (250 records) or 5 s have passed, answering "out of range" until then. So this is a POLL, and
    // it is driven off a timer rather than a blocking loop: TsLink is synchronous, and spinning on
    // it here would freeze the UI for seconds on an engine that is barely turning.
    //
    // The logger is switched off again when we are done. It costs the ECU work on every trigger
    // edge, and we asked for that — leaving it running after one capture would be helping ourselves
    // rather than extending a hand.
    static jf::JTimer rusefiPoll;
    static int        rusefiPollsLeft = 0;
    static double     rusefiSpan      = 720.0;
    static bool       rusefiBusy      = false;

    auto finishRusefi = [](const std::string& status) {
        rusefiPoll.stop();
        rusefiBusy = false;
        if (tsLink.isOpen()) tsLink.compositeDisable();     // hand the ECU's per-edge cost back
        if (!status.empty()) {
            JLOGC("comms.cycle", jf::JLogLevel::Warn) << "rusefi capture: " << status;
            engineCyclePanel.pause();
            engineCyclePanel.setStatus(status);
        }
    };

    static std::function<void()> rusefiTick;
    rusefiTick = [finishRusefi] {
        if (!tsLink.isOpen())              { finishRusefi("link closed during capture"); return; }
        if (--rusefiPollsLeft < 0)         { finishRusefi("no composite data \xE2\x80\x94 is the engine turning?"); return; }

        const std::vector<uint8_t> raw = tsLink.compositeRead();
        if (raw.empty()) return;                            // buffer not ready yet — normal, keep polling

        // The ECU's own rpm. Needed because a 250-record buffer spans about ONE engine cycle, so it
        // usually holds a single TDC toggle — one anchor, which fixes WHERE tdc is but not how fast
        // the crank turned. Two marks make the buffer self-describing and this is ignored.
        const auto& cache = Cache::instance();
        const double rpm = cache.has("RPMValue") ? cache.value("RPMValue") : 0.0;

        enginecycle::Cycle c;
        const auto r = rusefi_cycle::decodeCycle(raw.data(), raw.size(), rusefiSpan, c, rpm);
        JLOGC("comms.cycle", jf::JLogLevel::Info)
            << "rusefi buffer: " << raw.size() << " bytes, " << r.samples << " samples, "
            << r.tdcMarks << " tdc marks, rpm=" << rpm
            << (r.singleAnchor ? " (single-anchor)" : " (bracketed)") << " -> " << r.message;
        // A buffer with fewer than two TDC marks is not a failure, it is an early one: the engine
        // may simply not have turned a whole cycle yet. Keep polling until the budget runs out, and
        // report the decoder's own reason if it never does.
        if (!r.ok) {
            if (rusefiPollsLeft <= 0) finishRusefi(r.message);
            return;
        }
        finishRusefi({});                                    // stop + disable, keep the picture
        engineCyclePanel.addFrame(c);                        // appends, follows if live, asks for the next
    };

    auto captureRusefiCycle = [](double span) {
        if (rusefiBusy) return;
        rusefiBusy      = true;
        rusefiSpan      = span;
        rusefiPollsLeft = 60;                                // ~12 s at 200 ms — past the ECU's 5 s flush
        tsLink.compositeEnable();
        rusefiPoll.start(std::chrono::milliseconds(200), jf::JTimer::JMode::Repeating);
    };
    rusefiPoll.onTick.connect([] { if (rusefiTick) rusefiTick(); });

    onOpenEngineCycle = [openToolTab, foreignCycleAngle, captureRusefiCycle] {
        JLOGC("ui.lazy", jf::JLogLevel::Info) << "View\xE2\x96\xB8" "Engine Cycle \xE2\x86\x92 opening centre tool tab";
        openToolTab("Engine Cycle", &engineCyclePanel);
        // OPENING IS NOT STARTING. Recording began on the menu pick, so simply looking at the tool
        // armed it — and with the retry loop it would then sit re-requesting for as long as the tab
        // was open, whether or not anybody wanted a capture. Start is a decision; the button makes it.
        engineCyclePanel.setCapacity(static_cast<size_t>(
            jf::JSettings::instance().get<int>("engineCycle.frames", 60)));
        if (!link.isOpen() && !tsLink.isOpen()) { engineCyclePanel.setStatus("Not connected"); return; }
        engineCyclePanel.setStatus("Press Start to record cycles");
    };

    // The cycle view's notice states ONE condition — the trigger log is holding the decoder down — so
    // it is DERIVED from that state wherever the state can change, never latched by whoever set it.
    //
    // It used to be cleared in exactly one place: the trigger log panel's Stop button. Every OTHER way
    // the log can end therefore stranded the banner over a view that was free to capture again — and
    // the commonest of those is a REFUSED ARM. Start the log with the engine running and the ECU says
    // no (it will not disable a decoder that is keeping the engine alive), which clears the running
    // flag asynchronously, long after onStart put the notice up. The result was a view permanently
    // captioned "the trigger log has the decoder disabled" by a trigger log that never started.
    // A rejected read and a dropped link do the same thing by the same route.

    // The panel asks; this decides WHO answers. Native first, exactly as connect does — a jayecu ECU
    // reports the angle it actually fired at, so there is nothing to reconstruct. A rusEFI ECU gets
    // its tooth log converted into the same model. One feature, two producers.
    // Re-arm hook for the capture retry timer, which is created further down with the failure handler
    // it shares. Declared here because onCaptureRequest below needs it and runs first.
    static std::function<void()> cycleRetryArm;
    engineCyclePanel.onCaptureRequest = [foreignCycleAngle, captureRusefiCycle] {
        // THE TRIGGER LOG NO LONGER BLOCKS THIS. It used to switch the ECU's decoder off, so there
        // was no angle to capture and asking anyway produced a request storm — measured at ~55/s.
        // The log leaves the decoder alone now, so both tools run at once and this needs no guard.
        if (link.isOpen())        { engineCyclePanel.setStatus("Capturing\xE2\x80\xA6"); link.captureEngineCycle(); return; }
        if (tsLink.isOpen())      { engineCyclePanel.setStatus("Capturing from rusEFI\xE2\x80\xA6");
                                    captureRusefiCycle(foreignCycleAngle()); return; }
        // Not connected is a WAIT too — the link comes back, and the recording should still be running
        // when it does. Retried on the same timer rather than ended.
        engineCyclePanel.setStatus("Waiting for a connection \xE2\x80\xA6");
        if (engineCyclePanel.recording()) cycleRetryArm();
    };
    link.engineCycleReady.connect([](const std::vector<uint8_t>& img) {
        enginecycle::Cycle c;
        const cyclewire::Result r = cyclewire::decode(img.data(), img.size(), c);
        JLOGC("comms.cycle", jf::JLogLevel::Info)
            << "capture " << r.message << " edges=" << r.total << " dropped=" << int(r.dropped);
        if (!r.ok) {
            // Same rule as a failed capture: a bad frame is not a reason to end a recording the user
            // started. Report it and let the next cycle land.
            engineCyclePanel.setStatus(r.message);
            // Paced, not immediate: a frame that will not decode would otherwise re-request as fast as
            // the link can deliver it.
            if (engineCyclePanel.recording()) cycleRetryArm();
            return;
        }
        // An empty capture is not an error — a synced ECU whose tune maps no coils or injectors
        // genuinely drives nothing. Say that, rather than showing a blank plot that reads as a fault.
        if (c.empty())
            engineCyclePanel.setStatus("Captured, but no edges \xE2\x80\x94 no outputs are mapped in this tune");
        engineCyclePanel.addFrame(c);   // appends, follows if live, and asks for the next
    });
    // ---- View▸Trigger Log — the RAW time-domain log (0x27) -----------------------------------
    //
    // ITS OWN VIEW, deliberately not the cycle view. A trigger log has no angle in it and none can be
    // computed: edges/second is (features per revolution) x (revolutions per second), one equation
    // with two unknowns, so a one-cylinder distributor at a million rpm and a million-cylinder one at
    // 1 rpm produce identical captures. The cycle view's axis is degrees; there are none here.
    //
    // The two are exact complements. The cycle view is empty without sync, because its angles come
    // from the decoder. This one needs none of it: it records what the pin did, whatever anything
    // downstream makes of the edges, so it can be taken at any time and on a running engine.
    static TriggerLogPanel triggerLogPanel(g);

    // Lane names from the tune's configured stream ROLE. The log itself has no idea which line is a
    // cam — that is a property of a wheel the decoder understands, and this exists for one it does not.
    // Lane names. THE STREAM INDEX IS THE ROLE, so these are fixed and need nothing from the config —
    // which also means a lane is named correctly before the studio has read a tune, where the old
    // lookup silently produced "Stream 0" for a live synced ECU because an ABSENT cache entry and a
    // configured-zero one were indistinguishable.
    auto triggerLogStreamNames = [] {
        return std::vector<std::string>{ "Crank Primary", "Crank Secondary",
                                         "Cam Intake B1", "Cam Exhaust B1",
                                         "Cam Intake B2", "Cam Exhaust B2" };
    };

    triggerLogPanel.onStart = [] {
        if (!link.isOpen()) {
            triggerLogPanel.setStatus("Trigger log needs a jayecu ECU \xE2\x80\x94 not connected");
            return;
        }
        // The cycle recorder used to be stopped here: the two were mutually exclusive because this
        // one switched the ECU's decoder off, so a cycle capture could never close a boundary and
        // its recorder — which re-requests on every reply — spun at ~55 requests/second, saturating
        // the link and drowning the log. The trigger log leaves the decoder alone now, so both run.
        triggerLogPanel.setRunning(true);
        link.startTriggerLog();
    };
    triggerLogPanel.onStop = [] {
        link.stopTriggerLog();          // ends the capture on the ECU (the decoder was never touched)
        triggerLogPanel.setRunning(false);
        triggerLogPanel.setStatus("Stopped");
    };
    // The ECU stops appending by itself when the buffer fills, so there is no STOP to send here —
    // the capture ended itself.
    link.triggerLogComplete.connect([] { triggerLogPanel.setComplete(); });
    link.triggerLogUpdated.connect([triggerLogStreamNames](const std::vector<triggerlog::Record>& recs) {
        triggerLogPanel.setRecords(recs, triggerLogStreamNames());
        // THE SAME CAPTURE GOES TO THE DESIGNER, which fits a wheel to it and draws the result under
        // whatever is being drawn. Nothing is changed by this: the measurement appears as ghost ticks
        // and a "measured 36-1 at 1180 rpm" line, and adopting it is a button the user presses. A
        // capture off a misfiring engine is still a capture.
        std::vector<triggerlog::Lane> lanes;
        if (triggerlog::decodeLanes(recs, triggerLogStreamNames(), lanes).ok)
            triggerView.setCapture(lanes);
    });
    link.triggerLogFailed.connect([](const std::string& why) {
        JLOGC("comms.cycle", jf::JLogLevel::Warn) << "trigger log: " << why;
        triggerLogPanel.setRunning(false);
        triggerLogPanel.setStatus(why);
    });

    // ---- View▸Knock Scope — the classifier's latest verdict, with its evidence ----------------
    //
    // Polled rather than streamed: 0x28 returns whatever was last classified, and `seq` says whether
    // it is new. A tuner watching this while a burst is injected sees WHERE in the window the energy
    // landed relative to the spark, and how far over the learned floor it was — which is the basis of
    // both the knock threshold and the pre-ignition conjunction, and is not inferable from any single
    // telemetry number.
    static KnockScopeView knockScope(g);
    static jf::JTimer     knockScopePoll;
    // ALTERNATE the two questions. Asking only for the last EVENT leaves the panel frozen on a
    // healthy engine — the event is by definition rare and static. Asking only for the LATEST shows a
    // quiet window forever, because at 50 windows/sec the one that fired is gone before any poll
    // reaches it. So each tick swaps: the live trace proves the scope is running, the held event is
    // the thing worth reading.
    static bool wantEvent = false;
    knockScopePoll.onTick.connect([] {
        if (!link.isOpen()) return;
        wantEvent = !wantEvent;
        link.readKnockScope(wantEvent);
    });
    link.knockShotReady.connect([](const KnockShot& sh) {
        knockScope.setNotice(sh.valid ? "" : "no classifier on this ECU (no tune loaded?)");
        // The reply carries no flag saying which question it answers, so the request's own state is
        // what routes it. Single in-flight request per tick, so this cannot get out of step.
        if (wantEvent) knockScope.setEvent(sh);
        else           knockScope.setLive(sh);
    });

    onOpenKnockScope = [openToolTab] {
        openToolTab("Knock Scope", &knockScope);
        if (!link.isOpen())
            knockScope.setNotice("Knock scope needs a jayecu ECU \xE2\x80\x94 not connected");
        else
            // 10 Hz: live enough to watch a burst fired by hand, slow enough never to compete with
            // telemetry for the link.
            knockScopePoll.start(std::chrono::milliseconds(100), jf::JTimer::JMode::Repeating);
    };

    // ---- THE VE AUTOTUNER ---------------------------------------------------------------------------
    // The panel owns the arithmetic and reads the contract out of the loaded definition, so there is
    // nothing ECU-specific here beyond the two things only the app can do: hand it a telemetry frame,
    // and put a native ECU into open loop when asked.
    static AutotunePanel autotunePanel(g);

    // ONE RECORD PER TELEMETRY FRAME. The panel ignores them unless it is recording, so this costs a
    // branch while the tool is closed — and hanging it off the frame rather than a timer of its own
    // means it samples at exactly the rate the ECU is actually reporting, with no interpolation and
    // no double-counting of a frame that never arrived.
    Cache::instance().frameUpdated.connect([] { autotunePanel.onFrame(); });
    // A new definition or a re-read tune means a different grid, different cells, possibly a different
    // contract. Re-resolving is cheap and the alternative is a proposal against a map that has moved.
    Cache::instance().configLoaded.connect([] { autotunePanel.attach(); });
    // …and a plain cell edit — another page, an undo, a restore from baseline — moves the map the
    // proposal is expressed against without changing its shape, so the base is re-read rather than the
    // whole contract re-resolved.
    Cache::instance().tableEdited.connect([] { autotunePanel.onTableEdited(); });

    // OPEN LOOP FOR TUNING. The ECU's own trims are folded into the measurement, so leaving them on is
    // correct arithmetic — but it is not the honest way to measure a table, because what the loop is
    // hiding has to be inferred from a channel rather than simply not being hidden. Switching them off
    // ZEROES them (firmware Lambda.cpp: off means 1.000, not "frozen at the last value"), which is the
    // only reason one button can be trusted to do this.
    //
    // The paths are a native ECU's; on anything else they are not config and this does nothing, which
    // is why the button is only shown when the trims are actually found and actually on.
    static const char* kTrimPaths[] = { "lambda.enabled", "lambda.ltft_enabled" };
    auto trimsActive = [] {
        const Cache& c = Cache::instance();
        for (const char* p : kTrimPaths)
            if (c.isConfig(p) && c.configValue(p) != 0.0) return true;
        return false;
    };
    // WHAT THE TRIMS WERE, so the panel can put them back. Two independent switches — a tuner may
    // legitimately be running the fast loop with nothing learned, or the learned surface with the fast
    // loop stopped — so the pair is remembered as it stood rather than collapsed into "on".
    static std::vector<std::pair<std::string, double>> savedTrims;
    autotunePanel.onSetOpenLoop = [](bool open) {
        Cache& c = Cache::instance();
        c.beginEdit();
        if (open) {
            savedTrims.clear();
            for (const char* p : kTrimPaths)
                if (c.isConfig(p)) { savedTrims.emplace_back(p, c.configValue(p)); c.setConfigValue(p, 0.0); }
            c.endEdit("Open loop for autotune");
        } else {
            for (const auto& [path, v] : savedTrims)
                if (c.isConfig(path)) c.setConfigValue(path, v);
            savedTrims.clear();
            c.endEdit("Restore closed loop");
        }
    };
    // Watched rather than read once: the tuner can switch a trim back on from the O2 page while this
    // tab is open, and a panel still claiming open loop would be reporting a measurement it is not making.
    static jf::JTimer autotunePoll;
    autotunePoll.onTick.connect([&surfaceTabs, trimsActive] {
        // A CLOSED TAB MEANS STOP. The panel samples off the telemetry frame, so a run left going
        // behind a closed tab would keep accumulating a proposal nobody can see — and the Apply
        // button that eventually gets pressed would be applying a drive nobody remembers.
        if (!surfaceTabs.toolOpen(&autotunePanel)) {
            if (autotunePanel.recording()) {
                JLOGC("ui.lazy", jf::JLogLevel::Info) << "Auto Tune tab closed \xE2\x86\x92 stopping the run";
                autotunePanel.stop();
            }
            autotunePoll.stop();
            return;
        }
        autotunePanel.setTrimState(trimsActive(), !savedTrims.empty());
    });

    // CHOOSING A TEMPLATE IS A QUESTION, and it did not used to ask one: the button loaded
    // available().front(), which with three templates installed silently added a Bosch wideband
    // every time it was pressed. Twelve identical frames later that was obvious.
    GenericCanPanel::onLoadTemplate = [&win](GenericCanPanel* panel) {
        const auto& all = CanTemplates::available();
        if (all.empty()) { win.showStatus("No CAN templates installed", 3000); return; }

        // A PROTOCOL RUNS AT THE RATE IT RUNS AT. Every node on a bus has to agree on the bit rate —
        // a mismatch never ACKs, which on the bench looks exactly like nothing being plugged in — so
        // a template that names 1 Mbit cannot be loaded onto a bus set to 500 kbit, and calling it
        // "loaded" would be a lie the tuner finds out about with a scope. The row STAYS, dimmed and
        // saying what it wants, because the fix is to change the bus and hiding the row hides that.
        //
        // 11-bit against 29-bit is NOT one of these: a CAN controller carries both id widths on the
        // same wire at once and the firmware runs an accept-all filter. The IDs column says which a
        // template uses because it is worth knowing, never because it decides anything.
        static constexpr int kBusRate[4] = { 125000, 250000, 500000, 1000000 };
        const int    bus     = panel->bus();
        const Cache& cache   = Cache::instance();
        const std::string bp = "can.bus[" + std::to_string(bus) + "].";
        const int    rateIdx = int(std::lround(cache.configValue(bp + "bitrate")));
        const int    busRate = kBusRate[std::clamp(rateIdx, 0, 3)];
        const bool   busOn   = cache.configValue(bp + "enabled") != 0.0;
        const std::string busName = "CAN" + std::to_string(bus + 1);

        // AN EMPTY BUS HAS NOT COMMITTED TO ANYTHING. Its rate is whatever it was left at and nothing
        // is running on it, so there is nothing to contradict: every template is offered, and the one
        // chosen SETS the rate. Once a frame is on the bus that rate is the bus's intent — every frame
        // on it was added for that rate — and from then on only what matches is offered. The Setup
        // page locks the control at the same moment, off the same count.
        const int  framesHere  = int(std::lround(MathEvaluator::instance().evaluate(
                                     "canframes(" + std::to_string(bus) + ")")));
        const bool uncommitted = (framesHere == 0);

        auto rateText = [](int bps) {
            return (bps % 1000000 == 0) ? std::to_string(bps / 1000000) + " Mbit"
                                        : std::to_string(bps / 1000) + " kbit";
        };

        std::vector<CanTemplateDialog::Entry> entries;
        int offered = 0;
        for (const auto& t : all) {
            size_t fields = 0, ext = 0;
            for (const auto& f : t.frames) { fields += f.fields.size(); ext += f.ext ? 1 : 0; }
            const int here  = panel->templateFramesHere(t);
            const bool dupe = (here == static_cast<int>(t.frames.size())) && !t.frames.empty();
            // A template with no rate stated does not care about one — the generic shapes, which are
            // a field layout rather than somebody's product.
            const bool rateOk = uncommitted || (t.bitrate == 0) || (t.bitrate == busRate);
            const bool ok     = busOn && rateOk && !dupe;

            std::string status, why;
            if (!busOn)        { status = "bus off";   why = busName + " is switched off — turn the bus on in CAN Setup."; }
            else if (!rateOk)  { status = rateText(t.bitrate);
                                 why = t.name + " runs at " + rateText(t.bitrate) + ", and " + busName +
                                       " is committed to " + rateText(busRate) + " by the " +
                                       std::to_string(framesHere) + " frame" +
                                       (framesHere == 1 ? "" : "s") + " already on it — every node on "
                                       "a bus must agree, or nothing ever ACKs. Clear the bus to change it."; }
            else if (dupe)     { status = "loaded";    why = "Every frame of " + t.name + " is already on " + busName + "."; }
            else {
                // THE WHOLE NOTE. The footer used to be one clipped line, so only the first sentence was
                // offered; it wraps now (CanTemplateDialog) and the grid gives up the room it needs.
                why = t.note;
                if (uncommitted && t.bitrate && t.bitrate != busRate) {
                    status = rateText(t.bitrate) + " \xE2\x86\x92";       // an arrow: this MOVES the bus
                    why = "Loading this sets " + busName + " to " + rateText(t.bitrate) + ", which is "
                          "currently " + rateText(busRate) + ". Nothing is on the bus yet, so nothing "
                          "breaks.";
                }
            }

            entries.push_back({
                { t.name,
                  t.transmit ? "Transmit" : "Receive",
                  (t.bitrate == 0) ? std::string("any") : rateText(t.bitrate),
                  ext == 0 ? "11-bit" : (ext == t.frames.size() ? "29-bit" : "mixed"),
                  std::to_string(t.frames.size()),
                  std::to_string(fields),
                  status },
                ok, why });
            offered += ok ? 1 : 0;
        }

        std::vector<CanTemplateDialog::Column> cols = {
            { "Template",  300.f },
            { "Direction",  86.f },
            { "Bit rate",   80.f, jf::JDataGrid::ColAlign::Right },
            { "IDs",        66.f, jf::JDataGrid::ColAlign::Right },
            { "Frames",     62.f, jf::JDataGrid::ColAlign::Right },
            { "Signals",    66.f, jf::JDataGrid::ColAlign::Right },
            { "Status",    110.f },
        };

        win.openModal<CanTemplateDialog>(
            std::string("Load CAN Template \xE2\x80\x94 ") + busName + " " + rateText(busRate),
            std::string("Load"), std::move(cols), std::move(entries),
            std::function<void(int)>([&win, panel, bp, busRate, busName, uncommitted, rateText](int i) {
                const auto& all2 = CanTemplates::available();
                if (i < 0 || i >= static_cast<int>(all2.size())) return;
                const CanTemplates::Template& t = all2[static_cast<size_t>(i)];
                int dup = 0;
                for (const auto& f : t.frames) if (panel->hasFrame(f.id, f.ext)) dup++;

                // THE FIRST TEMPLATE ONTO AN EMPTY BUS SETS THE BUS, and says so. This is the only
                // moment it can be done safely, and it is announced rather than done quietly: the
                // rate is a setting the user may well have chosen on purpose, and a template
                // overwriting one in silence is how a bus ends up at a rate nobody remembers picking.
                std::string moved;
                if (uncommitted && t.bitrate && t.bitrate != busRate) {
                    static constexpr int kRate[4] = { 125000, 250000, 500000, 1000000 };
                    for (int k = 0; k < 4; ++k)
                        if (kRate[k] == t.bitrate) {
                            Cache::instance().setConfigValue(bp + "bitrate", double(k));
                            moved = "  \xE2\x80\x94 " + busName + " set to " + rateText(t.bitrate);
                        }
                }
                panel->loadTemplate(t);
                win.showStatus("Loaded " + t.name + (dup ? " (skipped " + std::to_string(dup) +
                                                           " already present)" : "") + moved, 5000);
            }));
        if (offered == 0)
            win.showStatus(busOn ? ("Nothing loadable on " + busName + " at " + rateText(busRate))
                                 : (busName + " is switched off"), 4000);
    };

    // …AND THE WAY BACK OUT. Loading is additive — a template's frames land beside whatever is already
    // on the bus — so the only gesture missing was taking one back off again as the set it arrived as.
    // Doing that by hand means picking Haltech V2's twenty-nine frames out of the list by eye.
    //
    // Only templates this bus is actually carrying are offered, and each entry says how many frames it
    // would take and how many CAN sensors are reading them: a removal is matched by frame ID, so it can
    // reach a frame the user built by hand that happens to share an id, and a count is what makes that
    // visible before it happens rather than after.
    GenericCanPanel::onDropTemplate = [&win](GenericCanPanel* panel) {
        const auto& all = CanTemplates::available();
        std::vector<int>                      idx;
        std::vector<CanTemplateDialog::Entry> entries;
        for (size_t i = 0; i < all.size(); ++i) {
            const int here = panel->templateFramesHere(all[i]);
            if (!here) continue;                       // not on this bus: nothing to take off it
            const int reading = panel->sensorsReading(all[i]);
            idx.push_back(static_cast<int>(i));
            // A SENSOR LEFT POINTING AT A FRAME THAT IS GONE is not silently cleared — it raises its
            // config DTC and reads "missing" in its own CAN Field picker. The count is here so that
            // is visible BEFORE the removal rather than discovered afterwards from a code.
            std::string why = std::to_string(here) + " of " + all[i].name + "'s " +
                              std::to_string(all[i].frames.size()) + " frames are on this bus.";
            if (reading)
                why += "  " + std::to_string(reading) + " sensor" + (reading == 1 ? "" : "s") +
                       " read" + (reading == 1 ? "s" : "") +
                       " them and would be left naming a frame that is gone.";
            entries.push_back({ { all[i].name,
                                  all[i].transmit ? "Transmit" : "Receive",
                                  std::to_string(here) + " of " + std::to_string(all[i].frames.size()),
                                  std::to_string(reading) },
                                true, why });
        }
        if (entries.empty()) { win.showStatus("No template's frames are on this bus", 3000); return; }

        std::vector<CanTemplateDialog::Column> cols = {
            { "Template",       320.f },
            { "Direction",       86.f },
            { "Frames on bus",  106.f, jf::JDataGrid::ColAlign::Right },
            { "Sensors reading", 116.f, jf::JDataGrid::ColAlign::Right },
        };

        win.openModal<CanTemplateDialog>(
            std::string("Remove CAN Template"), std::string("Remove"),
            std::move(cols), std::move(entries),
            std::function<void(int)>([&win, panel, idx](int i) {
                const auto& all2 = CanTemplates::available();
                if (i < 0 || i >= static_cast<int>(idx.size())) return;
                const size_t t = static_cast<size_t>(idx[static_cast<size_t>(i)]);
                if (t >= all2.size()) return;
                const int reading = panel->sensorsReading(all2[t]);
                const int n = panel->dropTemplate(all2[t]);
                win.showStatus("Removed " + std::to_string(n) + " frame" + (n == 1 ? "" : "s") +
                               " of " + all2[t].name +
                               (reading ? "  \xE2\x80\x94 " + std::to_string(reading) + " sensor" +
                                          (reading == 1 ? "" : "s") + " now name" +
                                          (reading == 1 ? "s" : "") + " a frame that is gone" : ""),
                               5000);
            }));
    };

    onOpenAutotune = [openToolTab, trimsActive] {
        JLOGC("ui.lazy", jf::JLogLevel::Info) << "Tools\xE2\x96\xB8" "Auto Tune \xE2\x86\x92 opening centre tool tab";
        openToolTab("Auto Tune", &autotunePanel);
        autotunePanel.attach();
        autotunePanel.setTrimState(trimsActive(), !savedTrims.empty());
        autotunePanel.setLinkNotice((link.isOpen() || tsLink.isOpen())
                                        ? std::string()
                                        : "Not connected \xE2\x80\x94 nothing to measure");
        autotunePoll.start(std::chrono::milliseconds(500), jf::JTimer::JMode::Repeating);
    };

    // A CLOSED TAB MEANS THE USER IS FINISHED WITH THAT TOOL, and both of these keep the ECU working
    // after the window has gone. The engine-cycle recorder re-requests a capture on every arrival —
    // and, since it stopped giving up on a stalled engine, re-arms on a timer as well — so a closed
    // tab left it asking the ECU for cycles for the rest of the session with nothing on screen to
    // show for it. The trigger log streams: a full buffer ends a run by itself, but a run ended any
    // other way still holds the decoder until something says stop.
    //
    // Polled rather than hung off onTabClosed, which reports an INDEX after the removal and so cannot
    // say WHICH tool closed. Four times a second, two pointer compares.
    static jf::JTimer toolTabPoll;
    toolTabPoll.onTick.connect([&surfaceTabs]{
        if (!surfaceTabs.toolOpen(&engineCyclePanel) && engineCyclePanel.recording()) {
            JLOGC("ui.lazy", jf::JLogLevel::Info) << "Engine Cycle tab closed \xE2\x86\x92 stopping the recorder";
            engineCyclePanel.pause();
        }
        if (!surfaceTabs.toolOpen(&triggerLogPanel) && link.triggerLogRunning()) {
            JLOGC("ui.lazy", jf::JLogLevel::Info) << "Trigger Log tab closed \xE2\x86\x92 stopping the ECU log";
            link.stopTriggerLog();
        }
    });
    toolTabPoll.start(std::chrono::milliseconds(250), jf::JTimer::JMode::Repeating);

    onOpenTriggerLog = [openToolTab] {
        JLOGC("ui.lazy", jf::JLogLevel::Info) << "View\xE2\x96\xB8" "Trigger Log \xE2\x86\x92 opening centre tool tab";
        openToolTab("Trigger Log", &triggerLogPanel);
        if (!link.isOpen())
            triggerLogPanel.setStatus("Trigger log needs a jayecu ECU \xE2\x80\x94 not connected");
    };

    // KEEP TRYING UNTIL THE USER SAYS STOP. A capture cannot complete while the engine is not turning,
    // and that is the normal state of a bench between runs — not a failure to report and give up on.
    // Stopping on it meant arriving at a rig, pressing Start, being told "is the engine turning?", and
    // having to press Start again once it was. Recording is a mode the USER ends.
    //
    // So while recording, a failed capture re-arms on a timer instead of pausing: paced, because the
    // reasons that keep failing (engine stopped, link down) would otherwise spin the link as fast as it
    // can refuse; the delay stops that without giving up.
    static jf::JTimer cycleRetry;
    cycleRetryArm = []{ cycleRetry.start(std::chrono::milliseconds(750), jf::JTimer::JMode::SingleShot); };
    cycleRetry.onTick.connect([]{
        cycleRetry.stop();
        if (engineCyclePanel.recording() && engineCyclePanel.onCaptureRequest)
            engineCyclePanel.onCaptureRequest();
    });
    link.engineCycleFailed.connect([](const std::string& why) {
        JLOGC("comms.cycle", jf::JLogLevel::Warn) << "capture failed: " << why;
        if (!engineCyclePanel.recording()) { engineCyclePanel.setStatus(why); return; }
        // Say what it is WAITING for, not that something went wrong — the frames already on screen stay
        // there, and the next real cycle appends to them.
        engineCyclePanel.setStatus("Waiting for a cycle \xE2\x80\xA6 " + why);
        cycleRetryArm();
    });
    // A closed tab's surface survives in the pool — list the closed ones and reopen the picked one.
    //
    // THE ROWS CARRY THEIR POOL INDEX. Names are not unique (New Surface Tab made them all "Surface"),
    // so the row is labelled with its index and reopened by it. By name, the first match won every time:
    // both rows opened the same surface, and if that one was already open the loop just focused its tab
    // and returned — so the second was unreachable for as long as it existed.
    onReopenSurface = [&win, &surfaceTabs] {
        std::vector<std::string> labels;
        std::vector<int> ids;
        for (const auto& sr : surfaceTabs.surfaces())
            if (!sr.open) { labels.push_back(sr.name + "   #" + std::to_string(sr.index)); ids.push_back(sr.index); }
        if (labels.empty()) { win.showStatus("No closed surfaces to reopen", 2500); return; }
        win.openModal<ListPickerDialog>(std::string("Reopen a surface"), labels, false,
            std::function<void(int, std::string)>([&surfaceTabs, ids](int i, std::string) {
                if (i >= 0 && i < static_cast<int>(ids.size())) surfaceTabs.reopenAt(ids[i]);
            }));
    };

    // RENAMING IS NOT HERE. A tab is the name of the thing it holds, and it is on screen — so it is
    // renamed on screen: double-click it (or F2), which JTabWidget now does in place with a real text
    // editor, the same gesture that renames a tree row. It was briefly a fourth menu item listing every
    // surface so you could pick the one you were already looking at, which is a dialog standing in for
    // a double-click.
    onDeleteSurface = [&win, &surfaceTabs] {
        std::vector<std::string> labels; std::vector<int> ids;
        for (const auto& sr : surfaceTabs.surfaces()) {
            labels.push_back(sr.name + (sr.open ? "   (open)" : "") + "   #" + std::to_string(sr.index));
            ids.push_back(sr.index);
        }
        if (labels.empty()) { win.showStatus("No surfaces", 2500); return; }
        win.openModal<ListPickerDialog>(std::string("Delete a surface \xE2\x80\x94 its contents go with it"),
            labels, false,
            std::function<void(int, std::string)>([&win, &surfaceTabs, ids](int i, std::string) {
                if (i < 0 || i >= static_cast<int>(ids.size())) return;
                win.showStatus(surfaceTabs.removeAt(ids[i]) ? "Surface deleted"
                                                           : "That is the last surface \xE2\x80\x94 not deleted", 4000);
            }),
            std::function<void()>{}, std::string("Delete"));
    };

    // Widgets dock = the control palette (click a type to add it to the active surface).
    static ControlPalette palette(g);
    palette.onAdd = [&surfaceTabs](const std::string& type) { if (auto* s = surfaceTabs.activeSurface()) s->addControl(type); };
    widgets.setContent(&palette);

    // The Properties dock inspects the selected element (edit mode only). It fills the right-area
    // "Properties" dock declared above.
    static PropertiesDock propsDock(g);
    properties.setContent(&propsDock);
    // Selecting a dictionary LEAF points this dock at that binding (a read-only meta view: kind, datatype,
    // scale, schema min/max, geometry). Branches carry no userData, so they clear it. Selecting a canvas
    // widget replaces the view via showFor() as usual. Wired here — propsDock doesn't exist any earlier.
    // Adding, renaming and removing PcVariables happens HERE, in the dictionary, because they are
    // dictionary entries: host-side paths the project owns. Everything else in this tree is generated and
    // read-only, so only the pc.* rows and the ghost row respond.
    //
    // Persistence goes with the ECU project, not the dashboard: the declaration is part of what the paths
    // ARE (meta), while the dashboard is only controls. saveVars writes the side-car; the generated meta
    // file is never touched, because it is regenerated from the schema.
    static auto pcVarsPath = []() -> std::string {
        return ecu ? (std::filesystem::path(ecu->dir()) / "pcvars.json").string() : std::string();
    };
    static auto savePcVars = [] {
        const std::string path = pcVarsPath();
        if (path.empty()) return;
        // THE PROJECT'S OWN, not the live set: the definition's variables arrive with the meta every
        // time, and writing them here would freeze a copy that a newer firmware could not update.
        jf::JJson arr = jf::JJson::array();
        for (const auto& v : meta.projectPcVars()) {
            jf::JJson o = jf::JJson::object();
            o["name"] = v.name; o["datatype"] = v.datatype; o["scale"] = v.scale;
            o["min"] = v.minV; o["max"] = v.maxV; o["units"] = v.units;
            o["label"] = v.label; o["kind"] = v.kind;
            if (v.hasDef) o["default"] = v.defV;     // dropped before, so a declared default lasted one save
            if (!v.options.empty()) {
                jf::JJson opts = jf::JJson::array(); for (const auto& x : v.options) opts.push(x);
                o["options"] = opts;
            }
            arr.push(o);
        }
        jf::JJson doc = jf::JJson::object(); doc["pcVars"] = arr;
        doc.dumpToFile(path, 2);
    };
    // A FULL rebuild — for a new schema or an import, where the whole dictionary changes. NEVER for an
    // in-place edit: those edit the live tree below.
    static auto rebuildDict = [&dictTree] { if (meta.isValid()) dictTree.setRootNode(buildDictionary(meta)); };

    dictTree.onSelectionChanged.connect([](jf::JTreeViewNode* n) {
        // Selecting the ghost does nothing. A DOUBLE-CLICK (or F2) starts the rename — JTreeView already
        // does that for any editable tree — and the variable exists only once a name is committed.
        if (n && n->placeholder) return;
        propsDock.showForBinding(n ? n->userData : std::string());
    });

    // Promoting a ghost and renaming a variable are the same edit, so one handler does both.
    //
    // It edits the LIVE tree — copy it, change the one node, set it back — which is exactly what the
    // navigation tree does, and why that one keeps its place. Rebuilding from the model instead produces a
    // tree with no expansion state, so committing a rename collapsed every category and threw the reader
    // back to the top: an edit to one row must not move the other three hundred.
    dictTree.onNodeRenamed.connect([&dictTree](jf::JTreeViewNode* n) {
        if (!n) return;
        const bool isGhost = n->placeholder;
        const bool isPcRow = n->userData.rfind("pc.", 0) == 0;
        std::vector<int> path;
        if (!findIndexPath(dictTree.root(), n, path) || path.empty()) return;

        JTreeViewNode copy = dictTree.root();          // carries expansion + scroll with it
        JTreeViewNode* pn = nodeAtPath(copy, path);
        if (!pn) return;
        const std::string ph = pcPlaceholderCaption();
        auto putBack = [&](const std::string& label) { pn->label = label; dictTree.setRootNode(std::move(copy)); };
        if (!isGhost && !isPcRow) { putBack(pn->label); return; }        // generated rows are read-only

        std::string clean;
        for (char c : n->label) if (std::isalnum((unsigned char)c) || c == '_') clean += c;
        auto vars = meta.pcVars();
        const std::string oldName = isPcRow ? n->userData.substr(3) : std::string();
        // The definition's own variables are read-only: they come back with every meta load, so a
        // rename here would only leave a second, orphaned copy in the side-car.
        if (isPcRow && meta.isDefinitionPcVar(oldName)) { putBack(oldName); return; }
        bool dup = false;
        for (const auto& v : vars) if (v.name == clean && v.name != oldName) dup = true;
        if (clean.empty() || dup) { putBack(isGhost ? ph : oldName); return; }   // unusable -> nothing changes

        if (isGhost) {
            MetaModel::PcVar nv; nv.name = clean; nv.datatype = "F32"; nv.scale = 1.0;
            vars.push_back(nv);
            pn->placeholder = false;                    // ghost -> real row
            pn->label = clean;
            pn->userData = "pc." + clean;
            // A fresh ghost trails it, so the add affordance stays the last child of the category.
            std::vector<int> pp(path.begin(), path.end() - 1);
            if (JTreeViewNode* parent = pp.empty() ? &copy : nodeAtPath(copy, pp)) {
                JTreeViewNode g{ph, false, false, {}};
                g.placeholder = true;
                parent->children.push_back(std::move(g));
            }
        } else {
            for (auto& v : vars) if (v.name == oldName) v.name = clean;
            pn->label = clean;
            pn->userData = "pc." + clean;
        }
        meta.applyPcVars(vars);
        Cache::instance().initSegments();   // the host block changed size — reallocate before anything reads it
        savePcVars();
        dictTree.setRootNode(std::move(copy));          // the EDITED live tree, not a rebuilt one
    });

    // Remove: Delete on a pc.* row drops the declaration (and with it the path) — again by editing the
    // live tree, so the rest of the dictionary stays where the reader left it.
    dictTree.onDeleteKey.connect([&dictTree] {
        const jf::JTreeViewNode* sel = dictTree.selectedNode();
        if (!sel || sel->userData.rfind("pc.", 0) != 0) return;
        const std::string name = sel->userData.substr(3);
        if (meta.isDefinitionPcVar(name)) return;         // the definition's own: not the project's to remove
        std::vector<int> path;
        if (!findIndexPath(dictTree.root(), sel, path) || path.empty()) return;
        JTreeViewNode copy = dictTree.root();
        std::vector<int> pp(path.begin(), path.end() - 1);
        JTreeViewNode* parent = pp.empty() ? &copy : nodeAtPath(copy, pp);
        if (!parent || path.back() >= int(parent->children.size())) return;
        parent->children.erase(parent->children.begin() + path.back());

        auto vars = meta.pcVars();
        vars.erase(std::remove_if(vars.begin(), vars.end(),
                                  [&](const MetaModel::PcVar& v) { return v.name == name; }), vars.end());
        meta.applyPcVars(vars);
        Cache::instance().initSegments();
        savePcVars();
        dictTree.setRootNode(std::move(copy));
    });
    // Data source = an expression that resolves to a value — the SAME recipe as a visibility test — so it
    // opens the ONE expression editor, with the full #/$/@ sigil navigator, not a separate channel picker.
    propsDock.onPickSource = [&win, &surfaceTabs](const std::string& cur, std::function<void(std::string)> onPicked) {
        // The live test evaluates inside the page's element, so a "[*]" built here previews against the
        // element the page is about rather than resolving to nothing.
        Surface* s = surfaceTabs.activeSurface();
        win.openModal<ExpressionEditor>(cur,
            buildSigilTree(meta, g_widgetSigils.lister ? g_widgetSigils.lister() : std::vector<std::string>{}),
            std::move(onPicked), ExpressionEditor::FirmwareTarget{},
            s ? s->elementScope() : std::string());
    };
    // A "node" editor (e.g. a hyperlink's Link) opens the navigation tree in the shared popup picker: single
    // click on any node returns its "/"-joined path. Rebuilt from the LIVE tree each open, so renames/adds show.
    propsDock.onPickNode = [&win, &nodeTree](const std::string& cur, std::function<void(std::string)> onPicked) {
        win.openModal<PopupSignalPicker>(buildNodePickTree(nodeTree.root(), ""), cur, std::move(onPicked), "Select Node");
    };
    // Host hooks: the inspector is surface-agnostic, so the app binds repaint + undo to the ACTIVE surface
    // (wired once — the lambdas query the live active surface, so a tab switch needs no rewiring).
    // The surface being AUTHORED: the page open in the MDI when there is one, else the active tab.
    auto editSurf = [&surfaceTabs]() -> Surface* {
        return pageSurface ? pageSurface.get() : surfaceTabs.activeSurface();
    };
    propsDock.onInvalidate      = [editSurf]{ if (Surface* s = editSurf()) s->invalidate(); };
    propsDock.onSnapshot        = [editSurf]() -> std::vector<PanelElement> { Surface* s = editSurf(); return s ? s->elementsSnapshot() : std::vector<PanelElement>{}; };
    propsDock.onApplyEdit       = [editSurf](const std::string& l, const std::vector<PanelElement>& b, int m){ if (Surface* s = editSurf()) s->applyExternalEdit(l, b, m); };
    propsDock.onCanvasSnapshot  = [editSurf]() -> PanelModel::CanvasState { Surface* s = editSurf(); return s ? s->canvasSnapshot() : PanelModel::CanvasState{}; };
    propsDock.onApplyCanvasEdit = [editSurf](const std::string& l, const PanelModel::CanvasState& b, int m){ if (Surface* s = editSurf()) s->applyCanvasEdit(l, b, m); };
    propsDock.onResolveInstance = [editSurf](int id) -> CanvasWidget* { Surface* s = editSurf(); return s ? s->widgetById(id) : nullptr; };   // author prop edits onto the live widget
    // Point the inspector at the active surface's model + selection (Edit mode only; else it clears).
    auto showProps = [](Surface* s){
        const bool editing = s && s->mode() == Surface::Mode::Edit;
        propsDock.setElementScope(editing ? s->elementScope() : std::string());   // "[*]"/"$*" -> this element, for the unit picker
        propsDock.showFor(editing ? s->model() : nullptr, editing ? s->selection() : std::vector<int>{}, editing);
    };
    surfaceTabs.selectionChanged.connect([showProps](Surface* s){ showProps(s); });
    // …and the same for the page in the MDI, which is where authoring actually happens now.
    g_syncProps = [showProps]{ if (pageSurface) showProps(pageSurface.get()); };
    // A TAB CARRIES ITS OWN PAGE, so arriving on one moves the tree to match. Selecting in the tree is
    // the other direction and already works; this closes the loop. It cannot echo — the tab emits only
    // when its node differs from the one the tree is already on.
    surfaceTabs.nodeFollowed.connect([&](const std::string& path) {
        treeSelPath = path;
        nodeTree.selectByPath(path);
        Cache::instance().setEditScope(path);
    });
    // A bulk table op that clamped values says so — otherwise a paste silently differs from what was pasted.
    surfaceTabs.statusMessage.connect([&win](const std::string& m){ win.showStatus(m, 4000); });
    // A viewport's "Open in Separate Tab" → open/focus a tab rooted at that node's page (the only per-node
    // tab, user-initiated — ports PanelView::viewportActivated → openDefinitionSurface).
    Surface::onOpenNodeTab = [&surfaceTabs](std::string nodePath){
        const size_t s = nodePath.rfind('/');
        surfaceTabs.openNode(nodePath, s == std::string::npos ? nodePath : nodePath.substr(s + 1));
    };
    // Tree selection drives the surface view-switcher (ports MainWindow::onNodeSelected): the selected node's
    // path scopes every surface so only that node's viewports show; clearing the selection hides them all.
    nodeTree.onSelectionChanged.connect([&surfaceTabs, &nodeTree, &treeSelPath, openPage](JTreeViewNode* n){
        // Choosing a node ENDS any drag in flight. A dictionary drag lives until something accepts it —
        // deliberately, so a drag begun in a floating dock can still be dropped on the next click — but a
        // drag released over the tree or the chrome is never accepted by anything, so its label stayed
        // stuck to the cursor: "[Calibration]" following you around a node that has no viewport at all.
        if (jf::JDragDrop::isDragging()) jf::JDragDrop::cancel();
        std::string path; if (n) nodePathOf(nodeTree.root(), n, "", path);
        treeSelPath = path;                 // remember the OLD path so a following rename knows what to re-key
        // THE PAGE IS THE WINDOW'S JOB NOW. The surfaces behind the windows carry the live instrumentation —
        // the readout strip, the channels, the graph — and their node viewports used to BE the page view.
        // Leaving them following the tree drew the selected page twice: once on the background and again
        // in the window over it, the same controls in two places, either of which could be clicked. The
        // background keeps no page; openPage opens the one page there is.
        surfaceTabs.setActiveNode({});
        openPage(path);
        // The page being shown is the scope every edit made from here is filed under, so that page's own
        // < > arrows walk its own changes and nobody else's (Cache::setEditScope).
        Cache::instance().setEditScope(path);
    });
    // …AND A CLICK ON THE NODE THAT IS ALREADY SELECTED STILL OPENS ITS PAGE. Selection only changes once,
    // so once a page's window had been closed the tree had nothing left to say about it: clicking the very
    // node the reader was looking at did nothing at all, and the only way back to the page was to select
    // something else and come back. onNodeActivated fires on every click, changed or not; openPage ignores
    // the ones for a page already open, so this costs nothing on an ordinary walk through the tree.
    nodeTree.onNodeActivated.connect([&nodeTree, openPage](JTreeViewNode* n){
        std::string path; if (n) nodePathOf(nodeTree.root(), n, "", path);
        openPage(path);
    });
    // A nav-tree node reads as a hyperlink (underlines on hover) when it owns a non-empty page — i.e. clicking
    // it actually shows a viewport/dashboard, not just an empty category. The library holds each node's page.
    nodeTree.onIsHyperlink = [&nodeTree](const JTreeViewNode* n) -> bool {
        if (!n) return false;
        std::string path; if (!nodePathOf(nodeTree.root(), n, "", path) || path.empty()) return false;
        const PanelModel* pm = panelLibrary.find(path);
        return pm && !pm->elements().empty();
    };
    // Hyperlink widgets navigate through this hook: select the linked node in the tree, exactly as if the
    // user had clicked it — selectByPath emits onSelectionChanged, which switches the surface view AND moves
    // the tree highlight. Falls back to a direct view switch if the path isn't in the tree.
    // A LINK ANSWERS TO THE SAME CONDITION AS THE MENU ENTRY. applyNodeConditions() hides a nav node whose
    // visibility expression is false, and a hidden node hides its whole subtree -- so walking the label path
    // and refusing at the first hidden step is exactly the tree's own answer, ancestors included. An unknown
    // path is unreachable too, which covers a link whose target was renamed or deleted underneath it.
    hyperlink::reachable() = [&nodeTree](const std::string& path) -> bool {
        const JTreeViewNode* n = &nodeTree.root();
        for (const std::string& seg : splitOn(path, '/')) {
            if (seg.empty()) continue;
            const JTreeViewNode* next = nullptr;
            for (const auto& c : n->children) if (c.label == seg) { next = &c; break; }
            if (!next || next->hidden) return false;      // gone, or condition-hidden (subtree included)
            n = next;
        }
        return n != &nodeTree.root();                     // the root itself is not a destination
    };
    hyperlink::navigator() = [&surfaceTabs, &nodeTree](const std::string& path){
        if (path.empty()) return;
        nodeTree.selectByPath(path);        // move the tree highlight (also fires onSelectionChanged → the window)
        surfaceTabs.setActiveNode({});      // the background shows instruments, not the page (see above)
        Cache::instance().setEditScope(path);
    };
    // Persist the whole navigation tree + refresh the path index (call after any structural change).
    auto persistTree = [&]{ g_docDirty = true;
                            treePaths.clear(); collectPaths(nodeTree.root(), "", treePaths); };

    // ── The project document ── one file, one shape: dashboard.gui = { tree, surface, panelLibrary }.
    // buildDoc/loadDoc are the single serialise/deserialise
    // pair; Save (dashboard.gui), Save-Layout-As (any path), Open-Layout (any path) and ECU-open all go through
    // them, so the three menu actions can never drift into saving different subsets.
    auto buildDoc = [&]() -> jf::JJson {
        surfaceTabs.commitAll();   // flush widget-owned state into the models FIRST — serialise only at this boundary
        // The TREE is what makes a page reachable — see pruneUnplacedPages. Passing the live path set
        // here is what stops a save deleting pages that are perfectly reachable but not named by any
        // viewport.
        surfaceTabs.pruneUnplacedPages(treePaths);
        jf::JJson doc = jf::JJson::object();
        jf::JJson tree = jf::JJson::array();
        for (const auto& c : nodeTree.root().children) if (!c.placeholder) tree.push(dashNodeToJson(c));
        doc["tree"] = std::move(tree);
        doc["surfaces"] = surfaceTabs.surfacesToJson();   // the WHOLE pool + open tabs, not just the active one
        doc["panelLibrary"] = panelLibrary.toJson();
        // The per-CHANNEL layer: unit, scale and the bands that make a reading amber. It belongs to the
        // document rather than to any control, because that is exactly what makes one threshold enough.
        doc["channelPrefs"] = ChannelPrefs::instance().toJson();
        // WHAT THIS LAYOUT WAS DRAWN AGAINST. Every widget binds by PATH, which is the right choice —
        // a path survives offsets moving — but paths are not eternal: rename or retire one and the
        // widget binds to nothing and draws a blank, with nothing on screen to say why. Recording the
        // schema identity lets the load side notice it is looking at a layout from a different build
        // and name the bindings that no longer resolve, instead of leaving the user to wonder which of
        // their gauges went quiet.
        if (const MetaModel* mm = Cache::instance().meta()) {
            jf::JJson built = jf::JJson::object();
            built["layout_hash"]    = jf::JJson(mm->layoutHash());
            built["telemetry_size"] = jf::JJson(mm->telemetrySize());
            // And WHICH STUDIO saved it: a dashboard can use widgets and properties an older studio does
            // not know, and that studio should say so rather than quietly drawing less than was made.
            built["studio"]         = jf::JJson(std::string(STUDIO_VERSION));
            doc["builtFor"] = built;
        }
        return doc;
    };
    auto loadDoc = [&](const jf::JJson& doc) {
        JLOGC("ui.lazy", jf::JLogLevel::Info) << "loading project document: tree="
            << (doc.contains("tree") ? doc["tree"].arr().size() : 0) << " node(s), surfaces="
            << (doc.contains("surfaces") ? doc["surfaces"].arr().size() : 0);
        JTreeViewNode root{"Nodes", true, false, {}};
        if (doc.contains("tree")) for (const auto& n : doc["tree"].arr()) {
            if (isSeparator(n)) continue;                    // see NavTree.cpp — rules are not tree rows
            root.children.push_back(dashNode(n));
        }
        setTree(std::move(root));
        panelLibrary = PanelLibrary{};                                   // replace, don't merge (a fresh document)
        if (doc.contains("panelLibrary")) panelLibrary.load(doc["panelLibrary"]);
        // The lamps' page, if this definition has any. A native document has no [FrontPage] strip, so the
        // dock simply shows nothing rather than an empty frame pretending to be a panel.
        statusDock.setContent(nullptr);
        statusSurface.reset();
        if (PanelModel* lamps = panelLibrary.find(tsconvert::kStatusPage)) {
            statusSurface = std::make_unique<Surface>(g, Cache::instance(), lamps);
            // RUN, EXPLICITLY. A Surface defaults to Edit, and edit pins a page to its authored canvas
            // and draws the card round it — so these lamps rendered as a 1232x162 box floating in the
            // dock instead of filling it. Nobody edits the status strip; it is read.
            statusSurface->setMode(Surface::Mode::Run);
            statusDock.setContent(statusSurface.get());
            // THE DOCK OPENS THE SIZE OF WHAT IS IN IT. The importer knows how tall the lamp grid is —
            // it laid it out — and writes that as the page's canvas height, so the dock has no need to
            // guess. Left at the area's 200 px default it opened with a third of itself empty.
            //
            // WITHOUT OVERRIDING A DRAG. We remember the height we last suggested: if the saved height is
            // still that, nobody has touched it and a new document may suggest its own; once the user has
            // dragged the divider the saved height differs and is left alone for good. Re-importing a
            // definition with more lamps therefore re-fits, and resizing it by hand sticks.
            const int want = int(lamps->effCanvasH()) + kStatusDockChrome;
            const int saved = settings.get<int>("dock.bottomH", 0);
            const int prev  = settings.get<int>("dock.statusSuggested", 0);
            if (saved == 0 || saved == prev) {
                space.setBottomHeight(float(want));
                settings.set("dock.bottomH", jf::JVariant(want));
                settings.set("dock.statusSuggested", jf::JVariant(want));
            }
        }
        ChannelPrefs::instance().load(doc.contains("channelPrefs") ? doc["channelPrefs"] : jf::JJson());
        g_maintained.clear();
        for (const auto& m : doc["maintained"].arr())
            if (!m["target"].str().empty() && !m["expr"].str().empty())
                g_maintained.push_back({ m["target"].str(), m["expr"].str() });
        if (!g_maintained.empty())
            JLOGC("ui.lazy", jf::JLogLevel::Info) << "maintained constants: " << g_maintained.size();
        // Surfaces AFTER the library (node tabs resolve their pages there). A legacy single-"surface"
        // document loads into the fresh Main that the reset path creates.
        // DID THIS LAYOUT COME FROM A DIFFERENT BUILD? Widgets bind by path, so a schema change does not
        // corrupt anything — it just leaves some of them pointing at nothing, drawing a blank with no
        // explanation. If the layout was drawn against another schema, walk every binding and NAME the
        // ones that no longer resolve, so a gauge that went quiet is a message rather than a mystery.
        // MADE WITH A NEWER STUDIO? Not refused — a dashboard is drawn as far as this studio understands
        // it — but said, so a missing gauge is explained rather than a mystery.
        if (const std::string by = doc["builtFor"]["studio"].str();
            !by.empty() && jf::JVersion::parse(by).isNewerThan(jf::JVersion::parse(STUDIO_VERSION)))
            win.showStatus("This dashboard was made with jayecu Studio " + by + " \xE2\x80\x94 this is " STUDIO_VERSION
                           ", so some parts of it may not show. Update the studio to see all of it.", 15000);
        g_docBuiltForLayout = doc.contains("builtFor") ? doc["builtFor"]["layout_hash"].str() : std::string();
        if (g_checkLayoutBindings) g_checkLayoutBindings();
        surfaceTabs.surfacesFromJson(doc.contains("surfaces") ? doc["surfaces"] : jf::JJson());
        if (!doc.contains("surfaces") && doc.contains("surface"))
            if (auto* m = activeModel()) m->load(doc["surface"]);
        treePaths.clear(); collectPaths(nodeTree.root(), "", treePaths);
        g_treeUndo.clear();                                              // fresh document → fresh tree history
        if (auto* s = activeSurf()) s->invalidate();
    };

    // File▸Save (Ctrl+S) + the save-on-close path: write the whole document to the ECU's dashboard.gui and
    // clear the dirty flag. This is the ONLY writer of the canonical project file.
    auto saveAll = [&]{
        if (!ecu) return;                                // no open project → nothing to persist
        buildDoc().dumpToFile(ecu->dashboardPath(), 2);
        saveActiveTune();                                // the calibration persists alongside the layout (original parity)
        g_docDirty = false;
    };
    saveAllFn = saveAll;
    saveLayoutFn = [&, buildDoc]{
        if (!ecu) { win.showStatus("No ECU is open", 2000); return; }
        buildDoc().dumpToFile(ecu->dashboardPath(), 2);
        g_docDirty = false;
        win.showStatus("Saved layout", 1500);
    };
    // Save Layout As… — export the full document to a chosen path (a copy; the canonical file stays the ECU's).
    saveLayoutAsFn = [&, buildDoc](const std::string& path) {
        buildDoc().dumpToFile(path, 2);
        win.showStatus("Saved " + path, 2500);
    };
    // Open Layout… — import a full document into the current project. It replaces the working tree/surface/pages
    // and marks the doc dirty (unsaved to dashboard.gui until File▸Save).
    openLayoutFn = [&, loadDoc](const std::string& path) {
        auto j = jf::JJson::tryParseFile(path);
        if (!j || !j->isObject()) { win.showStatus("Cannot open " + path, 3000); return; }
        loadDoc(*j);
        g_docDirty = true;
        win.showStatus("Opened " + path, 2500);
    };
    // FILE▸CLOSE ECU CLOSES IT. It used to put the landing back and nothing else: the ECU, its tune, the
    // definition, the navigation tree, the pages and any tool tab all stayed loaded behind it, so the
    // tree went on showing an ECU that was supposedly closed. Now it empties everything a project brought
    // in, after the usual guard (unsaved layout edits prompt Save / Discard / Cancel; the tune saves itself).
    // It is only offered with no link up (MenuGate), so there is no connection to take down here.
    closeProjectFn = [&, loadDoc]{ maybeSaveThen([&, loadDoc]{
        // The document: tree, pages, surfaces, status lamps, channel prefs, tree history. Loading an empty
        // one also removes every tab, tool tabs included -- and toolTabPoll then stops the engine-cycle
        // recorder and the trigger log that those tabs were driving.
        loadDoc(jf::JJson());
        knockScopePoll.stop();
        autotunePoll.stop();
        // The ECU and its tune.
        ecu = nullptr;
        activeTuneName.clear();
        Cache::instance().setConfigImage({});
        Cache::instance().setBaseline({});
        g_docDirty = false;
        // The definition, and everything that was built from it.
        meta = MetaModel{};
        Cache::instance().setMeta(&meta);
        s_tsProto = jf::JJson();
        triggerLib.setShippedWheels({});
        dictTree.setRootNode(buildDictionary(meta));
        win.setCentralWidget(&landingView);
        win.showStatus("Closed", 1500);
    }); };
    // Loading the project document that travels with the open ECU's folder (openTune calls this through
    // g_loadProjectDoc — the lambda is declared up with the link because openTune is defined before loadDoc).
    g_checkLayoutBindings = [&]{
        const MetaModel* mm = Cache::instance().meta();
        const std::string& was = g_docBuiltForLayout;
        // No schema yet (the document loads before it on connect): nothing to check against.
        if (mm && !mm->layoutHash().empty()) {
            if (!was.empty() && was != mm->layoutHash()) {
                std::vector<std::string> dead;
                for (const auto& [node, page] : panelLibrary.pages()) {
                    if (!page) continue;
                    for (const PanelElement& el : page->elements()) {
                        const std::string bind = el.prop("signalName");
                        if (bind.empty() || bind.front() == '[') continue;   // unbound, or an expression
                        if (mm->locate(bind).valid()) continue;                    // config scalar/run/table
                        if (mm->telemetry().count(bind)) continue;                 // a live channel
                        if (mm->signalMap().count(bind)) continue;                 // a bus signal by name
                        dead.push_back(node + " : " + el.type + " -> " + bind);
                    }
                }
                JLOGC("ui.lazy", jf::JLogLevel::Warn)
                    << "layout was drawn against schema " << was << ", this ECU is " << mm->layoutHash()
                    << " — " << dead.size() << " binding(s) no longer resolve";
                for (const std::string& d : dead)
                    JLOGC("ui.lazy", jf::JLogLevel::Warn) << "    unresolved: " << d;
                if (!dead.empty())
                    win.showStatus(std::to_string(dead.size()) + " widget binding(s) from an older schema ("
                                   + was + ") no longer resolve — see the log", 15000);
            }
        }
    };
    g_seedNavFromMeta = [&]{
        if (!ecu || std::filesystem::exists(ecu->dashboardPath())) return;   // a saved document is the authority
        if (!nodeTree.root().children.empty()) return;                       // already has a tree: leave it alone
        const jf::JJson& nav = meta.navigationTree();
        if (nav.arr().empty()) return;                                       // meta not settled yet, or has none
        JTreeViewNode root{"Nodes", true, false, {}};
        for (const auto& n : nav.arr()) root.children.push_back(seedNode(n));
        JLOGC("ui.lazy", jf::JLogLevel::Info)
            << "no project document; seeded " << root.children.size() << " navigation root(s) from the meta";
        setTree(std::move(root));
        treePaths.clear(); collectPaths(nodeTree.root(), "", treePaths);
        // A seeded tree exists only in memory, so the document IS ahead of the disk — say so, and let the
        // usual unsaved-changes gate offer to keep it on the way out. Connecting still writes nothing by
        // itself; whether this ECU gets a project file stays the user's decision, they are just asked
        // instead of silently re-seeding every session.
        g_docDirty = true;
    };

    // Help▸"Add Missing Navigation Entries…". A SAVED tree is the user's and stops tracking the meta by
    // design; this is how a node the definition has since gained (a new sensor, a new module) reaches an
    // existing project. The left pane is the meta tree PRUNED to what the current tree lacks, so it shows
    // what is genuinely on offer rather than the whole definition again.
    reseedFn = [&]{
        const jf::JJson& nav = meta.navigationTree();
        if (nav.arr().empty()) { win.showStatus("No ECU definition loaded", 2500); return; }
        std::set<std::string> have;
        collectPaths(nodeTree.root(), "", have);
        // Prune by LABEL PATH: a node counts as present if the same path exists in the tree, so a branch
        // you kept keeps its children out of the offer while a branch you deleted comes back on offer
        // whole. Returns false when nothing under this node is new (so an all-present branch is dropped).
        std::function<bool(const jf::JJson&, const std::string&, JTreeViewNode&)> prune =
            [&](const jf::JJson& src, const std::string& prefix, JTreeViewNode& out) -> bool {
                const std::string label = src["name"].str();
                const std::string path  = prefix.empty() ? label : prefix + "/" + label;
                JTreeViewNode n{label, true, false, {}};
                bool anyNew = false;
                for (const auto& c : src["children"].arr())
                    if (prune(c, path, n)) anyNew = true;
                if (!anyNew && have.count(path)) return false;   // this node AND its subtree are already here
                out.children.push_back(std::move(n));
                return true;
            };
        JTreeViewNode avail{"New", true, false, {}};
        for (const auto& n : nav.arr()) prune(n, "", avail);
        if (avail.children.empty()) { win.showStatus("Nothing new in the ECU definition", 3000); return; }

        win.openModal<ReseedDialog>(avail, nodeTree.root(),
            std::function<void(std::vector<std::string>)>([&](std::vector<std::string> paths) {
                if (paths.empty()) return;
                // ONE undoable step for the whole import: snapshot, graft, swap. Ctrl+Z takes the lot back
                // out, which is what makes an automatic-looking bulk edit safe to offer.
                JTreeViewNode before = nodeTree.root(), after = before;
                std::sort(paths.begin(), paths.end());          // parents before children
                int added = 0;
                for (const std::string& p : paths) {
                    const std::vector<std::string> parts = splitOn(p, '/');
                    JTreeViewNode* cur = &after;
                    for (const std::string& part : parts) {     // walk, creating what is missing
                        JTreeViewNode* next = nullptr;
                        for (auto& c : cur->children) if (c.label == part) { next = &c; break; }
                        if (!next) {
                            cur->children.push_back(JTreeViewNode{part, true, false, {}});
                            next = &cur->children.back();
                            ++added;
                        }
                        cur = next;
                    }
                }
                if (!added) { win.showStatus("Nothing to import", 2000); return; }
                g_treeUndo.push(new TreeSwap(std::move(before), after));   // push applies via redo()
                treePaths.clear(); collectPaths(nodeTree.root(), "", treePaths);
                g_docDirty = true;
                win.showStatus("Imported " + std::to_string(added) + " node(s) from the ECU definition", 3000);
            }));
    };
    // A PROJECT'S HOST VARIABLES, RE-APPLIABLE. They live beside the dashboard rather than in the meta,
    // because the meta is regenerated from the schema and would lose them — but that also means a meta
    // (re)load WIPES them: loadFile replaces config_ wholesale, so "pc.diag_view" and its option list
    // stopped resolving the moment the schema landed. The Diagnostics view chooser then showed a raw 0
    // with no options and could not be changed, because the path it binds no longer existed.
    //
    // So this is a step, not a line inside the document loader: it runs when the document loads AND again
    // after any meta load.
    g_applyProjectPcVars = [&]{
        if (!ecu) return;
        {
            const std::string pv = (std::filesystem::path(ecu->dir()) / "pcvars.json").string();
            if (std::filesystem::exists(pv)) {
                try {
                    const jf::JJson doc = PanelLibrary::parseDocument(pv);
                    std::vector<MetaModel::PcVar> vars;
                    for (const jf::JJson& o : doc["pcVars"].arr()) {
                        MetaModel::PcVar v;
                        v.name = o["name"].str();
                        if (v.name.empty()) continue;
                        v.datatype = o.contains("datatype") ? o["datatype"].str() : std::string("F32");
                        v.scale = o["scale"].number(1.0);
                        v.minV  = o["min"].number(); v.maxV = o["max"].number();
                        v.units = o["units"].str();  v.label = o["label"].str();
                        v.kind  = o["kind"].str();
                        if (o.contains("default")) { v.hasDef = true; v.defV = o["default"].number(); }
                        for (const jf::JJson& x : o["options"].arr()) v.options.push_back(x.str());
                        vars.push_back(std::move(v));
                    }
                    if (!vars.empty()) {
                        meta.applyPcVars(vars);
                        Cache::instance().initSegments();
                        dictTree.setRootNode(buildDictionary(meta));
                    }
                } catch (...) { /* a malformed side-car must not stop the project opening */ }
            }
        }
    };

    g_loadProjectDoc = [&]{
        if (!ecu) return;
        // PcVariables FIRST: they are paths, and the document about to load may bind widgets to them.
        if (g_applyProjectPcVars) g_applyProjectPcVars();
        // NO PROJECT FILE YET → SEED THE NAVIGATION TREE FROM THE META. The firmware's codegen emits a
        // "navigation_tree" alongside the field descriptions, and it is the ECU's own idea of how its
        // settings are organised — Configuration ▸ Lambda ▸ Lambda LTFT Bank 1, and so on. Without this a
        // first connect to any ECU landed on an empty tree with no way to know one was ever expected.
        //
        // The tree ONLY. Pages are the user's to build (or an .ini import's to supply); this seeds where
        // they hang. Nothing is written here — the seed is deterministic from the meta, so it costs
        // nothing to redo, and connecting has no business creating a project file the user never saved.
        // NO PER-ECU DOCUMENT YET → TAKE THE ONE THAT SHIPPED WITH THE FIRMWARE, IF THERE IS ONE.
        // A dashboard is authored per board (definition/boards/<board>.dashboard.gui in the firmware
        // tree) and installed into the library beside the meta. Without this step a fresh checkout
        // did everything right — build, flash, connect — and still landed on a bare seeded tree,
        // because the authored layout was a file the studio had never been told to look at. Copied
        // in rather than read through: from this moment it is the user's document, theirs to
        // rearrange, and the shipped copy is not something their edits write back to.
        //
        // THE ONE DRAWN FOR THIS ECU'S LAYOUT when the library has it ("<board> <hash>.gui", from a kit,
        // the SD card or a release). Connected, and it has not: nothing is copied yet — the chain after
        // the schema (SD card, then the firmware's release, then the newest dashboard) finds the right
        // one, where copying the newest now would give an ECU on older firmware pages drawn for newer.
        // Offline there is no chain to wait for, so the newest is taken at once.
        if (!std::filesystem::exists(ecu->dashboardPath())) {
            std::error_code ec;
            const std::filesystem::path exact = exactDashboard(ecu->board(), s_ecuLayout);
            const std::string newest = link.isOpen() ? std::string() : newestDashboard(ecu->board());
            const std::filesystem::path shipped =
                (link.isOpen() && !s_ecuLayout.empty() && std::filesystem::exists(exact, ec))
                    ? exact : std::filesystem::path(newest);
            if (!shipped.empty() && std::filesystem::exists(shipped, ec)) {
                std::filesystem::create_directories(
                    std::filesystem::path(ecu->dashboardPath()).parent_path(), ec);
                std::filesystem::copy_file(shipped, ecu->dashboardPath(),
                                           std::filesystem::copy_options::overwrite_existing, ec);
                if (ec)
                    JLOGC("ui.lazy", jf::JLogLevel::Warn)
                        << "could not install the shipped dashboard from " << shipped.string()
                        << ": " << ec.message();
            }
        }
        if (!std::filesystem::exists(ecu->dashboardPath())) {
            // Still nothing local: ask the ECU for it. Seed the tree from the meta FIRST so the window
            // has something in it while the transfer runs, then replace the whole document when it
            // lands — a fetch that fails or finds nothing simply leaves the seeded tree, which is
            // exactly what this did before the card was ever consulted.
            // Fetch first, seed only if there is nothing to fetch: a seeded tree put up while the real
            // document is on its way is a wrong dashboard the user has to watch being replaced.
            if (g_fetchShippedDash && g_fetchShippedDash()) return;
            if (g_seedNavFromMeta) g_seedNavFromMeta();
            return;
        }
        // Parsed through the page store, which is a COMPILED unit — this file is built at a lower
        // optimisation level (its own compile time is the cost of every UI change) and an unoptimised
        // parse of a 7.5 MB document is seven seconds between the window and the interface.
        try { loadDoc(PanelLibrary::parseDocument(ecu->dashboardPath())); } catch (...) { return; }
        g_docDirty = false;                                // a freshly loaded document is clean
    };
    // Ask the ECU for the dashboard that shipped with its firmware. Safe to call whenever: it does
    // nothing unless there is an open link, a resolved schema, an ECU with no document of its own,
    // and no fetch already in flight.
    g_fetchShippedDash = [&]() -> bool {
        const MetaModel* mm = Cache::instance().meta();
        if (!ecu || !mm || !link.isOpen() || !s_pendingDashFetch.empty()) return false;
        if (std::filesystem::exists(ecu->dashboardPath())) return false;
        const std::string board = ecu->board(), hash = mm->layoutHash();
        if (board.empty() || hash.empty()) return false;
        const std::filesystem::path lib = StudioPaths::dataDir("dashboards");
        // IN THE LIBRARY ALREADY: a kit's, or one a release brought with the schema a moment ago.
        if (std::error_code ec0; std::filesystem::exists(exactDashboard(board, hash), ec0)) {
            std::filesystem::create_directories(std::filesystem::path(ecu->dashboardPath()).parent_path(), ec0);
            std::filesystem::copy_file(exactDashboard(board, hash), ecu->dashboardPath(),
                                       std::filesystem::copy_options::overwrite_existing, ec0);
            if (!ec0) { if (g_loadProjectDoc) g_loadProjectDoc(); return true; }
        }
        s_dashFetchInstall = [lib](const std::string&, const std::vector<uint8_t>& data) {
            Ecu* e = ecu;                    // the file-scope open-ECU pointer (main.cpp:1000)
            if (!e) return;
            // THE CARD CARRIES A 4-BYTE CRC32 FOOTER; A DOCUMENT ON DISK MUST NOT. push_meta.py appends
            // it so a transfer can be proved, exactly as it does for the meta. Written through as-is the
            // file loads here anyway — this parser stops at the end of the JSON value — and is silently
            // not JSON to anything else: json.load() on it raises, which is every layout script in
            // tools/layout. It would also heal itself the first time the studio saved, so the corrupt
            // shape would appear only on machines that had never edited, which is all of a user's.
            if (data.size() <= 4) return;
            const size_t body = data.size() - 4;
            const uint32_t want = uint32_t(data[body]) | (uint32_t(data[body + 1]) << 8) |
                                  (uint32_t(data[body + 2]) << 16) | (uint32_t(data[body + 3]) << 24);
            if (crc32_ieee::compute(reinterpret_cast<const char*>(data.data()), int(body)) != want) {
                JLOGC("ui.lazy", jf::JLogLevel::Warn)
                    << "dashboard from the ECU's SD failed its CRC — not installing it";
                return;
            }
            // Kept in the library under its layout's name as well, so a second ECU on that firmware
            // does not pull 7.9 MB over the wire again.
            std::error_code ec; std::filesystem::create_directories(lib, ec);
            const std::string keep = exactDashboard(e->board(), s_ecuLayout).string();
            { std::ofstream f(keep, std::ios::binary);
              if (f) f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(body)); }
            std::filesystem::create_directories(
                std::filesystem::path(e->dashboardPath()).parent_path(), ec);
            std::filesystem::copy_file(keep, e->dashboardPath(),
                                       std::filesystem::copy_options::overwrite_existing, ec);
        };
        s_pendingDashFetch = board + " " + hash + ".gui";
        JLOGC("ui.lazy", jf::JLogLevel::Info)
            << "no dashboard for this ECU — fetching " << s_pendingDashFetch << " from its SD card";
        // A MODAL, for the same reason the schema fetch uses one: this is 7.9 MB, and the alternative
        // is showing a navigation tree seeded from the meta — a DIFFERENT, emptier dashboard — for a
        // minute before the real one replaces it. Watching the wrong layout resolve into the right one
        // reads as a bug, and there is no way for anyone to tell it is not.
        //
        // REUSED IF ONE IS ALREADY UP. On a first connect the schema is fetched from the card too, and
        // these are one wait to the person in front of it: closing that bar and opening this one turns
        // a single ninety-second transfer into two, with a gap in the middle where the studio looks
        // like it finished and then started again.
        win.showStatus("No dashboard for this ECU \xE2\x80\x94 fetching it from the SD card \xE2\x80\xA6", 4000);
        if (auto* d = jf::JProgressDialog::active())
            d->setNote(s_pendingDashFetch + " from the ECU's SD card");
        else
            win.openModal<jf::JProgressDialog>(std::string("Fetching dashboard"),
                                                  s_pendingDashFetch + " from the ECU's SD card");
        link.sdMcu();
        link.fetchFile(s_pendingDashFetch);
        return true;
    };
    // NOT ON THE CARD EITHER. The release that shipped the ECU's firmware carries the dashboard drawn for
    // it (FirmwareFetch::fetchVersion); failing that, the newest dashboard this studio has, which binds by
    // name and names on connect whatever the older firmware does not have. Only a studio that has none at
    // all is left on the tree seeded from the meta.
    s_dashFallback = [&win] {
        Ecu* e = ecu;
        if (!e || std::filesystem::exists(e->dashboardPath())) return;
        const std::string board = e->board(), hash = s_ecuLayout, ver = s_ecuFwVersion;
        auto install = [e](const std::filesystem::path& from, const char* what) {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(e->dashboardPath()).parent_path(), ec);
            std::filesystem::copy_file(from, e->dashboardPath(), std::filesystem::copy_options::overwrite_existing, ec);
            JLOGC("ui.lazy", jf::JLogLevel::Info) << "dashboard: " << what << " (" << from.string() << ")"
                                                   << (ec ? " — could not copy: " + ec.message() : std::string());
            if (!ec && g_loadProjectDoc) g_loadProjectDoc();
        };
        auto newest = [board, install] {
            if (const std::string n = newestDashboard(board); !n.empty()) install(n, "the newest one this studio has");
        };
        if (ver.empty() || hash.empty()) { newest(); return; }
        win.showStatus("Looking online for the dashboard drawn for " + board + " " + ver + " \xE2\x80\xA6", 6000);
        fwfetch::fetchVersion(board, ver, hash, false, true,
            [e, board, hash, ver, install, newest, &win](const fwfetch::VersionFiles& r) {
                if (ecu != e || std::filesystem::exists(e->dashboardPath())) return;   // moved on meanwhile
                if (!r.dashboard.empty()) {
                    const auto keep = exactDashboard(board, hash);
                    std::error_code ec; std::filesystem::create_directories(keep.parent_path(), ec);
                    { std::ofstream f(keep, std::ios::binary);
                      f.write(reinterpret_cast<const char*>(r.dashboard.data()), std::streamsize(r.dashboard.size())); }
                    install(keep, "downloaded from the firmware's release");
                    win.showStatus("Dashboard for " + board + " " + ver + " downloaded", 5000);
                    return;
                }
                JLOGC("ui.lazy", jf::JLogLevel::Info) << "no released dashboard for " << board << " " << ver
                                                       << ": " << r.error;
                newest();
            });
    };

    // Real unsaved-changes gate (pass-through until now): prompt Save/Discard/Cancel, honour the
    // remembered saveOnExit choice, then run the destructive action. The tune itself always auto-saves.
    // The gate in front of anything that DITCHES the current project — connecting to another ECU, opening
    // a project, creating one, quitting. Clean: just do it, no prompt, because swapping to one set of
    // files for another is not an edit. Dirty: ask, unless the user has already said what they want done
    // (Preferences ▸ Editor, or the dialog's "remember" tick). Cancel abandons the action itself.
    maybeSaveThen = [&](std::function<void()> then) {
        saveActiveTune();                        // the calibration auto-saves; only the LAYOUT is in question
        if (!g_docDirty) { then(); return; }
        const int mode = settings.get<int>("saveOnExit", 0);
        if (mode == 1) { saveAllFn(); then(); return; }
        if (mode == 2) { g_docDirty = false; then(); return; }
        win.openModal<SaveChangesDialog>(std::string("Save changes to this project first?"),
            std::function<void(int, bool)>([&, then](int result, bool remember) {
                if (result == 0) return;                                        // Cancel → the action is off
                if (remember) settings.set("saveOnExit", jf::JVariant(result));
                if (result == 1) saveAllFn(); else g_docDirty = false;
                then();
            }));
    };
    g_adoptConnectedEcu = [&](Ecu* e, std::function<void()> after) {
        if (!e) return;
        if (ecu && ecu->uid() == e->uid()) {              // reconnecting to the same ECU: keep the live layout
            JLOGC("ui", jf::JLogLevel::Info) << "[connect] same ECU (" << e->uid() << ") — keeping the live layout";
            if (after) after();
            return;
        }
        maybeSaveThen([&, e, after] {                      // dirty layout → ask; cancelling abandons the adopt
            saveActiveTune();                              // persist the OUTGOING project before switching
            ecu = e;
            noteRecentEcu(e->label(), e->uid());           // THIS is now the last project, whatever protocol it speaks
            // Resume this ECU's last calibration name so edits auto-save back to it (default: "current").
            activeTuneName = activeTuneFor(e);
            if (g_loadProjectDoc) g_loadProjectDoc();      // document, or a navigation tree seeded from the meta
            bootMark("document loaded");
            // Reveal the surfaces only NOW — after the layout is loaded — so connecting from the landing
            // doesn't briefly show the previous surface before the correct dashboard appears.
            if (g_showSurfaces) g_showSurfaces();
            // The saved per-node expansion PREFERENCE (a UI pref, kept in settings) on top of the tree.
            { std::set<std::string> exp; std::string str = settings.get<std::string>("tree.expanded", ""); size_t p = 0;
              while (p < str.size()) { const size_t x = str.find('\n', p);
                const std::string t = str.substr(p, x == std::string::npos ? std::string::npos : x - p);
                if (!t.empty()) exp.insert(t); if (x == std::string::npos) break; p = x + 1; }
              if (!exp.empty()) { JTreeViewNode r = nodeTree.root(); for (auto& c : r.children) applyExpanded(c, "", exp); setTree(std::move(r)); } }
            treePaths.clear(); collectPaths(nodeTree.root(), "", treePaths);
            if (after) after();
        });
    };

    Surface::onModified = []{ g_docDirty = true; };       // any surface edit dirties the document
    // Deleting a node's LAST viewport frees the node's page at once (the tree node stays): see
    // SurfaceTabs::gcNodeIfUnplaced.
    Surface::onViewportRemoved = [&surfaceTabs](const std::string& node){ surfaceTabs.gcNodeIfUnplaced(node); g_docDirty = true; };

    // Startup preferences (Preferences▸Editor — both keys were saved but never read): reopen the last
    // project offline, and/or kick off the connect scan on launch. Reopen goes through openTune, which
    // now also restores the project document + newest restore-point baseline.
    if (settings.get<bool>("startup.reopenLastProject", false)) {
        const std::string rec  = settings.get<std::string>("recent.ecus", "");
        const std::string line = rec.substr(0, rec.find('\n'));
        const size_t tab = line.find('\t');
        if (tab != std::string::npos) {
            if (Ecu* e = Ecu::open(line.substr(tab + 1))) {
                win.setCentralWidget(&mdi);
                if (g_openProject) g_openProject(e, activeTuneFor(e));
            }
        }
    }
    if (settings.get<bool>("connection.autoConnect", false)) {
        win.setCentralWidget(&mdi);
        if (connectFn) connectFn();
    }
    // Not during a scripted run: those are tests and screenshots, and must not depend on the network.
    if (settings.get<bool>("updates.checkOnStartup", true) && !g_run.any())
        s_updater.check(false);
    if (settings.get<bool>("updates.firmwareOnStartup", true) && !g_run.any())
        s_checkFirmware(false);
    // THE APPLICATIONS MENU (Linux AppImage only; DesktopIntegration.h). Already there: say nothing, and
    // quietly re-point it if this AppImage has moved. Not there: ask once — and a "no" is remembered as
    // the Preferences ▸ Updates tick box, so nobody is asked at every launch. Never in a scripted run.
    if (!g_run.any()) {
        const desktop::MenuState ms = desktop::menuState();
        if (ms == desktop::MenuState::Installed) {
            desktop::install();
        } else if (ms == desktop::MenuState::NotInstalled && !settings.get<bool>(desktop::kSkipSetting, false)) {
            win.openModal<ChoiceDialog>(
                std::string("Add jayECU Studio to your applications menu?"),
                std::string("The studio is running from an AppImage, which does not install itself. Adding it "
                            "puts jayECU Studio and its icon in your applications menu, for you only, "
                            "pointing at this file. Nothing else on the computer changes.\n"
                            "If you choose not to, you will not be asked again. You can turn the question "
                            "back on in Edit \xE2\x96\xB8 Preferences \xE2\x96\xB8 Updates."),
                std::vector<ChoiceDialog::Choice>{
                    { "Add to menu", jf::JDialogButtonBox::Role::Accept },
                    { "Don't add",   jf::JDialogButtonBox::Role::Reject } },
                std::function<void(int)>([&win](int idx) {
                    if (idx == 0) {
                        win.showStatus(desktop::install() ? "jayECU Studio added to the applications menu"
                                                          : "Could not add jayECU Studio to the applications menu",
                                       4000);
                    } else {
                        // Declined, or dismissed: either way the answer is "not now, and don't nag".
                        jf::JSettings::instance().set(desktop::kSkipSetting, true);
                    }
                }));
        }
    }

    // Housekeeping at 4 Hz: SIGTERM/SIGINT → the same guarded close as the window ✕ (save prompt included);
    // window title ← "<tune>[*] — studio[ — <board ver (hash)>]", recomputed from live state, set on change.
    // A scripted run (--import / --node / --shot / --quit-after) drives itself from here, a tick at a time,
    // because each step needs the one before it to have SETTLED: an import rebuilds the tree and the
    // surfaces, and a capture taken in the same tick would photograph a half-built frame. Counting ticks is
    // cruder than chaining callbacks and far easier to reason about when a step misbehaves.
    installScriptedRun(win, nodeTree, connectFn, lockBtn);

    // GATING (MenuGate): offer only what this ECU can do. SHOW = applies to this kind of ECU at all;
    // ENABLE = can be done now. The state is read fresh each tick, so no event has to be caught.
    //   project  — an ECU's tune and layout are open (Close ECU clears it).
    //   native   — a jayecu definition; imported — a TunerStudio/rusEFI .ini brought in by the importer.
    //   nativeUp — the jayecu link is past identity and config (the chip is green), not merely a port.
    //   busy     — any link open or opening: nothing may swap the definition or project under it.
    static MenuGate gate;
    {
        auto project  = [] { return ecu != nullptr; };
        auto native   = [] { return meta.isValid() && meta.tsSignature().empty(); };
        auto imported = [] { return meta.isValid() && !meta.tsSignature().empty(); };
        auto engine   = [] { return !meta.isValid() || meta.deviceClass() == "ecu"; };   // not a TCU
        auto nativeUp = [&connectBtn] { return link.isOpen() && connectBtn.connState() == ConnectButton::State::Connected; };
        auto tsUp     = [] { return tsLink.isOpen(); };
        auto busy     = [&connectBtn] { return link.isOpen() || tsLink.isOpen()
                                               || connectBtn.connState() == ConnectButton::State::Connecting; };

        // FILE — the open ECU's documents. Nothing that swaps the project runs under a live link.
        gate.item(fileMenu, "New Tune…",       [busy] { return Gate{ true, !busy() }; });
        gate.item(fileMenu, "Open ECU…",       [busy] { return Gate{ true, !busy() }; });
        gate.item(fileMenu, "Recent ECUs",     [busy] { return Gate{ true, !busy() }; });
        gate.item(fileMenu, "Open Tune…",      [project] { return Gate{ true, project() }; });
        gate.item(fileMenu, "Save Tune",       [project] { return Gate{ true, project() && !activeTuneName.empty() }; });
        gate.item(fileMenu, "Save Tune As\xE2\x80\xA6", [project] { return Gate{ true, project() && !activeTuneName.empty() }; });
        gate.item(fileMenu, "Open Layout…",    [project] { return Gate{ true, project() }; });
        gate.item(fileMenu, "Save Layout",     [project] { return Gate{ true, project() }; });
        gate.item(fileMenu, "Save Layout As…", [project] { return Gate{ true, project() }; });
        gate.item(fileMenu, "Close ECU",       [project, busy] { return Gate{ true, project() && !busy() }; });

        // EDIT
        for (const char* l : { "Undo", "Redo", "Cut", "Copy", "Paste" })
            gate.item(editMenu, l, [project] { return Gate{ true, project() }; });
        // The restore pushes over the jayecu link only; on an imported ECU it would change the studio's
        // copy and never the ECU's, so it is not offered there.
        gate.item(editMenu, "Restore Tune to Connect Point", [imported, nativeUp] {
            return Gate{ !imported(), nativeUp() && Cache::instance().hasBaseline() }; });

        // VIEW
        // Cache::readOnly() is true while EDITING (it guards the tune, not the layout) -- the one mode
        // where hidden alternatives can be shown.
        gate.item(viewMenu, "Show Hidden Widgets", [] { return Gate{ true, Cache::instance().readOnly() }; });
        for (const char* l : { "New Surface Tab", "Delete Surface\xE2\x80\xA6", "Reopen Surface…" })
            gate.item(viewMenu, l, [project] { return Gate{ true, project() }; });

        // LIBRARY — definitions replace what the studio reads the ECU through: not under a live link.
        gate.item(libraryMenu, "Load ECU Definition…",     [busy] { return Gate{ true, !busy() }; });
        gate.item(libraryMenu, "Import TunerStudio .ini…", [busy] { return Gate{ true, !busy() }; });

        // TOOLS — each is offered for the ECU kinds it works with, and enabled when its link is up.
        gate.item(toolsMenu, "Trigger Designer", [imported, engine] { return Gate{ !imported() && engine(), true }; });
        gate.item(toolsMenu, "Engine Cycle",     [imported, nativeUp, tsUp, engine] {
            return Gate{ engine(), imported() ? tsUp() : nativeUp() }; });
        gate.item(toolsMenu, "Trigger Log",      [imported, nativeUp, engine] { return Gate{ !imported() && engine(), nativeUp() }; });
        gate.item(toolsMenu, "Knock Scope",      [imported, nativeUp, engine] { return Gate{ !imported() && engine(), nativeUp() }; });
        gate.item(toolsMenu, "Auto Tune",        [nativeUp, tsUp] {
            return Gate{ meta.isValid() && meta.autotune().valid(), nativeUp() || tsUp() }; });
        gate.item(toolsMenu, "Reset ECU",        [imported, nativeUp, engine] { return Gate{ !imported() && engine(), nativeUp() }; });

        // LOGGING — the PC-side recording needs a definition with telemetry; the card is a jayecu thing.
        for (const char* l : { "Start Recording", "Stop Recording" })
            gate.item(logMenu, l, [] { return Gate{ true, DatalogRecorder::instance().recording()
                                                          || (meta.isValid() && (link.isOpen() || tsLink.isOpen())) }; });
        gate.item(logMenu, "Recording Channels\xE2\x80\xA6", [] { return Gate{ true, meta.isValid() }; });
        gate.item(logMenu, "Onboard Logging\xE2\x80\xA6", [imported, project, engine] { return Gate{ !imported() && engine(), project() }; });
        gate.item(logMenu, "Logs on Card\xE2\x80\xA6",    [imported, engine] { return Gate{ !imported() && engine(), true }; });

        // HELP — the Lua reference is built from the definition; offered when it declares one.
        gate.item(helpMenu, "Add Missing Navigation Entries\xE2\x80\xA6", [project] {
            return Gate{ lockBtn.isToggled(), meta.isValid() && !meta.navigationTree().arr().empty() && project() }; });
        gate.item(helpMenu, "Lua API Reference", [] {
            return Gate{ meta.isValid() && (!meta.luaFunctions().empty() || !meta.luaCallbacks().empty()), true }; });

        // TOOLBAR
        gate.widget(resetBtn,  [imported, nativeUp] { return Gate{ !imported(), nativeUp() }; });
        gate.widget(verifyBtn, [nativeUp, tsUp] { return Gate{ true, (nativeUp() || tsUp()) && Cache::instance().hasConfig() }; });
        gate.widget(recBtn,    [] { return Gate{ true, DatalogRecorder::instance().recording()
                                                     || (meta.isValid() && (link.isOpen() || tsLink.isOpen())) }; });
    }
    gate.apply();

    static jf::JTimer houseTimer;
    houseTimer.onTick.connect([&win]{
        gate.apply();
        if (g_termRequest) { g_termRequest = 0; win.requestClose(); }
        if (g_trackDockHomes) g_trackDockHomes();   // keep each panel's last-known placement current
        if (g_syncViewToggles) g_syncViewToggles(); // and the View menu honest about what's on screen
        std::string t = "jayecu Studio";
        if (ecu && !activeTuneName.empty()) t = activeTuneName + (g_docDirty ? " *" : "") + " \xE2\x80\x94 studio";
        if (!s_connTitle.empty()) t += " \xE2\x80\x94 " + s_connTitle;
        static std::string lastTitle;
        if (t != lastTitle) { lastTitle = t; win.window().setTitle(t); }
    });
    houseTimer.start(std::chrono::milliseconds(250), jf::JTimer::JMode::Repeating);

    // Save-on-close prompt (both ✕ and File▸Quit, via JAppWindow::onCloseRequest). Same rule as every
    // other ditch: ask, unless a preference already answers it. saveOnExit: 0 = Ask, 1 = Save, 2 = Discard,
    // set by the dialog's "remember" tick and editable in Preferences ▸ Editor.
    win.onCloseRequest = [&]() -> bool {
        // NEVER DURING A FIRMWARE UPDATE. Closing mid-flash stops the write partway: the ECU is left in
        // its bootloader with no working firmware, and its USB can hang until it is reset.
        if (s_upgrade.writing()) {
            jf::JDialog::message("Firmware update in progress",
                "The ECU's firmware is being updated. Closing the studio now would leave the ECU "
                        "without working firmware.\n\nPlease wait for the update to finish.");
            return false;
        }
        saveActiveTune();                                       // the tune always auto-saves (silent)
        if (!g_docDirty || g_closeState == 2) return true;      // nothing unsaved, or the user already chose
        const int mode = settings.get<int>("saveOnExit", 0);
        if (mode == 1) { saveAllFn(); return true; }            // remembered: always save
        if (mode == 2) return true;                             // remembered: always discard
        if (g_closeState == 1) return false;                    // prompt already up — keep the window alive
        g_closeState = 1;
        win.openModal<SaveChangesDialog>(std::string("Save changes to this project before closing?"),
            std::function<void(int, bool)>([&](int result, bool remember) {
                if (result == 0) { g_closeState = 0; return; }  // Cancel → stay open
                if (remember) settings.set("saveOnExit", jf::JVariant(result));   // 1=Save / 2=Discard
                if (result == 1) saveAllFn();
                g_closeState = 2;                               // confirmed → the next close proceeds
                win.requestClose();
            }));
        return false;
    };
    // Rename (F2 / double-click / menu): the node label changed. Re-key its page + every viewport that
    // referenced it, so placements follow the rename (PanelLibrary::renamePrefix). treeSelPath holds the
    // pre-rename path (selection can't change mid-edit); the new path is read back from the tree.
    nodeTree.onNodeRenamed.connect([&](JTreeViewNode* n){
        if (!n) return;
        // Promote-on-edit: a "New node…" ghost that got a real, non-empty name becomes a real node, and
        // syncPlaceholders trails a fresh ghost in its place. An empty / unchanged label leaves it a ghost
        // (restoring the caption if it was cleared). n is still valid here (commit hasn't rebuilt the tree yet);
        // relocate it in a fresh copy by index-path since setRootNode invalidates the pointer.
        if (n->placeholder) {
            const std::string ph = placeholderCaption();
            const std::string label = n->label;
            std::vector<int> path;
            if (!findIndexPath(nodeTree.root(), n, path)) return;
            JTreeViewNode copy = nodeTree.root();
            JTreeViewNode* pn = nodeAtPath(copy, path); if (!pn) return;
            if (!label.empty() && label != ph) {
                pn->placeholder = false;                  // promote ghost → real node
                syncPlaceholders(copy, treeEditing);      // fresh trailing ghosts reappear (incl. under the new node)
                const std::string np = joinLabels(copy, path);   // the promoted node's path (ghosts are trailing → index stable)
                nodeTree.setRootNode(std::move(copy));
                persistTree();
                // Make the new node the ACTIVE view. setRootNode restores the previous selection silently,
                // so without this the surface stays on the old node and a viewport dropped on the new node
                // (node-tagged → shown only for the active node) would be invisible until it's reselected.
                if (!np.empty()) nodeTree.selectByPath(np);
            } else if (label != ph) {                     // emptied → restore the ghost caption
                pn->label = ph;
                syncPlaceholders(copy, treeEditing);
                nodeTree.setRootNode(std::move(copy));
            }
            return;
        }
        std::string newPath; nodePathOf(nodeTree.root(), n, "", newPath);
        if (!treeSelPath.empty() && !newPath.empty() && treeSelPath != newPath) {
            panelLibrary.renamePrefix(treeSelPath, newPath);
            panelLibrary.retagRefs(treeSelPath, newPath);
            surfaceTabs.retagNodeRefs(treeSelPath, newPath);
        }
        treeSelPath = newPath;
        persistTree();   // deferred: in-memory only; panelLibrary flushes on Save / clean exit, not per edit
    });
    // Internal drag-reorder finished (framework moved the node within the tree). Recover the moved subtree's
    // old→new path by diffing the path set (a single drag moves exactly one subtree: the shortest removed
    // path is its old root, the shortest added path its new root) and re-key its page + viewports.
    nodeTree.onNodeMoved.connect([&](JTreeViewNode*, JTreeViewNode*, int){
        std::set<std::string> now; collectPaths(nodeTree.root(), "", now);
        std::vector<std::string> removed, added;
        std::set_difference(treePaths.begin(), treePaths.end(), now.begin(), now.end(), std::back_inserter(removed));
        std::set_difference(now.begin(), now.end(), treePaths.begin(), treePaths.end(), std::back_inserter(added));
        if (!removed.empty() && !added.empty()) {
            auto shortest = [](const std::vector<std::string>& v){ const std::string* s = &v.front();
                for (const auto& x : v) if (x.size() < s->size()) s = &x; return *s; };
            const std::string oldPfx = shortest(removed), newPfx = shortest(added);
            panelLibrary.renamePrefix(oldPfx, newPfx);
            panelLibrary.retagRefs(oldPfx, newPfx);
            surfaceTabs.retagNodeRefs(oldPfx, newPfx);
        }
        persistTree();   // deferred: panelLibrary flushes on Save / clean exit, not per edit
    });
    // Context-menu tree reordering (replaces drag-reorder, which can't coexist with the out-of-tree
    // placement drag in this dock router). Each op relocates the selection in a copy of the tree; ops that
    // change a node's path (Indent/Outdent) re-key its page + viewports via the same before/after diff.
    auto rekeyMovedSubtree = [&](const std::set<std::string>& before){
        std::vector<std::string> removed, added;
        std::set_difference(before.begin(), before.end(), treePaths.begin(), treePaths.end(), std::back_inserter(removed));
        std::set_difference(treePaths.begin(), treePaths.end(), before.begin(), before.end(), std::back_inserter(added));
        if (removed.empty() || added.empty()) return;
        auto shortest = [](const std::vector<std::string>& v){ const std::string* s = &v.front();
            for (const auto& x : v) if (x.size() < s->size()) s = &x; return *s; };
        const std::string oldPfx = shortest(removed), newPfx = shortest(added);
        panelLibrary.renamePrefix(oldPfx, newPfx); panelLibrary.retagRefs(oldPfx, newPfx);
        surfaceTabs.retagNodeRefs(oldPfx, newPfx);   // deferred: treeMutate already dirtied; flush on Save
    };
    auto reorder = [&](const char* what, const std::function<bool(JTreeViewNode&, JTreeViewNode*, const std::vector<int>&)>& fn){
        if (!requireEdit(what)) return;
        std::set<std::string> before = treePaths;
        treeMutate(fn);                    // commits + persists the tree + refreshes treePaths
        rekeyMovedSubtree(before);
    };
    treeMenu.addSeparator(g);
    treeMenu.add(g, "Move Up")->onTriggered.connect([&]{ reorder("Move Up", [](JTreeViewNode& root, JTreeViewNode*, const std::vector<int>& path){
        if (path.empty() || path.back() == 0) return false;
        std::vector<int> pp(path.begin(), path.end() - 1); JTreeViewNode* p = nodeAtPath(root, pp); if (!p) return false;
        std::swap(p->children[path.back()], p->children[path.back() - 1]); return true; }); });
    treeMenu.add(g, "Move Down")->onTriggered.connect([&]{ reorder("Move Down", [](JTreeViewNode& root, JTreeViewNode*, const std::vector<int>& path){
        if (path.empty()) return false; std::vector<int> pp(path.begin(), path.end() - 1); JTreeViewNode* p = nodeAtPath(root, pp); if (!p) return false;
        if (path.back() + 1 >= static_cast<int>(p->children.size())) return false;
        std::swap(p->children[path.back()], p->children[path.back() + 1]); return true; }); });
    treeMenu.add(g, "Indent (Into Sibling Above)")->onTriggered.connect([&]{ reorder("Indent", [](JTreeViewNode& root, JTreeViewNode*, const std::vector<int>& path){
        if (path.empty() || path.back() == 0) return false;
        std::vector<int> pp(path.begin(), path.end() - 1); JTreeViewNode* p = nodeAtPath(root, pp); if (!p) return false;
        const int i = path.back(); JTreeViewNode moved = std::move(p->children[i]); p->children.erase(p->children.begin() + i);
        JTreeViewNode& prev = p->children[i - 1]; prev.expanded = true; prev.children.push_back(std::move(moved)); return true; }); });
    treeMenu.add(g, "Outdent (To Parent Level)")->onTriggered.connect([&]{ reorder("Outdent", [](JTreeViewNode& root, JTreeViewNode*, const std::vector<int>& path){
        if (path.size() < 2) return false;
        std::vector<int> pp(path.begin(), path.end() - 1); JTreeViewNode* parent = nodeAtPath(root, pp); if (!parent) return false;
        std::vector<int> gp(pp.begin(), pp.end() - 1); JTreeViewNode* grand = nodeAtPath(root, gp); if (!grand) return false;
        const int i = path.back(), pIdx = pp.back(); JTreeViewNode moved = std::move(parent->children[i]); parent->children.erase(parent->children.begin() + i);
        grand->children.insert(grand->children.begin() + pIdx + 1, std::move(moved)); return true; }); });

    // Drag a node OUT of the tree onto a surface → a NodePlacement payload (the surface drops it as a
    // viewport). internalReorder is OFF, so onNodeDragStarted fires as soon as the drag passes threshold.
    // Copy semantics: the node stays put, only a reference is placed.
    nodeTree.onNodeDragStarted.connect([&nodeTree](JTreeViewNode* n){
        if (!n) return;
        std::string path; nodePathOf(nodeTree.root(), n, "", path);
        if (!path.empty()) jf::JDragDrop::start<NodePlacement>(NodePlacement{path}, 0.f, 0.f, n->label);
    });
    // NB: double-click / activate a tree node does NOT open a tab — the framework begins an inline rename
    // (EditTree::mouseDoubleClickEvent parity). A node's page is authored by entering one of its viewports
    // in place on a surface; "Open in separate tab" is a viewport right-click item (SurfaceTabs::openNode).
    // A node deleted (menu or Delete key): forget its subtree's pages + close any open node-page tab.
    onNodeRemoved = [&surfaceTabs](const std::string& path){
        if (path.empty()) return;
        panelLibrary.removePrefix(path);
        surfaceTabs.forgetNode(path);
        g_docDirty = true;   // deferred: panelLibrary flushes on Save / clean exit, not per edit
    };
    // Tree keys: Delete removes the selection, Enter adds a sibling.
    nodeTree.onDeleteKey.connect([&]{
        if (!treeEditing) { win.showStatus("Delete — enable Editing first", 2500); return; }
        const JTreeViewNode* sel = nodeTree.selectedNode(); if (!sel) return;
        std::string delPath; nodePathOf(nodeTree.root(), sel, "", delPath);
        treeMutate([](JTreeViewNode& root, JTreeViewNode*, const std::vector<int>& path){
            if (path.empty()) return false; std::vector<int> pp(path.begin(), path.end() - 1);
            JTreeViewNode* parent = nodeAtPath(root, pp); if (!parent) return false;
            parent->children.erase(parent->children.begin() + path.back()); return true; });
        onNodeRemoved(delPath); win.showStatus("Deleted node", 1500);
    });
    nodeTree.onEnterKey.connect([&]{
        if (!treeEditing) return;
        std::string np;
        treeMutate([&np](JTreeViewNode& root, JTreeViewNode*, const std::vector<int>& path){
            if (path.empty()) { root.children.push_back(JTreeViewNode{"New node", false, false, {}});
                np = joinLabels(root, {static_cast<int>(root.children.size()) - 1}); return true; }
            std::vector<int> pp(path.begin(), path.end() - 1);
            JTreeViewNode* parent = nodeAtPath(root, pp); if (!parent) return false;
            const int at = path.back() + 1;
            parent->children.insert(parent->children.begin() + at, JTreeViewNode{"New node", false, false, {}});
            std::vector<int> ip = pp; ip.push_back(at);
            np = joinLabels(root, ip); return true; });
        if (!np.empty()) nodeTree.selectByPath(np);
    });
    // Live values: every telemetry frame repaints the active surface, so bound cards tick in real time.
    // Run-mode tree filtering (ports EditTree::applyConditions): when locked, hide every node whose
    // visibility condition (stored in userData) evaluates false; when editing, every node is shown. Cheap +
    // idempotent — safe to call on lock and each telemetry frame (live evaluation).
    auto applyNodeConditions = [&nodeTree, &treeEditing]{
        if (treeEditing) { nodeTree.clearVisibility(); return; }
        // HIDDEN MEANS HIDDEN. A condition that fails takes its node and its subtree out of the tree, so
        // the tree lists what this tune actually has — which is the whole point of gating a node on a
        // feature's enable, and the reason the flag is called `hidden`.
        //
        // These were briefly DIMMED instead, so a switched-off feature still showed in the tree. The
        // discoverability that was meant to buy is already paid for, and better: JTreeView's
        // search REVEALS a hidden match (drawn dim, still selectable), so a feature that is switched off
        // is found by typing its name, turned on from its own page, and joins the live tree when the
        // search clears. A permanently greyed row is 300 rows of things this car does not have, in the
        // way of the ones it does.
        nodeTree.applyVisibility([](const JTreeViewNode& n){
            return n.userData.empty() || MathEvaluator::instance().evaluate(n.userData) != 0.0;
        });
    };
    Cache::instance().frameUpdated.connect([&surfaceTabs, applyNodeConditions]{ surfaceTabs.invalidate(); applyNodeConditions(); refreshLinkOverlay(link.isOpen()); });
    // Every telemetry frame, hold the maintained constants at what their expression says. The expression
    // is written to evaluate to the field's OWN value except when a calibration is being reported, so this
    // is a no-op until the ECU has something to hand over — and the comparison keeps it a no-op even then,
    // rather than rewriting the same value forever. The write goes down the ordinary path, so it reaches
    // the ECU and is burned with everything else.
    Cache::instance().frameUpdated.connect([&win]{
        if (g_maintained.empty() || !tsLink.isOpen()) return;
        Cache& c = Cache::instance();
        for (const auto& m : g_maintained) {
            // UNITS. TunerStudio's expressions are in DISPLAY units throughout — `calibrationValue` is the
            // volts the ECU just measured — while this Cache stores and writes RAW (the widgets cook). For a
            // field with a scale that difference is the whole value: rusEFI keeps tpsMin/tpsMax as ADC counts
            // at 0.005 V each, so writing a measured 4.5 V straight in stored FOUR counts and the page read
            // back 0.02 V. Fields with scale 1 (the pedal voltages, stored as floats) hid it completely.
            //
            // So: cook the field's own reference before evaluating (the expression's fallback branch IS the
            // field — `select(mode == 1, calibrationValue, ts.tpsMax)` — and it must not arrive raw while the
            // other branch is in volts), then convert the answer back to raw to write it.
            const double scale   = c.configScale(m.target);
            const double haveRaw = c.value(m.target);
            const double want    = MathEvaluator::instance().evaluate(
                                       _substituteToken(m.expr, m.target, haveRaw * scale));
            if (std::isnan(want)) continue;
            const double wantRaw = scale != 0.0 ? want / scale : want;
            // "Already there" is a float epsilon for a float field and half a count for an integer one:
            // an S16 cannot hold the difference between 899.7 and 900, and rewriting it every frame would
            // be a write storm to the ECU.
            const double tol = c.configDatatype(m.target) == "F32" ? 1e-9 : 0.5;
            if (std::abs(wantRaw - haveRaw) < tol) continue;
            JLOGC("comms", jf::JLogLevel::Info) << "maintained " << m.target << ": raw " << haveRaw
                << " -> " << wantRaw << " (" << want << " display units, scale " << scale << ")";
            c.setConfigValue(m.target, wantRaw);
            win.showStatus("Calibrated " + m.target + " = " + std::to_string(want), 4000);
        }
    });
    // WHAT the config error IS. rusEFI raises hasCriticalError and TunerStudio then asks the ECU to say
    // what is wrong ([TunerStudio] retrieveConfigError); the lamp on its own is "something is broken, good
    // luck". The answer can be long and several lines — a list of every misconfigured pin, say — so the
    // status bar is not where it goes: it lands in DIAGNOSTICS, whole and scrollable, which is where an
    // ECU's own words about itself belong, with a one-line summary on the status bar pointing at it.
    //
    // Asked once per rising edge, and re-asked only while the ECU has not answered yet.
    Cache::instance().frameUpdated.connect([&win]{
        static bool s_wasErr = false;
        if (!tsLink.isOpen()) { s_wasErr = false; return; }
        const jf::JJson& proto = s_tsProto;
        const std::string cmd = proto["configErrorCommand"].str();
        const bool err = Cache::instance().value("hasCriticalError") != 0.0;
        if (!err) {
            if (s_wasErr) { s_wasErr = false; win.setStatusText("Connected (TS): " + s_connTitle); }
            return;
        }
        if (s_wasErr || cmd.empty()) return;           // already asked (or nothing to ask with)
        // ASK ONCE. This is a blocking round trip on the same link the telemetry poll just used, and it is
        // called from a telemetry callback — so "retry until it answers" means a blocking read on EVERY
        // frame, with the UI waiting behind each one. An ECU that does not answer this command must cost
        // one attempt, not a stalled application: mark it asked whatever comes back.
        s_wasErr = true;
        const std::string text = tsLink.configErrorText(cmd);
        JLOGC("comms", jf::JLogLevel::Warn) << "ECU config error: "
            << (text.empty() ? "(the ECU answered nothing: " + tsLink.lastError() + ")" : text);
        if (text.empty()) {                            // say SOMETHING: the lamp is lit either way
            win.setStatusText("\xE2\x9A\xA0 Config error \xC2\xB7 the ECU did not say what it is");
            return;
        }
        diag.appendConsole("\xE2\x9A\xA0 ECU configuration error\n" + text + "\n");
        diag.showConsole();                            // pop the tab: an error nobody can find is not reported
        // …and one line of it where the eye already is, naming where the rest of it lives.
        const std::string firstLine = text.substr(0, text.find('\n'));
        win.setStatusText("\xE2\x9A\xA0 Config error \xC2\xB7 " + firstLine
                          + (firstLine.size() < text.size() ? "  (full text in Diagnostics)" : ""));
    });

    // A menu node's condition reads SETTINGS, not just live telemetry: "Staged injection outputs" appears
    // when staged injection is on, "VE 3D view" when injection is enabled. Re-filtering only on a telemetry
    // frame meant an OFFLINE project (an import with no ECU attached) never re-filtered at all — every
    // conditional node stayed visible, when the tree should show the menu the current tune actually calls for.
    // A config load is a new tune; a value edit may be the very setting a condition reads.
    g_refilterTree = applyNodeConditions;      // tree rebuilds re-apply the filter through this
    applyNodeConditions();                     // and the tree standing right now gets it too
    Cache::instance().configLoaded.connect(applyNodeConditions);
    Cache::instance().configValueChanged.connect([applyNodeConditions](const std::string&) { applyNodeConditions(); });
    // The notice strip reads the OUTPUT GATEWAYS, which are config and not telemetry: a toggle changes
    // them between frames, and a config load brings a whole tune's worth at once. Refreshing only on
    // frameUpdated would have left the strip a frame behind on a live link and permanently wrong on a
    // quiet one — the tune says the outputs are off and the banner still says nothing.
    Cache::instance().configLoaded.connect([] { refreshLinkOverlay(link.isOpen()); });
    Cache::instance().configValueChanged.connect([](const std::string&) { refreshLinkOverlay(link.isOpen()); });
    // THE STUDIO LAYS OUT THE COILS AND INJECTORS — never the firmware. An edit to a setting that decides them
    // (cylinder count, cycle, ignition mode, stages) rewrites those output rows in the same undo step.
    engine_outputs::install(Cache::instance());
    // …and a row's Cylinder picker offers only what this engine and that row's stage can use.
    ComboBoxWidget::optionFilter = [](const std::string& b) { return engine_outputs::cylinderOptions(Cache::instance(), b); };
    Cache::instance().tableEdited.connect(applyNodeConditions);
    // The Locked/Editing toolbar toggle drives app-wide edit mode. The
    // authoring docks — Properties, Dictionary, Widgets palette — are edit only: run mode removes them
    // (operator sees just the live instrument); edit mode restores them.
    // Edit and run each keep their OWN complete dock arrangement across mode flips. On a flip we capture
    // the OUTGOING mode's whole layout (every dock host + reserved sizes) and restore the INCOMING mode's.
    // Capturing/restoring all hosts together is what makes it robust: a partial per-host rebuild can
    // orphan a dock (clear it from one host without re-adding it), but a full-space restore places every
    // managed panel exactly once — a panel absent from the target layout is simply not shown in that mode.
    // Title->dock resolution is the FRAMEWORK'S registry, not a list kept here.
    //
    // This was a hand-written if-chain of every dock's title, and it had to be edited whenever a dock was
    // added. "Trigger Library" was added and this was not, so restoring a mode's layout resolved its title
    // to null, restore() skipped it, and the panel was left in no host at all — it vanished on the first
    // live→edit→live flip and did not come back until the layout was reset. Nothing failed to compile and
    // nothing was logged, because a null resolve is how the resolver says "deliberately not this one".
    //
    // JDockWidget::byTitle asks the live-panel registry, so a dock that exists is resolvable the moment it
    // is constructed and there is no second list to keep in step.
    // Every dock area, TOP INCLUDED. It used to hold left/right/bottom only, while the placement log
    // scanned all four — so a panel dragged into the top area was invisible to the capture and the next
    // mode flip left it there while a stale snapshot re-homed it elsewhere: the same panel in two places.
    struct SpaceLayout { JDockLayoutSnapshot left, right, bottom, top; float lw{}, rw{}, bh{}, th{}; };
    SpaceLayout editLayout, runLayout;
    bool haveEdit = false, haveRun = false;
    auto captureSpace = [&space]() -> SpaceLayout {
        return SpaceLayout{ space.left().snapshot(), space.right().snapshot(), space.bottom().snapshot(),
                            space.top().snapshot(),
                            space.leftWidth(), space.rightWidth(), space.bottomHeight(), space.topHeight() };
    };
    auto applySpace = [&space, &win](const SpaceLayout& L) {
        // A FLOATING panel is not part of any host layout, so its title must not resolve here. It used to:
        // restoring a mode's layout dragged the panel out of its own window and back into a host, and since
        // the OTHER mode's snapshot had been captured while it floated — with no entry for it at all — the
        // next flip left it in no host and no window. Floating is the user's decision; a mode change has no
        // business undoing it. That is the ONLY reason this is not the framework's default resolver.
        auto resolve = [&win](std::string_view t) -> JDockWidget* {
            JDockWidget* d = jf::JDockWidget::byTitle(t);
            return (d && win.isDockFloating(d)) ? nullptr : d;
        };
        space.left().restore(L.left,     resolve);
        space.right().restore(L.right,   resolve);
        space.bottom().restore(L.bottom, resolve);
        space.top().restore(L.top,       resolve);
        space.setLeftWidth(L.lw); space.setRightWidth(L.rw);
        space.setBottomHeight(L.bh); space.setTopHeight(L.th);
    };
    // Log where every managed dock landed after a flip, and shout if an always-docked panel went missing —
    // the whole point being that a regression like a vanishing tab shows up in the log the instant it happens.
    // EDIT-ONLY panels — the ones run mode is SUPPOSED to strip. Everything else is expected in every mode,
    // so being in no host is an orphan and gets shouted about. Stated as the short list of exceptions rather
    // than as a roll-call of every panel: this check exists to catch a panel that went missing, and a
    // roll-call cannot report a panel whose author forgot to add it here — which is precisely how the
    // Trigger Library disappeared for months with a "shout if a panel goes missing" guard already running.
    auto editOnly = [&dictionary, &widgets, &properties](JDockWidget* d) {
        return d == &dictionary || d == &widgets || d == &properties;
    };
    // Is this panel supposed to be on screen at all? Its View toggle says so. A dock with no toggle is
    // one nothing can hide, so it is always wanted.
    auto wantedOnScreen = [](JDockWidget* d) {
        const auto it = dockToggles.find(d);
        return it == dockToggles.end() || !it->second || it->second->isChecked();
    };
    auto logPlacement = [&space, &win, editOnly, wantedOnScreen](const char* phase) {
        auto hostOf = [&space](JDockWidget* d) -> const char* {
            const char* found = nullptr;
            auto scan = [&](const char* nm, JDockHost& h){ h.forEachDockPanel([&](JDockWidget* p, const JRect&, bool, int){ if (p == d) found = nm; }); };
            scan("left", space.left()); scan("right", space.right()); scan("bottom", space.bottom()); scan("top", space.top());
            return found;
        };
        for (JDockWidget* d : jf::JDockWidget::s_activeDocks) {
            if (!d) continue;
            const std::string& name = d->title();
            const char* h = hostOf(d);
            // A torn-out panel is in NO host by design, so check that BEFORE crying orphan — the warning
            // used to fire on every mode flip for each floating panel and drowned out the real thing.
            if (h)                        JLOGC("studio.dock", jf::JLogLevel::Debug) << phase << ": '" << name << "' -> " << h << " host";
            else if (win.isDockFloating(d))
                JLOGC("studio.dock", jf::JLogLevel::Debug) << phase << ": '" << name << "' floating"
                    << (win.isFloatingDockVisible(d) ? "" : " (hidden for this mode)");
            else if (!wantedOnScreen(d))  JLOGC("studio.dock", jf::JLogLevel::Debug) << phase << ": '" << name << "' hidden (closed by the user)";
            else if (!editOnly(d))        JLOGC("studio.dock", jf::JLogLevel::Warn)  << phase << ": '" << name << "' ORPHANED — a panel that belongs in every mode is in no host!";
            else                          JLOGC("studio.dock", jf::JLogLevel::Debug) << phase << ": '" << name << "' hidden (edit-only)";
        }
    };
    // Floating edit-only panels this mode change hid, so entering edit maps back exactly those — never one
    // the user closed or hid themselves.
    static std::set<jf::JDockWidget*> floatHiddenByMode;
    onSetEditMode = [&surfaceTabs, &space, &win, &dictionary, &widgets, &properties, &treeEditing, &nodeTree, &treeMenu, &treeMenuRun, applyNodeConditions,
                     &editLayout, &runLayout, &haveEdit, &haveRun, captureSpace, applySpace, logPlacement, showProps](bool edit){
        treeEditing = edit;                               // gate the tree's structural ops on edit mode
        // Entering edit shows the "New node…" add-affordance rows; leaving strips them (self-correcting rebuild).
        { JTreeViewNode r = nodeTree.root(); syncPlaceholders(r, edit); nodeTree.setRootNode(std::move(r)); }
        nodeTree.setContextMenu(edit ? &treeMenu : &treeMenuRun);   // edit block (Add/Rename/Visibility/Delete) only when editing
        // Edit-mode only: inline rename + drag-to-reorder. The tree now arms BOTH payloads on a drag —
        // an in-tree move when released inside, and an out-of-tree NodePlacement when dropped on a surface
        // (viewport) — so reordering by drag and placing by drag coexist (dual payload).
        nodeTree.setEditable(edit);
        nodeTree.setInternalReorder(edit);
        nodeTree.setDragEnabled(edit);                    // run mode is a pure navigator — no node drag at all
        surfaceTabs.setMode(edit ? Surface::Mode::Edit : Surface::Mode::Run);
        // …and the page open in the MDI, which is the one being looked at.
        pageMode = edit ? Surface::Mode::Edit : Surface::Mode::Run;
        if (pageSurface) pageSurface->setMode(pageMode);
        if (g_syncProps) g_syncProps();      // the inspector shows the page's selection, or clears in run mode
        if (edit) {
            runLayout = captureSpace(); haveRun = true;   // remember the run layout we're leaving
            if (haveEdit) { JLOGC("studio.dock", jf::JLogLevel::Info) << "-> EDIT: restoring saved edit layout"; applySpace(editLayout); }
            else          { JLOGC("studio.dock", jf::JLogLevel::Info) << "-> EDIT: first entry, adding authoring docks";
                            space.left().addDock(&dictionary); space.left().addDock(&widgets); space.right().addDock(&properties); }
        } else {
            editLayout = captureSpace(); haveEdit = true; // remember the edit layout we're leaving
            if (haveRun) { JLOGC("studio.dock", jf::JLogLevel::Info) << "-> RUN: restoring saved run layout"; applySpace(runLayout); }
            else         { JLOGC("studio.dock", jf::JLogLevel::Info) << "-> RUN: first entry, stripping authoring docks";
                           space.left().removeDock(&dictionary); space.left().removeDock(&widgets); space.right().removeDock(&properties); }
        }
        // The layout above is expressed purely in dock HOSTS, and a panel torn out into its own window is in
        // no host at all — so an undocked Dictionary/Controls/Properties survived the strip and stayed on
        // screen through run mode. Hide the float instead (it keeps its size, position and contents) and map
        // it again on the way back to edit. Only floats WE hid come back, so one closed by hand stays closed.
        for (JDockWidget* d : { &dictionary, &widgets, &properties }) {
            if (edit) {
                if (floatHiddenByMode.erase(d)) win.setFloatingDockVisible(d, true);
                continue;
            }
            if (!win.isDockFloating(d)) continue;
            // Sole occupant: unmap the window, so it returns exactly as it was. Sharing a float with panels
            // that BELONG in run mode: unmapping would take them down too, so remove just this one.
            if (win.isFloatingDockVisible(d) && win.setFloatingDockVisible(d, false)) { floatHiddenByMode.insert(d); }
            else if (jf::JDockHost* h = d->placedIn())                                { h->removeDock(d); }
        }
        logPlacement(edit ? "EDIT" : "RUN");
        if (g_syncViewToggles) g_syncViewToggles();   // the flip moved panels; the View ticks follow
        showProps(surfaceTabs.activeSurface());   // clears in run (mode-gated inside showFor)
        applyNodeConditions();                            // edit → reveal all; lock → hide condition-failing nodes
    };

    // Studio opens in RUN mode (the live instrument); editing is opt-in via the toggle. Hide the
    // authoring docks to match.
    surfaceTabs.setMode(Surface::Mode::Run);
    nodeTree.setEditable(false); nodeTree.setInternalReorder(false); nodeTree.setDragEnabled(false);   // locked navigator until Editing is on
    space.left().removeDock(&dictionary); space.left().removeDock(&widgets);
    space.right().removeDock(&properties);
    showProps(surfaceTabs.activeSurface());

    // --- ECU pipeline ---------------------------------------------------------------------------
    // Inbound: telemetry -> decoded channel values; the full tune image -> value store + baseline;
    // a targeted re-read chunk -> spliced into the store.
    link.telemetryFrame.connect([](const std::vector<uint8_t>& f) {
        if (s_upgrade.active()) s_upgrade.onTelemetry(f);
        Cache::instance().ingestTelemetry(f);
    });
    link.configImageReady.connect([&win, &connectBtn, saveActiveTune, ensureLocalTuneLoaded](const std::vector<uint8_t>& img) {
        if (s_upgrade.onConfigImage(img)) return;          // read by the firmware upgrade, not a connect
        Cache& c = Cache::instance();

        // Manual "Verify ↔ ECU": byte-diff the fresh read against the cache (the source of truth,
        // including any unflushed local edits) and re-push every divergent run — on-demand insurance
        // against post-ack corruption. Skips connect flow.
        if (s_verifyPending) {
            s_verifyPending = false;
            const std::vector<uint8_t>& local = c.configImage();
            size_t repushed = 0;
            for (size_t i = 0; i < local.size() && i < img.size();) {
                if (local[i] == img[i]) { ++i; continue; }
                size_t j = i;
                while (j < local.size() && j < img.size() && local[j] != img[j]) ++j;
                link.writeConfig(static_cast<int>(i), std::vector<uint8_t>(local.begin() + i, local.begin() + j));
                repushed += j - i;
                i = j;
            }
            win.showStatus(repushed ? "Verify: re-pushed " + std::to_string(repushed) + " byte(s) to the ECU"
                                    : "Verify: ECU matches the project tune", 5000);
            return;
        }

        // Connect flow. The ECU's as-connected image is ALWAYS the restore baseline…
        c.setBaseline(img);
        connectBtn.setConnState(ConnectButton::State::Connected);   // tune in — all good, go green
        // AUTOLOGGING starts here rather than at port-open: the meta is what names the columns, and
        // until the tune is in there is no definition to write a header from.
        if (DatalogRecorder::autoLog() && !DatalogRecorder::instance().recording()) {
            std::string why;
            if (DatalogRecorder::instance().start(&why)) { refreshRecBtn();
                win.showStatus("Recording \xE2\x80\x94 " + DatalogRecorder::instance().path(), 4000); }
            else JLOGC("model.datalog", jf::JLogLevel::Warn) << "autolog did not start: " << why;
        }
        JLOGC("ui.connect", jf::JLogLevel::Info) << "ECU image received (" << img.size() << "B); reconciling";

        // …and is archived as a timestamped restore point named after the active tune (crash-safe
        // history; the newest one seeds the offline baseline when the project reopens).
        if (ecu && !activeTuneName.empty()) {
            namespace fs = std::filesystem;
            std::error_code ec;
            fs::create_directories(ecu->restoreDir(), ec);
            std::time_t now = std::time(nullptr);
            char stamp[24]; std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H.%M", std::localtime(&now));
            std::ofstream rf(fs::path(ecu->restoreDir()) / (activeTuneName + " " + stamp + ".tune"), std::ios::binary);
            if (rf) rf.write(reinterpret_cast<const char*>(img.data()), std::streamsize(img.size()));
        }

        // Reconcile — the SHARED routine (see reconcileTune): a jayecu ECU and a TunerStudio one ask the
        // user exactly the same question, in the same words, with the same three answers.
        const MetaModel* m = c.meta();
        // THE STUDIO'S SIDE, FETCHED BEFORE THE QUESTION IS ASKED. The reconcile decides which of two
        // tunes survives, so the studio must be holding one: on a cold connect the cache is empty and
        // the saved tune is read off disk here (see ensureLocalTuneLoaded). This used to pass whatever
        // happened to be in memory, which on the Connect-from-launch path was nothing at all.
        if (m) ensureLocalTuneLoaded(ecu, *m);
        // An image loaded for a DIFFERENT ECU is nothing to protect, so it is not offered as a candidate.
        const std::vector<uint8_t> candidate = (ecu && s_imageUid == ecu->uid()) ? c.configImage()
                                                                                 : std::vector<uint8_t>{};
        // The battery-backed learned region is not tune data, so it is pulled on every path — but only
        // once the config block is settled, so its bindings read live over the same r/w path.
        auto pullLearned = [] {
            Cache& cc = Cache::instance();
            JLOGC("ui.connect", jf::JLogLevel::Info) << "config settled (" << cc.configImage().size()
                << "B); allocating + pulling non-tune cache blocks";
            cc.initSegments();
            cc.requestSegmentReads();
        };
        reconcileTune(win, activeTuneName, candidate, img, m, ecu,
            [&win, &connectBtn, img, candidate, m, saveActiveTune, pullLearned](JTuneSync d) {
                switch (d) {
                    case JTuneSync::NothingLocal:
                        JLOGC("ui.connect", jf::JLogLevel::Info) << "no comparable local tune \xE2\x86\x92 adopting ECU image ("
                            << img.size() << "B)";
                        Cache::instance().setConfigImage(img);
                        if (ecu) s_imageUid = ecu->uid();
                        pullLearned();
                        win.showStatus("Tune loaded (" + std::to_string(img.size()) + " bytes)", 4000);
                        break;
                    case JTuneSync::InSync:   // keep the local image: identical by dictionary, and it is ours
                        JLOGC("ui.connect", jf::JLogLevel::Info) << "in sync \xC2\xB7 keeping local tune '" << activeTuneName << "'";
                        pullLearned();
                        win.showStatus("\xE2\x97\x8F In sync \xC2\xB7 " + activeTuneName, 4000);
                        break;
                    case JTuneSync::Pull:
                        Cache::instance().setConfigImage(img);
                        if (ecu) s_imageUid = ecu->uid();
                        saveActiveTune();
                        pullLearned();
                        win.showStatus("Pulled the ECU image into \"" + activeTuneName + "\"", 4000);
                        break;
                    case JTuneSync::Push:
                        link.writeConfigImage(candidate);
                        pullLearned();
                        win.showStatus("Pushing \"" + activeTuneName + "\" to the ECU \xE2\x80\xA6", 3000);
                        break;
                    case JTuneSync::PushDefaults:
                        // The firmware's default tune, in the studio and on the ECU, and saved as this ECU's
                        // tune — the ECU had none, so there is nothing of the user's for it to replace.
                        Cache::instance().setConfigImage(m->defaultImage());
                        if (ecu) s_imageUid = ecu->uid();
                        link.writeConfigImage(m->defaultImage());
                        saveActiveTune();
                        pullLearned();
                        win.showStatus("Putting the default tune on the ECU \xE2\x80\xA6 then Burn and Reset ECU", 6000);
                        break;
                    case JTuneSync::Cancel:
                        // No session. The link goes down with neither tune written, and the learned region
                        // is left alone too — there is nothing live to bind it to.
                        link.close();
                        connectBtn.setConnState(ConnectButton::State::Idle);
                        s_connTitle.clear();
                        win.setStatusText("Connection cancelled \xC2\xB7 nothing changed");
                        break;
                }
            });
    });
    link.configRangeReady.connect([](int off, const std::vector<uint8_t>& b) {
        Cache::instance().applyConfigRange(off, b);
    });

    // The identity handshake (sent on open) confirms the ECU: route to its folder, load the
    // dashboard node tree, size the link from the meta, and pull the full tune image.
    // The ECU's clock, corrected by EcuLink the moment the handshake lands. Reported HERE, in the
    // connect narrative, rather than the status bar: the RTC reply arrives after "Connected: ..." is
    // shown, so writing it there would replace the one line the user is waiting for. This is where
    // someone asking "why are my SD log timestamps wrong" looks.
    link.rtcSynced.connect([&](bool hadClock, const std::string& msg) {
        JLOGC("ui.connect", hadClock ? jf::JLogLevel::Info : jf::JLogLevel::Warn)
            << msg << (hadClock ? "" : " — SD log files and learned totems will carry no usable date");
    });

    // THE IDENTITY HANDLER IS A STORED FUNCTION, not a lambda handed straight to the signal, so the
    // firmware question can PAUSE it: asked before anything is loaded, and the connect carries on from
    // the same point once it is answered.
    s_onIdentity = [&](const std::string& sig) {
        if (s_upgrade.onIdentity(sig)) return;           // the ECU coming back mid-upgrade
        // identity = "jayecu <board> <version> <build> <layout_hash> <device_uid> t<telemetry_size>"
        const auto p = splitOn(sig, ' ');
        const std::string board = p.size() > 1 ? p[1] : std::string();
        const std::string uid   = p.size() > 5 ? p[5] : std::string();
        s_ecuBoard     = board;
        s_ecuFwVersion = p.size() > 2 ? p[2] : std::string();
        s_ecuFwBuild   = p.size() > 3 ? p[3] : std::string();
        s_ecuLayout    = p.size() > 4 ? p[4] : std::string();
        // NEWER FIRMWARE? Asked HERE, before the meta, the dashboard or the tune are loaded: if the
        // answer is to upgrade, none of that belongs to the firmware the ECU is about to run. Asked
        // once per connection — the answer re-enters this handler, which then goes straight past.
        if (!s_firmwareAsked) {
            s_firmwareAsked = true;
            if (s_askFirmware(sig, [sig] { s_onIdentity(sig); })) return;
        }

        // THE FRAME SIZE IS PART OF THE IDENTITY, and it is checked here because layout_hash cannot
        // carry it: the hash covers the CONFIG byte layout, so adding a telemetry channel leaves it
        // untouched while every channel after the new one shifts in the frame. A meta one build out of
        // step then decodes the whole frame off-by-one and shows plausible rubbish — the failure mode
        // that put a 786-byte ECU behind a 787-byte meta with an unchanged 5431b57e. Say so loudly
        // rather than draw it.
        if (p.size() > 6 && p[6].size() > 1 && p[6][0] == 't') {
            const int ecuTelem = std::atoi(p[6].c_str() + 1);
            const int metaTelem = Cache::instance().meta() ? Cache::instance().meta()->telemetrySize() : 0;
            if (ecuTelem > 0 && metaTelem > 0 && ecuTelem != metaTelem) {
                JLOGC("ui.connect", jf::JLogLevel::Warn)
                    << "TELEMETRY SIZE MISMATCH: ECU sends " << ecuTelem << "B, this meta expects "
                    << metaTelem << "B — every channel past the difference would decode from the wrong "
                    << "offset. Re-run codegen and push the meta (make flash), or flash the firmware "
                    << "this meta was built for.";
                win.showStatus("Telemetry mismatch: ECU " + std::to_string(ecuTelem) + "B vs meta "
                               + std::to_string(metaTelem) + "B — flash and push the meta together", 15000);
            }
        }
        JLOGC("ui.connect", jf::JLogLevel::Info) << "identity: board=" << board
            << " layout=" << (p.size() > 4 ? p[4] : std::string("?"))
            << " uid=" << (uid.size() > 6 ? uid.substr(uid.size() - 6) : uid) << " \xE2\x86\x92 pulling tune image";
        if (!uid.empty()) {
            const std::string prevUid = ecu ? ecu->uid() : std::string();
            // Switching to this ECU reloads its saved layout from disk, discarding the LIVE one. That's a
            // destructive document swap, so honour unsaved changes exactly like Close Project / Open Layout
            // do (through maybeSaveThen). And a reconnect to the SAME ECU keeps the live layout untouched —
            // nothing to switch, nothing to reload, so no prompt and no clobber (the reconnect-clobber bug).
            auto applyIdentity = [&, uid, board]{
                Ecu* e = Ecu::openOrCreate(uid, board);
                if (!e) return;
                e->setLastPort(s_openedPort);
                // THE shared adopt — the same one the TunerStudio path and Open Project use. It owns the
                // order (project document, tree, expansion prefs), the dirty prompt and the same-ECU case,
                // so this path no longer carries its own copy of any of them.
                if (g_adoptConnectedEcu) g_adoptConnectedEcu(e, nullptr);
            };
            // No same-ECU or dirty branching here any more: the adopt owns both, identically for every
            // protocol. This used to be three cases in one path and none in the other.
            {
                applyIdentity();
            }
        }
        win.setStatusText("Connected: " + (board.empty() ? sig : board) +
                          (uid.empty() ? "" : "  \xC2\xB7  " + uid));
        // Connected identity — folded into the window title by the housekeeping timer ("<tune>[*] — studio — <this>").
        { const std::string ver = p.size() > 2 ? p[2] : std::string(), hsh = p.size() > 4 ? p[4] : std::string();
          s_connTitle = board + (ver.empty() ? "" : " " + ver) + (hsh.empty() ? "" : " (" + hsh + ")"); }
        // Resolve the schema from the ECU's layout hash:
        // load the matching library .meta + rebuild the dict. When NOTHING matches, PROMPT the user to pick a
        // schema (the dialog's fallback) instead of silently connecting against the last/wrong meta.
        namespace fs = std::filesystem;
        const std::string hash = p.size() > 4 ? p[4] : std::string();
        const fs::path lib = StudioPaths::dataDir("meta");

        // Size the link from the settled meta + pull the tune image. Reading config is a READ, never a write.
        auto finishConnect = [&win]() {
            if (meta.configSize() == 0) { win.showStatus("No ECU definition loaded — Library ▸ Load ECU Definition… to load one", 4000); return; }
            Cache::instance().setMeta(&meta);
            // The SHIPPED wheel library travels with the firmware, in the meta. Refresh it on every
            // meta load so the wheels on offer are the ones THIS ECU can decode — they used to be
            // compiled into the studio, which made them a property of the build instead.
            triggerLib.setShippedWheels(wheelsFromMetaFile(meta.path()));
            link.setConfigSize(meta.configSize());
            link.setBlockSize(meta.blockSize());
            link.readConfigImage();
        };
        auto loadSchema = [&win, &dictTree, finishConnect, noteTsProto](const std::string& path) {
            if (meta.loadFile(path)) {
                noteTsProto(path);
                // The schema replaced config_ — including the project's host variables, if the document
                // was loaded first (it is, on every connect). Put them back before anything reads a
                // "pc." path, or a widget bound to one resolves to nothing for the rest of the session.
                if (g_applyProjectPcVars) g_applyProjectPcVars();
                dictTree.setRootNode(buildDictionary(meta));
                if (g_checkLayoutBindings) g_checkLayoutBindings();   // the document came before the schema
                // The schema has landed — which on a first connect is the first moment a navigation tree
                // exists to seed from. No-op when the ECU already has a project document.
                // The schema is also the first moment the shipped dashboard can be NAMED — the file on
                // the card is "<board> <layout_hash>.gui", and until now there was no hash. Seeding is
                // the fallback, not the opening move.
                if (!(g_fetchShippedDash && g_fetchShippedDash()))
                    if (g_seedNavFromMeta) g_seedNavFromMeta();
                win.showStatus("Schema: " + std::filesystem::path(path).stem().string(), 2500);
            } else if (!meta.needsStudio().empty()) {
                // Nothing in this studio can read that firmware's layout safely: say so and let go of
                // the ECU, rather than connect to it against a description it does not understand.
                tellNeedsStudio(meta.needsStudio());
                link.close();
                return;
            }
            finishConnect();
        };

        std::error_code ec; std::string matchPath;
        if (!hash.empty())
            for (const auto& e : fs::directory_iterator(lib, ec))
                if (e.path().extension() == ".meta" && e.path().stem().string().find(hash) != std::string::npos) { matchPath = e.path().string(); break; }
        if (!matchPath.empty()) { loadSchema(matchPath); return; }   // auto-resolved — the common path

        // Pool miss. Picker fallback (Browse-equivalent: the library list; Open Meta… covers arbitrary
        // paths) — used directly when we can't name the SD file, or after a failed SD fetch.
        // Not in the library, not on the card: the release that shipped this firmware, before asking.
        // Its dashboard comes with it, into the library under the layout's name, so the dashboard step
        // that follows the schema finds it there. Only when there is no such release (a development
        // build), or no network, is the user asked for the file.
        auto askForMeta = [&win, hash, lib, loadSchema] {
            std::error_code ec2;
            std::vector<std::string> labels, paths;
            for (const auto& e : fs::directory_iterator(lib, ec2))
                if (e.path().extension() == ".meta") { labels.push_back(e.path().stem().string()); paths.push_back(e.path().string()); }
            // Nothing in the library either (a clean install, or the SD fetch failed): ASK for the file
            // rather than naming a menu item and leaving the user connected to an ECU nothing describes.
            if (paths.empty()) {
                win.showStatus("No schema for this ECU (hash " + hash + ") \xE2\x80\x94 choose its meta", 5000);
                jf::JDialog::openFile("Open meta for hash " + hash, { "meta", "json" },
                                      [loadSchema](std::string path) { loadSchema(path); });
                return;
            }
            win.showStatus("Unknown ECU (hash " + hash + ") — choose its schema", 4000);
            win.openModal<ListPickerDialog>(std::string("Resolve schema \xC2\xB7 hash " + hash), labels, false,
                [paths, loadSchema](int idx, std::string) { if (idx >= 0 && idx < (int)paths.size()) loadSchema(paths[idx]); });
        };
        s_metaFetchFallback = [&win, hash, lib, board, loadSchema, askForMeta] {
            const std::string ver = s_ecuFwVersion;
            if (hash.empty() || board.empty() || ver.empty()) { askForMeta(); return; }
            win.openModal<jf::JProgressDialog>(std::string("Fetching schema"),
                                               board + " " + ver + " from its firmware release");
            fwfetch::fetchVersion(board, ver, hash, true, true,
                [&win, hash, lib, board, ver, loadSchema, askForMeta](const fwfetch::VersionFiles& r) {
                    if (auto* d = jf::JProgressDialog::active()) d->dismiss();
                    if (!link.isOpen() || s_ecuLayout != hash) return;          // disconnected meanwhile
                    if (r.meta.empty()) {
                        JLOGC("ui.connect", jf::JLogLevel::Info) << "no released schema for " << board << " " << ver
                                                                 << ": " << r.error;
                        askForMeta();
                        return;
                    }
                    std::error_code ec; std::filesystem::create_directories(lib, ec);
                    const std::string dest = (lib / (board + " " + hash + ".meta")).string();
                    { std::ofstream f(dest, std::ios::binary);
                      f.write(reinterpret_cast<const char*>(r.meta.data()), std::streamsize(r.meta.size())); }
                    if (!r.dashboard.empty()) {
                        const auto dd = exactDashboard(board, hash);
                        std::filesystem::create_directories(dd.parent_path(), ec);
                        std::ofstream f(dd, std::ios::binary);
                        f.write(reinterpret_cast<const char*>(r.dashboard.data()), std::streamsize(r.dashboard.size()));
                    }
                    win.showStatus("Schema for " + board + " " + ver + " downloaded from its release", 5000);
                    loadSchema(dest);
                });
        };
        // Try the ECU's own SD first (the firmware writes "<board> <hash>.meta" there): take the card,
        // fetch, install into the library under the canonical name, release, connect. The app-level
        // fileReceived / fileError handlers route by s_pendingMetaFetch; progress shows via the normal
        // SD transfer status line, non-modal.
        s_metaFetchInstall = [loadSchema, lib](const std::string& name, const std::vector<uint8_t>& data) {
            std::error_code ec3; fs::create_directories(lib, ec3);
            const std::string dest = (lib / name).string();
            std::ofstream f(dest, std::ios::binary);
            if (f) f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
            loadSchema(dest);
        };
        if (!hash.empty() && !board.empty()) {
            s_pendingMetaFetch = board + " " + hash + ".meta";
            // A MODAL, not a status line. This is 1.27 MB over a 115200 link — about ninety seconds in
            // which the studio can draw nothing, because nothing can be interpreted before the schema
            // arrives. A counter ticking in the status bar is the same place a tooltip goes: easy to miss
            // entirely, and indistinguishable from a hang. setModalDialog renders from the main frame
            // loop, so the link keeps polling underneath and the transfer it reports on continues.
            win.showStatus("Schema not in the library — fetching " + s_pendingMetaFetch + " from the ECU's SD \xE2\x80\xA6", 4000);
            win.openModal<jf::JProgressDialog>(std::string("Fetching schema"),
                                                  s_pendingMetaFetch + " from the ECU's SD card");
            link.sdMcu();
            link.fetchFile(s_pendingMetaFetch);
        } else {
            s_metaFetchFallback();
        }
    };
    link.identityReceived.connect([](const std::string& sig) { s_onIdentity(sig); });
    link.openedChanged.connect([&win, &connectBtn](bool open) {
        JLOGC("ui", jf::JLogLevel::Info) << "[link] " << (open ? "OPENED (connect)" : "CLOSED (disconnect)")
                                         << " | g_docDirty=" << g_docDirty;
        if (open) {
            s_firmwareAsked = false;                                    // a new connection gets asked afresh
            win.showStatus("Link open — waiting for ECU identity…");   // transient until identity
        } else {
            connectBtn.setConnState(ConnectButton::State::Idle);
            win.setStatusText("Disconnected");
            s_connTitle.clear();   // the housekeeping timer reverts the title
        }
    });
    link.errorOccurred.connect([&win, &connectBtn](const std::string& m) {
        // The native attempt failed. If it failed while CONNECTING — no jayecu ECU answered the identity
        // request — that is the moment the TunerStudio fallback gets its turn, and only then: this studio
        // looks for its own ECUs first, and a drop on an established link must never quietly re-open it on
        // some other protocol.
        if (connectBtn.connState() == ConnectButton::State::Connecting && tsFallbackAllowed()) {
            link.close();                                           // free the port before TS opens it
            if (g_tryTsConnect && g_tryTsConnect()) return;          // connected as TS: not an error at all
        }
        connectBtn.setConnState(ConnectButton::State::Error);       // red on failure/drop
        win.setStatusText("Disconnected");
        win.showStatus("Link error: " + m, 5000);                  // transient notification
    });

    // --- Left-side status messages ------------------------------------------------------------
    // linkActivity: retry / SD-request progress + terminal failures surfaced by the resolve pool.
    link.linkActivity.connect([&win](const std::string& m, bool isError) {
        win.showStatus(m, isError ? 6000 : 3000);
    });
    // A queued config write exhausted its retries, or the ECU rejected it. THE TWO MUST NOT BE LEFT
    // DISAGREEING ABOUT THE TUNE.
    //
    // This used to say so — "the RAM tune may be out of step with the ECU" — in a status line that
    // cleared itself after six seconds, and then leave it out of step. The studio went on showing a
    // number the ECU does not have: every control bound to those bytes, every condition that reads
    // them, and every table readout interpolated from them (Cache::solveTable reads the LOCAL image),
    // all confidently wrong, with nothing on screen saying which bytes were lying.
    //
    // So read that exact range back and let the ECU settle it. The bytes are no longer pending by now —
    // dirty_ is cleared at flush(), when the write goes to the link, not when it is acked — so the
    // splice is clean; and if the user has edited the same field SINCE, that edit is dirty again and
    // applyConfigRange's dirty-wins rule keeps it, which is the right answer both ways round.
    //
    // With the link down there is nothing to ask, and that is the one case worth a longer, blunter
    // message: the local tune may differ from the ECU and the studio cannot find out.
    link.writeFailed.connect([](int, int) { s_upgrade.onWriteFailed(); });
    link.writeFailed.connect([&win](int off, int size) {
        const std::string where = std::to_string(off) + " (" + std::to_string(size) + " B)";
        if (link.isOpen()) {
            link.readConfigRange(off, size);
            win.showStatus("ECU write failed at " + where + " \xE2\x80\x94 reading the ECU's value back", 6000);
        } else {
            win.showStatus("ECU write failed at " + where +
                           " \xE2\x80\x94 link down, the tune may differ from the ECU", 10000);
        }
    });
    // SD file transfer feedback: live progress (held until the next line), then the result.
    link.fileTransferProgress.connect([&win](const std::string& name, int64_t bytes, int64_t total) {
        if (s_upgrade.onFileProgress(name, bytes, total)) return;
        char b[160];
        std::snprintf(b, sizeof(b), "%s: %lld / %lld KB\xE2\x80\xA6", name.c_str(),
                      static_cast<long long>(bytes / 1024), static_cast<long long>(total / 1024));
        win.showStatus(b);   // ms == 0: hold until the next progress line / result replaces it
        if (auto* d = jf::JProgressDialog::active())
            d->setProgress(static_cast<long long>(bytes), static_cast<long long>(total));
    });
    link.fileReceived.connect([&win](const std::string& name, const std::vector<uint8_t>& data) {
        if (!s_pendingMetaFetch.empty() && name == s_pendingMetaFetch) {   // the awaited SD schema
            s_pendingMetaFetch.clear();
            link.sdRelease();
            if (auto* d = jf::JProgressDialog::active()) d->setNote("installing\xE2\x80\xA6");
            // Installing the schema is ALSO what discovers whether a dashboard has to come off the card:
            // loadSchema asks for one. So the bar is taken down AFTER that, and only if nothing followed
            // it — one bar for one wait, however many files that wait turns out to be.
            if (s_metaFetchInstall) s_metaFetchInstall(name, data);
            if (s_pendingDashFetch.empty())
                if (auto* d = jf::JProgressDialog::active()) d->dismiss();
            win.showStatus("Schema installed from the ECU's SD: " + name, 4000);
            return;
        }
        if (!s_pendingDashFetch.empty() && name == s_pendingDashFetch) {   // the awaited SD dashboard
            s_pendingDashFetch.clear();
            link.sdRelease();
            if (auto* d = jf::JProgressDialog::active()) { d->setNote("installing\xE2\x80\xA6"); d->dismiss(); }
            if (s_dashFetchInstall) s_dashFetchInstall(name, data);
            if (g_loadProjectDoc) g_loadProjectDoc();   // straight to the real document
            win.showStatus("Dashboard installed from the ECU's SD: " + name, 4000);
            return;
        }
        win.showStatus(name + " fetched", 4000);
    });
    link.sdStatusReceived.connect([](uint8_t state) { s_upgrade.onSdStatus(state); });
    link.fileWritten.connect([&win](const std::string& name) {
        if (s_upgrade.onFileWritten(name)) return;           // the firmware update's copy to the card
        win.showStatus(name + " written to SD", 4000);
    });
    link.fileError.connect([&win](const std::string& name, const std::string& msg) {
        if (s_upgrade.onFileError(name, msg)) return;
        if (!s_pendingMetaFetch.empty() && name == s_pendingMetaFetch) {   // SD has no schema → picker
            s_pendingMetaFetch.clear();
            link.sdRelease();
            // Close the progress modal FIRST: the fallback opens a picker, and stacking one over a dialog
            // that is reporting a transfer which has already failed reads as two things going wrong.
            if (auto* d = jf::JProgressDialog::active()) d->dismiss();
            win.showStatus("No schema on the ECU's SD (" + msg + ")", 4000);
            if (s_metaFetchFallback) s_metaFetchFallback();
            return;
        }
        if (!s_pendingDashFetch.empty() && name == s_pendingDashFetch) {
            // NOT AN ERROR THE USER NEEDS A DIALOG ABOUT. No dashboard on the card means an ECU that
            // was flashed without one; the tree seeded from the meta is already on screen and the
            // studio is entirely usable. Say it once, quietly, and leave them to it.
            s_pendingDashFetch.clear();
            link.sdRelease();
            JLOGC("ui.lazy", jf::JLogLevel::Info)
                << "no dashboard on the ECU's SD (" << name << "): " << msg;
            if (s_dashFallback) s_dashFallback();
            return;
        }
        win.showStatus("SD file error: " + name + " \xE2\x80\x94 " + msg, 6000);
    });

    // Outbound: value-store edits -> ECU config writes / CLI commands / scoped re-reads.
    Cache::instance().writeRequested.connect([](int off, const std::vector<uint8_t>& b) {
        link.writeConfig(off, b);
    });
    Cache::instance().cliRequested.connect([](const std::string& cmd) {
        link.sendCli(cmd);
    });
    Cache::instance().rangeReadRequested.connect([](int off, int size) {
        link.readConfigRange(off, size);
    });
    // A bench routine (findlimits/fillff/autotune/pedalcal) reported done via the command_state telemetry
    // channel — toast the result. On OK, Cache has already kicked the config re-read that pulls the new cal.
    Cache::instance().commandFinished.connect([&win](int op, bool ok) {
        static const char* kName[] = { "command", "ETB0 findlimits", "ETB1 findlimits", "ETB0 fillff",
                                       "ETB1 fillff", "ETB0 autotune", "ETB1 autotune", "pedal calibrate" };
        const std::string n = (op >= 1 && op <= 7) ? kName[op] : "command";
        win.showStatus(n + (ok ? " complete \xE2\x80\x94 syncing from ECU" : " failed"), 5000);
    });

    // STUDIO_GALLERY: place one of every control type on a surface (no landing, no comms) so the rendered
    // appearance of all controls can be captured + diffed in one shot.
    if (std::getenv("STUDIO_GALLERY")) {
        win.setCentralWidget(&mdi);
        surfaceTabs.newSurface("Gallery");
        if (auto* s = surfaceTabs.activeSurface()) if (auto* m = s->model()) {
            struct G { const char* type; const char* bind; };
            const G items[] = {
                {"field","rpm"}, {"gauge","rpm"}, {"dial","rpm"}, {"needle","rpm"},
                {"scale","rpm"}, {"checkbox","can.obd_enabled"}, {"slider","electronic_throttle.etb[0].min_tps_pct"},
                {"radio","electronic_throttle.etb[0].tps_a_src"}, {"enum","electronic_throttle.etb[0].tps_a_src"},
                {"configedit","electronic_throttle.etb[0].min_tps_pct"}, {"command",""}, {"settingselector",""},
            };
            float x = 20.f, y = 20.f; int col = 0;
            for (const auto& it : items) {
                const bool big = std::string(it.type) == "table" || std::string(it.type) == "curve";
                std::unordered_map<std::string, std::string> p;
                if (it.bind[0]) p["signalName"] = it.bind;
                m->add(it.type, x, y, big ? 260.f : 200.f, big ? 150.f : 66.f, p);
                x += 290.f; if (++col % 4 == 0) { x = 20.f; y += 175.f; }
            }
        }
    }

    // AI bus (JF_AI_BUS): value actions the generic click/focus dispatch can't do — set_value:<n> and
    // select:<label> need per-widget-type knowledge. Look the target up by node id in the live widget tree
    // and drive its setter, which fires the SAME change signal as a user edit (so bound models react, e.g.
    // the trigger designer's Cams count rebuilds its per-cam rows). Returns 1 handled / 0 not / -1 bad id.
    // Only ever invoked while the bus is enabled + ticking on the main thread, so the lookup is safe.
    jf::JAiBus::instance().onAction = [&win](uint32_t id, const std::string& action) -> int {
        // "select_node:Configuration/Engine Configuration/Trigger System" — go to a page, by the path the
        // tree knows it by. A page is reached by clicking a row in a tree that scrolls, filters and hides
        // whatever its conditions hide, so driving to one through the pointer means knowing where that row
        // happens to be this second. The path is what the page IS; this is the same call a link label
        // makes, which is why it lands the tree selection and the viewport together.
        if (action.compare(0, 12, "select_node:") == 0) {
            const std::string path = action.substr(12);
            if (path.empty() || !hyperlink::navigator()) return -1;
            hyperlink::navigator()(path);
            return 1;
        }
        // "context_menu:X,Y" — open the target widget's context menu at a window-relative point, the way
        // a right-click does. It exists so MENU PLACEMENT is testable: a menu that runs off the bottom of
        // the screen cannot be reproduced through the widget tree (the popup is a separate window and its
        // position is the thing under test), and driving a right-click through the pointer needs a window
        // manager the headless harness does not have.
        if (action.compare(0, 13, "context_menu:") == 0) {
            // The BUS's id, not the scene-graph node id: graph ids repeat across docks and surfaces, so
            // this used to resolve to whichever widget of that number came first (see JAiBus::idFor).
            jf::JWidget* t = jf::JAiBus::instance().widgetFor(id);
            if (!t) return -1;
            float mx = 0.f, my = 0.f;
            if (std::sscanf(action.c_str() + 13, "%f,%f", &mx, &my) != 2) return -1;
            t->prepareContextMenu(mx, my);                  // same order the right-click path uses
            if (!t->contextMenu() || !jf::JMenuManager::instance().onOpenMenu) return 0;
            jf::JMenuManager::instance().onOpenMenu(t->contextMenu(),
                                                    win.window().screenX() + static_cast<int>(mx),
                                                    win.window().screenY() + static_cast<int>(my),
                                                    false, /*pointAnchored=*/true);
            return 1;
        }
        // "submenu:INDEX,X,Y" — open the INDEXth item's SUBMENU directly, at a point of your choosing.
        // A submenu opens on hover, over a popup window, which no bus action can drive; and the submenu
        // is where a long menu actually runs off the screen (its parent is short and places fine). This
        // puts the tall popup at a chosen anchor so its PLACEMENT can be read off the log.
        if (action.compare(0, 8, "submenu:") == 0) {
            // The BUS's id, not the scene-graph node id: graph ids repeat across docks and surfaces, so
            // this used to resolve to whichever widget of that number came first (see JAiBus::idFor).
            jf::JWidget* t = jf::JAiBus::instance().widgetFor(id);
            if (!t) return -1;
            int idx = 0; float mx = 0.f, my = 0.f;
            if (std::sscanf(action.c_str() + 8, "%d,%f,%f", &idx, &mx, &my) != 3) return -1;
            t->prepareContextMenu(mx, my);
            jf::JMenu* m = t->contextMenu();
            if (!m || !jf::JMenuManager::instance().onOpenMenu) return 0;
            int seen = 0;
            for (const auto& it : m->items()) {
                auto* mi = dynamic_cast<jf::JMenuItem*>(it.get());
                if (!mi || !mi->submenu()) continue;
                if (seen++ != idx) continue;
                jf::JMenuManager::instance().onOpenMenu(mi->submenu(),
                                                        win.window().screenX() + static_cast<int>(mx),
                                                        win.window().screenY() + static_cast<int>(my),
                                                        false, /*pointAnchored=*/true);
                return 1;
            }
            return 0;
        }
        jf::JWidget* w = jf::JAiBus::instance().widgetFor(id);   // the bus's id space, not the graph's
        if (!w) return -1;
        auto suffix = [&](const char* pfx) -> const char* {
            const size_t n = std::strlen(pfx);
            return (action.size() >= n && action.compare(0, n, pfx) == 0) ? action.c_str() + n : nullptr;
        };
        if (const char* v = suffix("set_value:")) {
            try {
                if (auto* s = dynamic_cast<jf::JSpinBox*>(w))       { s->setValue(static_cast<int>(std::lround(std::stod(v)))); return 1; }
                if (auto* d = dynamic_cast<jf::JDoubleSpinBox*>(w)) { d->setValue(std::stod(v)); return 1; }
                if (auto* b = dynamic_cast<jf::JComboBox*>(w))      { b->setCurrentIndex(static_cast<int>(std::lround(std::stod(v)))); return 1; }
                if (auto* k = dynamic_cast<jf::JCheckBox*>(w))      { k->setChecked(std::stod(v) != 0.0); return 1; }
            } catch (...) { return -1; }   // non-numeric argument
            return 0;                      // a widget type with no numeric value
        }
        if (const char* v = suffix("select:")) {   // combo box by option label
            if (auto* b = dynamic_cast<jf::JComboBox*>(w)) {
                const auto& items = b->items();
                for (int i = 0; i < static_cast<int>(items.size()); ++i)
                    if (items[i] == v) { b->setCurrentIndex(i); return 1; }
            }
            return 0;
        }
        return 0;
    };

    // Profiling: JF_LOG=perf=info gives a summary every 120 frames — ms/frame, where the build time went,
    // and how much per-frame work (expression evaluations, cache reads) the pages are doing.
    if (Perf::enabled()) {
        win.onFrameTiming = [](const jf::JAppWindow::JFrameTiming& t) {
            Perf::instance().add("frame: BUILD (widget tree -> primitives)", t.buildMs);
            Perf::instance().add("frame: SUBMIT (GPU + present)", t.submitMs);
            Perf::instance().count("draw commands", (long long)t.drawCommands);
            Perf::instance().frame(t.totalMs);
        };
    }
    // THE TREE IS THE TAB THE STUDIO OPENS ON. Navigation and the Trigger Library share the left area,
    // and the active tab is whichever was added LAST — which is the library, for no better reason than
    // the order the docks happen to be declared in. What a studio opens on should be the thing you
    // navigate with, not the wheel editor.
    //
    // Raised HERE rather than at the declaration: run mode strips the authoring docks out of that area
    // on first entry, and this is after the initial mode has settled. A dock already in the host takes
    // the cheap path through insertDock — it is made the active tab and nothing moves.
    space.left().addDock(&tree);
    bootMark("entering the run loop");
    const int rc = win.run();
    // The document (tree + surface + node pages) is NOT auto-saved on exit — it's governed entirely by the
    // unsaved-changes prompt (Save routes to saveAll; Discard leaves dashboard.gui untouched).
    // Only UI PREFERENCES persist here — window geometry, dock visibility, tree expansion —
    // which aren't document content (discarding your edits shouldn't forget where your window was).
    settings.set("window.w", static_cast<int>(win.width()));  settings.set("window.h", static_cast<int>(win.height()));
    settings.set("window.x", win.windowX());                  settings.set("window.y", win.windowY());
    // Save each dock-area size ONLY while its host is enabled — a disabled host is collapsed to 0, and
    // persisting that would lose the size to restore it to when re-enabled.
    if (settings.get<bool>("dock.leftEnabled",   true)) settings.set("dock.leftW",   static_cast<int>(space.leftWidth()));
    if (settings.get<bool>("dock.rightEnabled",  true)) settings.set("dock.rightW",  static_cast<int>(space.rightWidth()));
    if (settings.get<bool>("dock.bottomEnabled", true)) settings.set("dock.bottomH", static_cast<int>(space.bottomHeight()));
    { std::string exp; for (const auto& c : nodeTree.root().children) collectExpanded(c, "", exp); settings.set("tree.expanded", jf::JVariant(exp)); }
    settings.set("dock.tree",        tgTree->isChecked()    ? 1 : 0);
    settings.set("dock.properties",  tgProps->isChecked()   ? 1 : 0);
    settings.set("dock.dictionary",  tgDict->isChecked()    ? 1 : 0);
    settings.set("dock.widgets",     tgWidgets->isChecked() ? 1 : 0);
    settings.set("dock.statusLamps", tgStatus->isChecked()  ? 1 : 0);
    settings.set("dock.diagnostics", tgDiag->isChecked()    ? 1 : 0);
    settings.set("dock.dtc",         tgDtc->isChecked()     ? 1 : 0);
    settings.saveJson();
    // A downloaded, checked studio update: install it now that this one has finished with its files.
    s_updater.installStaged();
    return rc;
}
