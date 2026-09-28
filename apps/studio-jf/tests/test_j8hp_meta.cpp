// A J8HP (ZF 8HP transmission controller) definition loads through the same MetaModel as a jayecu one, says it is a
// TCU, and its config / telemetry resolve; the engine ECU's own definition still says it is an ECU; the identity
// products the link accepts are exactly jayecu and j8hp.
#include "comms/Products.h"
#include "model/MetaModel.h"
#include <cstdio>
#include <cstdlib>
#include <string>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main()
{
    MetaModel t;
    CHECK(t.loadFile(J8HP_META));
    CHECK(t.isValid());
    CHECK(t.deviceClass() == "tcu");
    CHECK(t.product() == "j8hp");
    CHECK(t.board() == "j8hp_v1");
    CHECK(t.layoutHash().size() == 8);
    const int cfg = t.configSize();
    CHECK(cfg > 4 && static_cast<int>(t.defaultImage().size()) == cfg);
    CHECK(t.hasTelemetry("config_dirty"));              // the burn handshake channel
    CHECK(t.hasTelemetry("sol_ipropi_12"));
    CHECK(!t.hasTelemetry("rpm") && !t.hasTelemetry("clt"));
    CHECK(!t.navigationTree().arr().empty());

    MetaModel e;
    CHECK(e.loadFile(REAL_META));
    CHECK(e.deviceClass() == "ecu");                   // no device_class in an engine meta = ECU
    CHECK(e.hasTelemetry("rpm"));

    CHECK(isKnownProduct(std::string("jayecu jaytek_v1 0.4.0 c1c57d8 56935516 0011 t993")));
    CHECK(isKnownProduct(std::string("j8hp j8hp_v1 0.1.0 fd4e419 3cbece25 0011 t43")));
    CHECK(!isKnownProduct(std::string("j8hpx board")));  // a token, not a prefix
    CHECK(!isKnownProduct(std::string("J8HP e6edafa4")));  // the old J8HP identity is not accepted
    CHECK(!isKnownProduct(std::string("rusEFI master")));

    std::printf(fails ? "%d FAILED\n" : "ALL PASS\n", fails);
    return fails ? 1 : 0;
}
