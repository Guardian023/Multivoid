// coop/dev/bind_wait_selftest.cpp -- see coop/dev/bind_wait_selftest.h.

#include "coop/dev/bind_wait_selftest.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/bind_wait.h"
#include "coop/net/session.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/core/log.h"

#include <cstdint>
#include <vector>

namespace coop::dev::bind_wait_selftest {
namespace {

namespace BW = coop::element::bind_wait;

// Above kMaxElements (65536) and below kInvalidId: no row of the registry and no wire eid is ever here.
constexpr uint32_t kBase = 0xFFFE0000u;
constexpr uint32_t kEids = 8;   // kBase .. kBase+7

bool g_fired = false;

bool Enabled() {
    static const bool on = coop::config::ResolveInt(::coop::config_registry::rows::bind_wait_selftest) != 0;
    return on;
}
bool RedOn() {
    return coop::config::ResolveInt(::coop::config_registry::rows::bind_wait_red) != 0;
}

// What the replay saw: the first byte of each message and the slot that sent it.
struct Call { uint8_t tag; int slot; };
std::vector<Call> g_calls;
uint32_t g_reholdEid = 0;   // check 3: the eid the 'R' replay defers and holds again

void Record(const uint8_t* bytes, size_t n, int slot) {
    const uint8_t tag = n > 0 ? bytes[0] : 0;
    g_calls.push_back(Call{tag, slot});
    if (tag == 'R') {   // a replay that defers a new eid and binds it: due, but only at the next Pump
        BW::MarkDeferred(g_reholdEid, "selftest: a replay defers again");
        const uint8_t s = 'S';
        BW::Hold(g_reholdEid, 21, slot, &Record, &s, 1, false);
        BW::OnBound(g_reholdEid);
    }
}

bool HoldTag(uint32_t eid, uint16_t kind, int slot, uint8_t tag, bool coalesce) {
    return BW::Hold(eid, kind, slot, &Record, &tag, 1, coalesce);
}

bool Calls(const char* tags) {
    size_t i = 0;
    for (; tags[i]; ++i)
        if (i >= g_calls.size() || g_calls[i].tag != static_cast<uint8_t>(tags[i])) return false;
    return g_calls.size() == i;
}

// The first failing check's name, or null.
const char* CheckGreen() {
    g_calls.clear();
    // 1.
    if (HoldTag(kBase + 0, 1, 1, 'x', false)) return "a hold for an eid not deferred was taken";
    // 2.
    const uint32_t e2 = kBase + 1;
    BW::MarkDeferred(e2, "selftest: order");
    if (!BW::IsDeferred(e2)) return "a marked eid does not read as deferred";
    if (!HoldTag(e2, 10, 1, 'A', true) || !HoldTag(e2, 11, 1, 'B', true) || !HoldTag(e2, 10, 1, 'a', true) ||
        !HoldTag(e2, 12, 1, 'C', true))
        return "a hold for a deferred eid was refused";
    BW::OnBound(e2);
    if (BW::IsDeferred(e2)) return "a bound eid still reads as deferred";
    BW::Pump();
    if (!Calls("aBC")) return "the replay order is not A' B C";
    g_calls.clear();
    // 3.
    const uint32_t e3 = kBase + 2;
    g_reholdEid = kBase + 3;
    BW::MarkDeferred(e3, "selftest: re-hold");
    HoldTag(e3, 20, 1, 'R', false);
    HoldTag(e3, 22, 1, 'X', false);
    BW::OnBound(e3);
    BW::Pump();
    if (!Calls("RX")) return "a replay that holds again changed the running pump";
    BW::Pump();
    if (!Calls("RXS")) return "the hold made inside a replay did not run at the next pump";
    g_calls.clear();
    // 4.
    const uint32_t e4 = kBase + 4;
    BW::MarkDeferred(e4, "selftest: give up");
    HoldTag(e4, 30, 1, 'G', false);
    BW::GiveUp(e4, "selftest");
    BW::OnBound(e4);
    BW::Pump();
    if (BW::IsDeferred(e4) || !g_calls.empty()) return "a given-up eid's holds were replayed";
    // 5.
    const uint32_t e5 = kBase + 5, e6 = kBase + 6;
    BW::MarkDeferred(e5, "selftest: slot left");
    BW::MarkDeferred(e6, "selftest: slot left");
    HoldTag(e5, 40, 1, 'P', false);
    HoldTag(e5, 41, 2, 'Q', false);
    HoldTag(e6, 40, 1, 'U', false);
    BW::OnSlotLeft(1);
    BW::OnBound(e5);
    BW::OnBound(e6);
    BW::Pump();
    if (!Calls("Q") || g_calls[0].slot != 2) return "a slot that left took more than its own holds";
    g_calls.clear();
    // 6.
    const uint32_t e7 = kBase + 7;
    BW::MarkDeferred(e7, "selftest: bound");
    for (uint16_t k = 0; k < 65; ++k) HoldTag(e7, 100 + k, 1, 'z', false);
    BW::OnBound(e7);
    BW::Pump();
    if (g_calls.size() != 64) return "the per-eid bound did not hold at 64";
    return nullptr;
}

// With the red row on a hold is refused whatever the eid: the eid is deferred, so only the row refuses it.
const char* CheckRed() {
    const uint32_t e = kBase + 0;
    BW::MarkDeferred(e, "selftest: red arm");
    const bool taken = HoldTag(e, 1, 1, 'x', false);
    BW::GiveUp(e, "selftest");
    return taken ? "a hold was taken with bind_wait_red=1" : nullptr;
}

// The synthetic eids only: the real marks and holds of the session stay.
void Cleanup() {
    for (uint32_t i = 0; i < kEids; ++i) BW::GiveUp(kBase + i, "selftest cleanup");
    g_calls.clear();
}

void Run() {
    if (RedOn()) {
        if (HoldTag(kBase + 0, 1, 1, 'x', false)) {
            UE_LOGE("[BIND-WAIT-SELFTEST] FAIL: a hold for an eid not deferred was taken");
            return;
        }
        const char* failed = CheckRed();
        Cleanup();
        if (failed) UE_LOGE("[BIND-WAIT-SELFTEST] FAIL: %s", failed);
        else UE_LOGI("[BIND-WAIT-SELFTEST] RED ARM: bind_wait_red=1 refuses every hold (2 checks); no PASS by design");
        return;
    }
    const char* failed = CheckGreen();
    Cleanup();
    if (failed) UE_LOGE("[BIND-WAIT-SELFTEST] FAIL: %s", failed);
    else UE_LOGI("[BIND-WAIT-SELFTEST] PASS (6 checks)");
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!Enabled() || g_fired || !session || session->role() == coop::net::Role::Host) return;
    if (!coop::net_pump::HasAnnouncedWorldReady()) return;
    g_fired = true;
    Run();
}

void OnDisconnect() { g_fired = false; }

}  // namespace coop::dev::bind_wait_selftest
