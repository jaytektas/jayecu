// The autotuner's contract, as an imported TunerStudio-format definition states it.
//
// The studio's autotuner is driven entirely by what the definition declares — which map, which target,
// which channel reads the mixture, and which channel reports the correction the ECU is ALREADY
// applying. For our own firmware that comes from the schema. For a rusEFI board it comes from the
// .ini's [VeAnalyze] block, and two things about that block do not survive a naive reading:
//
//   1. The ego channel is DERIVED. The ini declares it as an alias ("egoForAutotune = { egoTrimPct }"
//      in this fixture) in [OutputChannels] — not a field declaration, so a field parser drops it and
//      the studio ends up not publishing a channel its own definition names.
//   2. The ego channel is a PERCENTAGE where 100 means no correction, which the ini says in a comment
//      beside the declaration. Our own channels are multipliers where 1.0 means no correction.
//
// Either mistake is silent and neither is small. A missing ego channel defaults to "no correction", so
// closed loop holds the mixture on target and a wrong VE table reads as a right one — the autotuner
// proposes nothing and looks like it agreed. Reading 103 % as a multiplier does not shade a proposal,
// it scales it by a hundred.
//
//   cmake --build build --target ts_autotune_test && ./build/ts_autotune_test
#include "model/TsIniImporter.h"
#include "model/MetaModel.h"
#include "model/TuneFile.h"
#include "model/TuneDiff.h"

#include <cstdio>
#include <cmath>
#include <fstream>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  %s  %s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : ("   - " + detail).c_str());
    if (!ok) ++fails;
}

// The shapes a TunerStudio-format definition uses, kept minimal but not simplified: the alias, a
// compound expression beside it, the [VeAnalyze] declaration with its trailing {condition}, and the two
// tables it names. Names, offsets and values are this test's own.
static const char* kIni = R"INI(
[TunerStudio]
   signature = "bench-ecu autotune fixture 0.4"

[Constants]
   pageSize = 4096
   page = 1
   fuelVe = array, U16, 64, [16x16], "%", 0.1, 0, 0, 250, 1
   fuelVeRpm = array, U16, 0, [16], "RPM", 1, 0, 0, 12000, 0
   fuelVeLoad = array, U16, 32, [16], "kPa", 0.1, 0, 0, 400, 1
   lambdaTarget = array, U08, 640, [16x16], "", 0.01, 0, 0.5, 2.0, 2
   lambdaTargetRpm = array, U16, 576, [16], "RPM", 1, 0, 0, 12000, 0
   lambdaTargetLoad = array, U16, 608, [16], "kPa", 0.1, 0, 0, 400, 1
   engineName = string, ASCII, 896, 32
   vinCode = string, ASCII, 928, 17, 17

[OutputChannels]
   ochBlockSize = 256
   rpm = scalar, U16, 0, "RPM", 1, 0
   clt = scalar, S16, 2, "deg C", 0.1, 0
   tps = scalar, S16, 4, "%", 0.01, 0
   lambda1 = scalar, U16, 6, "", 0.001, 0
   battV = scalar, U16, 8, "V", 0.01, 0
   egoTrimPct = scalar, U16, 10, "%", 0.01, 0
   tpsRate = scalar, F32, 12, "", 1, 0
   fuelLoad = scalar, U16, 16, "kPa", 0.1, 0
   targetLoad = scalar, U16, 18, "kPa", 0.1, 0
   ; the autotuner reads this one 100-based: 100 means the closed loop is adding nothing.
   egoForAutotune = { egoTrimPct }
   mixtureShown = { showLambda ? lambda1 : afr1 }

[TableEditor]
   table = fuelVeTbl, fuelVeMap, "Fuel VE", 1
      xBins = fuelVeRpm, rpm
      yBins = fuelVeLoad, fuelLoad
      zBins = fuelVe
   table = lambdaTargetTbl, lambdaTargetMap, "Lambda Target", 1
      xBins = lambdaTargetRpm, rpm
      yBins = lambdaTargetLoad, targetLoad
      zBins = lambdaTarget

[VeAnalyze]
   veAnalyzeMap = fuelVeTbl, lambdaTargetTbl, lambda1, egoForAutotune, { showLambda }
   filter = lowRpm, "Below idle", rpm,   <  , 600,  , true
   filter = battV, "Battery low", battV   <  , 11.5,  , true
)INI";

int main() {
    const jf::JJson root = TsIniImporter::importText(kIni);

    // ---- 1. THE DERIVED CHANNEL IS PUBLISHED, and it is the channel it aliases ----------------------
    // Delete the alias branch in the importer and this goes red: the definition then names an ego
    // channel the studio does not publish, which reads as "no correction" and cannot be told apart
    // from a correctly idle one.
    const jf::JJson& tel = root["telemetry"];
    const jf::JJson& ego = tel["egoForAutotune"];
    const jf::JJson& gego = tel["egoTrimPct"];
    ck(gego.isObject(), "the raw channel imports");
    ck(ego.isObject(), "…and the alias that names it is published too");
    if (ego.isObject() && gego.isObject()) {
        ck(ego["offset"].number<double>() == gego["offset"].number<double>() &&
           ego["scale"].number<double>()  == gego["scale"].number<double>()  &&
           ego["datatype"].str()          == gego["datatype"].str(),
           "…reading exactly the same bytes, the same way",
           "offset " + std::to_string(int(ego["offset"].number<double>())));
    }

    // AN EXPRESSION IS NOT A CHANNEL. There is no evaluator in an importer, so a compound one must be
    // ABSENT rather than invented — absent, a consumer can say it cannot read it; invented, it reads 0.
    ck(!tel["mixtureShown"].isObject(),
       "a compound expression is not turned into a channel it is not");

    // ---- 2. THE BASIS IS CARRIED, because the format fixes it --------------------------------------
    const jf::JJson& at = root["autotune"];
    ck(at.isObject(), "the [VeAnalyze] contract is carried across");
    ck(at["table"].str() == "ts.fuelVe", "…naming the map being tuned", at["table"].str());
    ck(at["target_table"].str() == "ts.lambdaTarget", "…and the map holding its target",
       at["target_table"].str());
    ck(at["lambda_channel"].str() == "lambda1", "…and the channel that reads the mixture");
    ck(at["filters"].arr().size() == 2, "…and its filters",
       std::to_string(at["filters"].arr().size()));
    const jf::JJson& egos = at["ego_channels"];
    ck(egos.arr().size() == 1, "…and one ego channel");
    if (egos.arr().size() == 1) {
        ck(egos[size_t(0)]["channel"].str() == "egoForAutotune", "…named as the ini names it");
        ck(egos[size_t(0)]["basis"].str() == "percent_100",
           "…declared 100-based, which is what a [VeAnalyze] ego channel is",
           egos[size_t(0)]["basis"].str());
    }

    // ---- 3. AND THE STUDIO READS IT THAT WAY -------------------------------------------------------
    // The conversion is the whole point of carrying the basis: 103 % is a 3 % enrichment, which is a
    // multiplier of 1.03. Read as a multiplier it would be 103.
    {
        MetaModel::AutotuneEgo e;
        e.basis = MetaModel::EgoBasis::Percent100;
        ck(std::fabs(e.asMultiplier(103.0) - 1.03) < 1e-9,
           "a 100-based channel reading 103 % is a multiplier of 1.03",
           std::to_string(e.asMultiplier(103.0)));
        ck(std::fabs(e.asMultiplier(100.0) - 1.0) < 1e-9, "…and 100 % is no correction at all");

        MetaModel::AutotuneEgo z;
        z.basis = MetaModel::EgoBasis::Percent0;
        ck(std::fabs(z.asMultiplier(3.0) - 1.03) < 1e-9, "a 0-based trim of 3 % is the same 1.03");

        MetaModel::AutotuneEgo m;   // the default, and what every native declaration means
        ck(std::fabs(m.asMultiplier(1.03) - 1.03) < 1e-9, "a multiplier is already what the engine wants");
        ck(MetaModel::egoBasisFromName("") == MetaModel::EgoBasis::Multiplier,
           "…and an unstated basis is a multiplier, so existing definitions mean what they meant");
    }

    // ---- 4. END TO END, through the file the studio actually loads ---------------------------------
    // The JSON above is not what the panel reads; it reads a MetaModel built from a .meta on disk. This
    // is the same trip: write it, load it, and ask the model what the contract says.
    {
        const std::string path = std::string(BUILD_TMP) + "/ts_autotune_test.meta";
        // writeMetaFile, not a plain dump: a .meta is the JSON body plus a CRC32 footer, and loadFile
        // checks it. Writing the body alone tests a file the studio would refuse.
        ck(TsIniImporter::writeMetaFile(root, path), "the import writes a .meta");
        MetaModel m;
        const bool loaded = m.loadFile(path);
        ck(loaded, "the imported definition loads as a definition");
        const MetaModel::Autotune& a = m.autotune();
        ck(a.valid(), "…carrying a usable contract");
        ck(a.egoChannels.size() == 1, "…with its ego channel",
           std::to_string(a.egoChannels.size()));
        if (a.egoChannels.size() == 1) {
            ck(a.egoChannels[0].basis == MetaModel::EgoBasis::Percent100,
               "…and the basis survives the round trip");
            ck(std::fabs(a.egoChannels[0].asMultiplier(97.0) - 0.97) < 1e-9,
               "…so a 3 % lean-out reads as 0.97, not 97");
        }
        ck(m.telemetry().count("egoForAutotune") == 1,
           "…and the ECU publishes the channel the contract names");
    }

    // ---- AN ASCII FIELD IS A WRITABLE SCALAR --------------------------------------------------------
    // A VIN or an engine name is text, and the studio edits it through a line edit that writes with
    // Cache::setConfigString. That write is gated on writableConfigPath, which is gated on isConfig,
    // which asks the meta whether the path resolves as a SCALAR. If an ASCII field resolved as anything
    // else the box would accept typing and silently discard it — the failure that cannot be seen, only
    // discovered later when the value is not there.
    {
        const std::string path = std::string(BUILD_TMP) + "/ts_autotune_test.meta";
        MetaModel m;
        if (m.loadFile(path)) {
            const MetaModel::Location L = m.locate("ts.engineName");
            ck(L.kind == MetaModel::Location::Kind::Scalar,
               "an ASCII config field resolves as a scalar, so it is writable",
               "kind=" + std::to_string(int(L.kind)));
            ck(L.valid(), "…and locates to real bytes", "offset " + std::to_string(L.offset));
            const auto& cfg = m.config();
            const auto it = cfg.find("ts.engineName");
            ck(it != cfg.end() && it->second.isText(),
               "…and the definition says it is text, which is what picks the line edit over a spin box");
        }
    }

    // ---- A TEXT DIFFERENCE IS A REPORTABLE DIFFERENCE -----------------------------------------------
    // Whether two tunes differ is decided by comparing what TuneFile::serialise writes; the difference
    // report walked config scalars and tables and decoded every one of them as a NUMBER. A name compared
    // that way reads its first four bytes and calls the rest equal — so two tunes could differ in an
    // engine name or a VIN, the studio would announce that the ECU no longer matches, and then have
    // nothing to show: a bare "keep which one?" prompt instead of the pages.
    {
        const std::string mp = std::string(BUILD_TMP) + "/ts_autotune_test.meta";
        MetaModel m2;
        if (m2.loadFile(mp)) {
            std::vector<uint8_t> a = m2.defaultImage();
            if (a.empty() && m2.configSize() > 0) a.assign(size_t(m2.configSize()), 0);
            int off = 0, size = 0;
            if (m2.resolveBlob("ts.engineName", off, size) && size > 8 && off + size <= int(a.size())) {
                for (int i = 0; i < 6; ++i) a[size_t(off + i)] = uint8_t('A' + i);
                std::vector<uint8_t> b = a;
                b[size_t(off + 5)] = 'Z';            // differs ONLY at byte 6, past a numeric read
                const bool differ = TuneFile::serialise(a, m2) != TuneFile::serialise(b, m2);
                ck(differ, "two tunes differing past a name's fourth byte are not in sync");
                const tunediff::Report r = tunediff::compare(m2, a, b, nullptr);
                ck(!r.empty(), "…and the report can say which setting it was",
                   std::to_string(r.settings) + " setting(s)");
                if (!r.unpaged.empty()) {
                    ck(r.unpaged[0].isText, "…reported as text, not decoded as a number");
                    ck(r.unpaged[0].textA != r.unpaged[0].textB, "…with both names shown",
                       r.unpaged[0].textA + " vs " + r.unpaged[0].textB);
                }
            }
        }
    }

    // ---- A TUNE SAVED FROM THE ECU MATCHES THE ECU WHEN IT IS LOADED AGAIN ---------------------------
    // The loop that kept the reconcile prompt coming back on every connect: the ECU held a
    // seventeen-character VIN, "keep the ECU's tune" saved seventeen to the file, and LOADING that file
    // put back sixteen — so the two disagreed again the moment the studio restarted, and choosing to keep
    // the ECU's tune appeared to do nothing at all. Save-then-load has to be the identity, or the studio
    // asks a question its own answer cannot settle.
    {
        const std::string mp = std::string(BUILD_TMP) + "/ts_autotune_test.meta";
        MetaModel m3;
        if (m3.loadFile(mp)) {
            int off = 0, size = 0;
            if (m3.resolveBlob("ts.vinCode", off, size) && size > 0) {
                std::vector<uint8_t> ecu = m3.defaultImage();
                if (ecu.empty() && m3.configSize() > 0) ecu.assign(size_t(m3.configSize()), 0);
                // Fill the field EXACTLY, the case a reserved terminator makes impossible.
                const auto& cf = m3.config().at("ts.vinCode");
                ck(cf.capacity() == size, "a VIN's capacity is its whole field, terminator or not",
                   std::to_string(cf.capacity()) + " of " + std::to_string(size));
                const int cap = cf.capacity();
                for (int i = 0; i < cap; ++i) ecu[size_t(off + i)] = uint8_t('A' + (i % 26));
                MigrationReport rep;
                const std::vector<uint8_t> saved = TuneFile::serialise(ecu, m3);
                const std::vector<uint8_t> back  = TuneFile::deserialise(saved, m3, rep);
                ck(back.size() == ecu.size(), "a saved tune loads back to the same size");
                bool same = back.size() == ecu.size();
                for (int i = 0; same && i < size; ++i)
                    same = back[size_t(off + i)] == ecu[size_t(off + i)];
                ck(same, "…and a text field that fills its capacity survives the round trip",
                   std::string(reinterpret_cast<const char*>(ecu.data() + off), size_t(cap)) + " -> " +
                   std::string(reinterpret_cast<const char*>(back.data() + off), size_t(cap)));
            }
        }
    }

    std::printf("\n%s\n", fails ? (std::to_string(fails) + " FAILED").c_str() : "ALL PASSED");
    return fails ? 1 : 0;
}
