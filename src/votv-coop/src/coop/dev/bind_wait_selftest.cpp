// coop/dev/bind_wait_selftest.cpp -- see coop/dev/bind_wait_selftest.h.

#include "coop/dev/bind_wait_selftest.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/bind_wait.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
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
constexpr uint32_t kEids = 16;  // kBase .. kBase+15
// A pump with real holds in it is not the selftest's to run: it waits this many ticks for them to clear.
constexpr uint32_t kMaxWaitTicks = 600;

bool g_fired = false;
uint32_t g_waitTicks = 0;

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
uint32_t g_giveUpEid = 0;   // check 7: the eid the 'K' replay gives up
int g_leaveSlot = 0;        // check 8: the slot the 'L' replay drops
uint32_t g_dropEid = 0;     // check 10: the eid whose kind-80 holds the 'D' replay drops

void Record(const uint8_t* bytes, size_t n, int slot) {
    const uint8_t tag = n > 0 ? bytes[0] : 0;
    g_calls.push_back(Call{tag, slot});
    if (tag == 'D') BW::Drop(g_dropEid, 80, "selftest: a replay drops a later hold of its own eid");
    if (tag == 'K') BW::GiveUp(g_giveUpEid, "selftest: a replay gives up a due eid");
    if (tag == 'L') BW::OnSlotLeft(g_leaveSlot);
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

// Check 9: a prop row that outlived its purged actor (a live row, an actor whose object slot is gone) is the
// window before BindUnboundReCreates re-binds it. A registry mirror at a free host id stands in for it, with
// an actor pointer that is never read: the slot index -1 reads as not live.
const char* CheckPurgeWindow() {
    namespace CE = coop::element;
    CE::ElementId eid = CE::kHostRangeSize - 1;
    while (eid > 1 && CE::Registry::Get().Get(eid)) --eid;
    if (CE::Registry::Get().Get(eid)) return "no free host eid for the purge-window row";
    CE::Element row(CE::ElementType::Prop);
    row.SetActor(reinterpret_cast<void*>(static_cast<uintptr_t>(0x10)), -1);
    if (!CE::Registry::Get().RegisterMirror(eid, &row)) return "the purge-window row was not registered";
    g_calls.clear();
    const char* failed = nullptr;
    if (!HoldTag(eid, 70, 1, 'W', false)) failed = "a hold for a row awaiting its re-bind was refused";
    else if (!BW::IsDeferred(eid)) failed = "a row awaiting its re-bind was not marked";
    else {
        BW::OnBound(eid);
        BW::Pump();
        if (!Calls("W")) failed = "the hold of a row awaiting its re-bind did not replay at the bind";
    }
    BW::GiveUp(eid, "selftest");
    g_calls.clear();
    return failed;   // `row` unregisters itself
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
    const uint32_t refused0 = BW::RefusedCount();
    for (uint16_t k = 0; k < 65; ++k) HoldTag(e7, 100 + k, 1, 'z', false);
    if (BW::RefusedCount() != refused0 + 1) return "the refusal at the per-eid bound was not counted";
    BW::OnBound(e7);
    BW::Pump();
    if (g_calls.size() != 64) return "the per-eid bound did not hold at 64";
    g_calls.clear();
    // 7. A give-up made by a replay reaches an eid already swapped out of the due list.
    const uint32_t e8 = kBase + 8, e9 = kBase + 9;
    g_giveUpEid = e9;
    BW::MarkDeferred(e8, "selftest: giver");
    BW::MarkDeferred(e9, "selftest: given up in flight");
    HoldTag(e8, 50, 1, 'K', false);
    HoldTag(e9, 51, 1, 'Y', false);
    BW::OnBound(e8);
    BW::OnBound(e9);
    BW::Pump();
    if (!Calls("K")) return "a give-up during a pump did not void the due eid it named";
    g_calls.clear();
    // 8. A slot that leaves during a pump has its remaining holds skipped, in every due eid.
    const uint32_t e10 = kBase + 10, e11 = kBase + 11;
    g_leaveSlot = 2;
    BW::MarkDeferred(e10, "selftest: leaver");
    BW::MarkDeferred(e11, "selftest: left");
    HoldTag(e10, 60, 1, 'L', false);
    HoldTag(e10, 61, 2, 'M', false);
    HoldTag(e11, 62, 2, 'N', false);
    HoldTag(e11, 63, 1, 'O', false);
    BW::OnBound(e10);
    BW::OnBound(e11);
    BW::Pump();
    if (!Calls("LO")) return "a slot that left during a pump still had its holds replayed";
    g_calls.clear();
    // 9. A row whose actor was purged is held without a mark, and its re-bind frees the hold.
    const char* purged = CheckPurgeWindow();
    if (purged) return purged;
    // 10. A replay that Drops a later hold of its own eid during the pass: that hold does not run.
    const uint32_t e12 = kBase + 12;
    g_dropEid = e12;
    const uint32_t dropped80 = BW::DroppedOf(80);
    BW::MarkDeferred(e12, "selftest: a replay drops");
    HoldTag(e12, 80, 1, 'D', false);
    HoldTag(e12, 81, 1, 'E', false);
    HoldTag(e12, 80, 1, 'F', false);
    BW::OnBound(e12);
    BW::Pump();
    if (!Calls("DE")) return "a Drop made by a replay did not void the later hold of its kind in the pass";
    if (BW::DroppedOf(80) != dropped80 + 1) return "the hold a replay's Drop voided was not counted as dropped";
    g_calls.clear();
    // 11. A Drop of a stored hold erases it, releases its bytes and leaves the eid's other kinds.
    const uint32_t e13 = kBase + 13;
    const uint32_t dropped90 = BW::DroppedOf(90);
    BW::MarkDeferred(e13, "selftest: drop stored");
    HoldTag(e13, 90, 1, 'S', false);
    HoldTag(e13, 91, 1, 'T', false);
    if (!BW::Holds(e13, 90) || !BW::Holds(e13, 91)) return "Holds did not see a stored hold";
    BW::Drop(e13, 90, "selftest");
    if (BW::Holds(e13, 90) || !BW::Holds(e13, 91)) return "a Drop of a stored hold took more or less than its kind";
    if (BW::DroppedOf(90) != dropped90 + 1) return "a Drop of a stored hold was not counted as dropped";
    BW::OnBound(e13);
    BW::Pump();
    if (!Calls("T")) return "a stored hold that was dropped still replayed";
    if (BW::HasHolds()) return "a Drop left bytes held";
    g_calls.clear();
    // 12. A Drop of a due hold (bound, not yet pumped) erases it.
    const uint32_t e14 = kBase + 14;
    BW::MarkDeferred(e14, "selftest: drop due");
    HoldTag(e14, 92, 1, 'U', false);
    HoldTag(e14, 93, 1, 'V', false);
    BW::OnBound(e14);
    if (!BW::Holds(e14, 92)) return "Holds did not see a due hold";
    BW::Drop(e14, 92, "selftest");
    if (BW::Holds(e14, 92) || !BW::Holds(e14, 93)) return "a Drop of a due hold took more or less than its kind";
    BW::Pump();
    if (!Calls("V")) return "a due hold that was dropped still replayed";
    g_calls.clear();
    // 13. Two holds of one kind: a coalesced one overwrites in place and counts as coalesced, not as held.
    const uint32_t e15 = kBase + 15;
    const uint32_t held94 = BW::HeldOf(94), coalesced94 = BW::CoalescedOf(94), replayed94 = BW::ReplayedOf(94);
    BW::MarkDeferred(e15, "selftest: coalesced tally");
    HoldTag(e15, 94, 1, 'c', true);
    HoldTag(e15, 94, 1, 'C', true);
    if (BW::HeldOf(94) != held94 + 1 || BW::CoalescedOf(94) != coalesced94 + 1)
        return "a coalesced hold was not counted apart from a distinct one";
    BW::OnBound(e15);
    BW::Pump();
    if (!Calls("C") || BW::ReplayedOf(94) != replayed94 + 1) return "a coalesced hold did not replay once, latest bytes";
    g_calls.clear();
    // 14. A GiveUp of an eid with two holds of one kind counts both into GivenUpOf.
    const uint32_t e16 = kBase + 1;
    const uint32_t givenUp95 = BW::GivenUpOf(95);
    BW::MarkDeferred(e16, "selftest: given-up tally");
    HoldTag(e16, 95, 1, 'g', false);
    HoldTag(e16, 95, 1, 'h', false);
    BW::GiveUp(e16, "selftest");
    if (BW::GivenUpOf(95) != givenUp95 + 2) return "a GiveUp of two holds of one kind was not counted into GivenUpOf";
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
    else UE_LOGI("[BIND-WAIT-SELFTEST] PASS (14 checks)");
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!Enabled() || g_fired || !session || session->role() == coop::net::Role::Host) return;
    if (!coop::net_pump::HasAnnouncedWorldReady()) return;
    // The checks pump the whole due list, so a real hold must not be in it: wait, bounded, then do not run.
    if (BW::HasHolds()) {
        if (++g_waitTicks < kMaxWaitTicks) return;
        g_fired = true;
        UE_LOGW("[BIND-WAIT-SELFTEST] SKIPPED: real holds present");
        return;
    }
    g_fired = true;
    Run();
}

void OnDisconnect() { g_fired = false; g_waitTicks = 0; }

}  // namespace coop::dev::bind_wait_selftest
