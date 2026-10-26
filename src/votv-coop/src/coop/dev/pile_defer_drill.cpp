// coop/dev/pile_defer_drill.cpp -- see coop/dev/pile_defer_drill.h. The client's counts of its deferred piles, its
// window and its verdict, the forced give-up, and the tick that hands the host's reports to the judge
// (pile_defer_drill_host.cpp).

#include "coop/dev/pile_defer_drill.h"

#include "pile_defer_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/dev/pile_defer_report.h"
#include "coop/element/bind_wait.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/save_time_retire_util.h"
#include "coop/session/join_progress.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace coop::dev::pile_defer_drill {
namespace {

namespace BW = coop::element::bind_wait;
namespace R = ue_wrap::reflection;
namespace Rep = coop::dev::pile_defer_report;

// The sweep makes 40 passes at 250 ms from the join's end; this is longer, and the quiescence signal is also asked.
constexpr uint64_t kWindowMs = 20000;
// A failure bound, never a wait: past it the load tail is called unquiesced and the drill fails.
constexpr uint64_t kQuiesceBoundMs = 120000;
constexpr uint16_t kConvertKind = static_cast<uint16_t>(coop::net::ReliableKind::PropConvert);
constexpr unsigned kMaxNoted = 6;   // deferrals said one by one; the rest are in the verdict

struct Deferred {
    uint64_t deferredAt = 0;
    uint64_t boundAt = 0;     // the sweep adopted its native
    uint64_t gaveUpAt = 0;    // the sweep gave up with no native
};
std::unordered_map<uint32_t, Deferred> g_deferred;

uint32_t g_convertsAtDeferred = 0, g_convertsHeld = 0;

uint32_t g_forcedEid = 0;
bool g_heldAtForce = false;

bool g_leftIdle = false, g_done = false;
uint64_t g_idleAtMs = 0;

bool Armed() { return Rep::ArmOf() != Rep::Arm::Off; }

// The native trash actors no host eid owns: what the join's census calls orphans.
unsigned CountOrphans() {
    unsigned orphans = 0;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o)) continue;
        if (!ue_wrap::prop::IsTrashActor(o)) continue;
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;
        if (!coop::prop_element_tracker::IsBoundMirrorNative(o)) ++orphans;
    }
    return orphans;
}

// One verdict's failing checks, each its own line, counted.
struct Fails {
    unsigned n = 0;
    void Add(const char* fmt, ...) {
        ++n;
        char text[256];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(text, sizeof(text), fmt, ap);
        va_end(ap);
        UE_LOGW("[PILE-DEFER] client FAIL: %s", text);
    }
};

void Window(coop::net::Session& session) {
    Fails fails;
    unsigned bound = 0, gaveUp = 0;
    for (const auto& [eid, d] : g_deferred) {
        if (d.gaveUpAt) { ++gaveUp; continue; }
        if (coop::element::LivePropActor(static_cast<coop::element::ElementId>(eid))) ++bound;
    }
    const uint32_t deferred = static_cast<uint32_t>(g_deferred.size());
    const uint32_t held = BW::HeldOf(kConvertKind), replayed = BW::ReplayedOf(kConvertKind);
    const uint32_t coalesced = BW::CoalescedOf(kConvertKind);   // converts a newer convert replaced: not in held
    const uint32_t heldGivenUp = BW::GivenUpOf(kConvertKind), neverBound = BW::DropsNeverBound();
    const unsigned orphans = CountOrphans();

    std::vector<coop::net::DevParityEntry> rows;
    detail::CollectTrash(rows, /*mirrorsOnly=*/true);
    if (!Rep::Send(session, rows)) fails.Add("the parity report of %zu rows was not sent", rows.size());

    if (held != replayed + heldGivenUp)
        fails.Add("held=%u != replayed=%u + heldGivenUp=%u", held, replayed, heldGivenUp);
    if (g_convertsHeld != g_convertsAtDeferred)
        fails.Add("%u of %u converts that met a deferred eid were not held", g_convertsAtDeferred - g_convertsHeld,
                  g_convertsAtDeferred);
    if (neverBound > 0) fails.Add("neverBound=%u, expected 0", neverBound);
    if (orphans > 0) fails.Add("orphans=%u, expected 0", orphans);
    if (Rep::ArmOf() == Rep::Arm::Check) {
        if (gaveUp > 0) fails.Add("gaveup=%u in arm check, expected 0", gaveUp);
    } else {
        if (!g_forcedEid) {
            fails.Add("no deferred pile's native was found by the sweep to destroy");
        } else {
            const auto it = g_deferred.find(g_forcedEid);
            const bool gaveUpIt = it != g_deferred.end() && it->second.gaveUpAt != 0;
            const bool materialised =
                coop::element::LivePropActor(static_cast<coop::element::ElementId>(g_forcedEid)) != nullptr;
            const bool stillHeld = BW::Holds(g_forcedEid, kConvertKind);
            if (!gaveUpIt) fails.Add("eid=%u was forced but the sweep did not give up on it", g_forcedEid);
            if (!g_heldAtForce) fails.Add("eid=%u had no convert held to replay", g_forcedEid);
            else if (stillHeld) fails.Add("eid=%u converts did not replay", g_forcedEid);
            if (!materialised) fails.Add("eid=%u ends with no mirror bound to it", g_forcedEid);
        }
    }
    UE_LOGI("[PILE-DEFER] client DONE deferred=%u bound=%u gaveup=%u held=%u replayed=%u heldGivenUp=%u coalesced=%u "
            "neverBound=%u orphans=%u", deferred, bound, gaveUp, held, replayed, heldGivenUp, coalesced, neverBound, orphans);
}

void ClientTick(coop::net::Session& session) {
    if (g_done) return;
    const bool idle = coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
    if (!g_leftIdle) {
        if (!idle) g_leftIdle = true;
        return;
    }
    if (!idle) return;
    const uint64_t now = coop::net::NowMs();
    if (!g_idleAtMs) {
        g_idleAtMs = now;
        return;
    }
    if (now - g_idleAtMs < kWindowMs) return;
    if (!coop::join_membership_sweep::HasLoadTailQuiesced()) {
        if (now - g_idleAtMs >= kQuiesceBoundMs) {
            UE_LOGW("[PILE-DEFER] client FAIL: the load tail had not quiesced %llu s after the join",
                    static_cast<unsigned long long>(kQuiesceBoundMs / 1000));
            UE_LOGI("[PILE-DEFER] client DONE deferred=%zu bound=0 gaveup=0 held=0 replayed=0 heldGivenUp=0 coalesced=0 "
                    "neverBound=0 orphans=0", g_deferred.size());
            g_done = true;
        }
        return;
    }
    Window(session);
    g_done = true;
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!Armed() || !session) return;
    if (session->role() == coop::net::Role::Host) {
        Rep::Tick();
        Rep::Report r;
        while (Rep::TakeReport(r)) detail::JudgeReport(r);
        return;
    }
    if (session->role() == coop::net::Role::Client) ClientTick(*session);
}

void OnDisconnect() {
    if (!Armed()) return;
    Rep::OnDisconnect();
    g_deferred.clear();
    g_convertsAtDeferred = g_convertsHeld = 0;
    g_forcedEid = 0;
    g_heldAtForce = false;
    g_leftIdle = g_done = false;
    g_idleAtMs = 0;
}

void NoteDeferred(uint32_t eid) {
    if (!Armed()) return;
    const auto ins = g_deferred.try_emplace(eid);
    if (!ins.second) return;
    ins.first->second.deferredAt = coop::net::NowMs();
    if (g_deferred.size() <= kMaxNoted)
        UE_LOGI("[PILE-DEFER] client deferred eid=%u (#%zu)", eid, g_deferred.size());
}

void NoteBound(uint32_t eid) {
    if (!Armed()) return;
    const auto it = g_deferred.find(eid);
    if (it == g_deferred.end() || it->second.boundAt) return;
    it->second.boundAt = coop::net::NowMs();
    if (g_deferred.size() <= kMaxNoted)
        UE_LOGI("[PILE-DEFER] client bound eid=%u, %llu ms after its deferral", eid,
                static_cast<unsigned long long>(it->second.boundAt - it->second.deferredAt));
}

void NoteGivenUp(uint32_t eid) {
    if (!Armed()) return;
    const auto it = g_deferred.find(eid);
    if (it == g_deferred.end() || it->second.gaveUpAt) return;
    it->second.gaveUpAt = coop::net::NowMs();
    UE_LOGI("[PILE-DEFER] client gave up eid=%u, %llu ms after its deferral", eid,
            static_cast<unsigned long long>(it->second.gaveUpAt - it->second.deferredAt));
}

void NoteConvertAtDeferred(bool held) {
    if (!Armed()) return;
    ++g_convertsAtDeferred;
    if (held) ++g_convertsHeld;
}

bool ForceGiveUp(uint32_t eid, void* native) {
    if (Rep::ArmOf() != Rep::Arm::Giveup || !native) return false;
    if (g_forcedEid) return eid == g_forcedEid;   // the same eid at a later pass: the destroyed native stays refused
    if (g_deferred.find(eid) == g_deferred.end()) return false;
    g_forcedEid = eid;
    g_heldAtForce = BW::Holds(eid, kConvertKind);
    UE_LOGI("[PILE-DEFER] client forced give-up eid=%u", eid);
    coop::save_time_retire_util::UnmarkAndDestroy(native);
    return true;
}

}  // namespace coop::dev::pile_defer_drill
