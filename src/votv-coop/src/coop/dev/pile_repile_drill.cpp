// coop/dev/pile_repile_drill.cpp -- see coop/dev/pile_repile_drill.h. [MEASURE sync-order-wp5, temporary]

#include "coop/dev/pile_repile_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/clear_hand.h"
#include "coop/dev/director/director.h"
#include "coop/element/element.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_snapshot.h"
#include "coop/props/trash_channel.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>

namespace coop::dev::pile_repile_drill {
namespace {

namespace D  = coop::director;
namespace E  = ue_wrap::engine;
namespace EL = coop::element;
namespace TC = coop::trash_channel;

constexpr float    kPickMinCm     = 250.f;    // the director's own pile band
constexpr float    kPickMaxCm     = 1.0e7f;   // the nearest pile, however far: a fresh world has none within 300 m
constexpr float    kReachCm       = 150.f;    // the use key's trace takes it from here
constexpr int      kWalkDeadlineS = 60;
constexpr float    kWalkFloorCmS  = 200.f;    // a walk's deadline grows by its straight length at under half the pace
constexpr uint64_t kStepBoundMs   = 60000;    // the grab, and the land, each
constexpr uint64_t kHoldMs        = 1500;     // a player holds it a moment before letting go
constexpr uint64_t kDriftWindowMs = 20000;
constexpr uint64_t kSampleMs      = 1000;
constexpr uint64_t kAfterJoinMs   = 30000;    // the second join's steady state, before DONE
constexpr float    kRestCmS       = 0.5f;     // at rest below this speed

enum class Step { WaitJoin, Pick, Walk, Grab, Hold, Land, Watch, Idle };

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_saidArm = false;

Step g_step = Step::WaitJoin;
uint64_t g_stepMs = 0;
uint32_t g_eid = 0;
ue_wrap::FVector g_grabbedAt{};
std::shared_ptr<D::BackgroundWalk> g_walk;
std::unique_ptr<D::AimedGrab> g_grab;
D::ClearHand g_clear;
bool g_clearing = false;
bool g_sawCarry = false;

// Watch: the drift of the landed pile, and the client's second join.
ue_wrap::FVector g_landedAt{};
uint64_t g_landedMs = 0, g_nextSampleMs = 0;
bool g_driftDone = false;
bool g_sawLeave = false;
uint64_t g_rejoinedMs = 0;

bool IsEnabled_() {
    static const bool s = coop::config::ResolveInt(::coop::config_registry::rows::pile_repile_drill) != 0;
    return s;
}

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float Len(const ue_wrap::FVector& v) { return std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z); }

void Fail(const char* step) {
    g_step = Step::Idle;
    if (g_walk) D::CancelBackgroundWalk(*g_walk);
    UE_LOGW("[PILE-REPILE] FAIL: %s", step);
}

// A slot whose join is over: its world announced and its bracket closed.
bool AnySlotJoined(coop::net::Session* s) {
    for (int i = 1; i < static_cast<int>(coop::players::kMaxPeers); ++i)
        if (s->IsSlotWorldReady(i) && coop::prop_snapshot::IsBracketClosed(i)) return true;
    return false;
}

bool AnySlotReady(coop::net::Session* s) {
    for (int i = 1; i < static_cast<int>(coop::players::kMaxPeers); ++i)
        if (s->IsSlotWorldReady(i)) return true;
    return false;
}

// E's pile, or null while E is a clump or has no live actor.
void* LivePile() {
    void* a = TC::LiveActorOf(static_cast<EL::ElementId>(g_eid));
    return (a && ue_wrap::prop::IsChipPile(a)) ? a : nullptr;
}

void TickDrift(uint64_t ms) {
    if (g_driftDone || ms < g_nextSampleMs) return;
    g_nextSampleMs = ms + kSampleMs;
    void* pile = LivePile();
    ue_wrap::FVector at{};
    if (!pile || !E::TryGetActorLocation(pile, at)) return;   // E is a clump again, or unread: the next sample
    const float speed = Len(E::GetActorVelocity(pile));
    const uint64_t since = ms - g_landedMs;
    UE_LOGI("[PILE-REPILE] eid=%u speed=%.1f cm/s %.1f cm from where it landed, %llu ms after", g_eid, speed,
            Dist(at, g_landedAt), static_cast<unsigned long long>(since));
    if (speed < kRestCmS) {
        UE_LOGI("[PILE-REPILE] eid=%u at rest after %llu ms, %.1f cm from where it landed", g_eid,
                static_cast<unsigned long long>(since), Dist(at, g_landedAt));
        g_driftDone = true;
    } else if (since >= kDriftWindowMs) {
        UE_LOGI("[PILE-REPILE] eid=%u not at rest after %llu ms, %.1f cm from where it landed", g_eid,
                static_cast<unsigned long long>(since), Dist(at, g_landedAt));
        g_driftDone = true;
    }
}

// The client leaves, joins again; 30 s after the second join's announcement and the drift read, DONE.
void TickRejoin(coop::net::Session* s, uint64_t ms) {
    if (!g_sawLeave) {
        if (!AnySlotReady(s)) {
            g_sawLeave = true;
            UE_LOGI("[PILE-REPILE] host: the client left");
        }
        return;
    }
    if (!g_rejoinedMs) {
        if (!AnySlotJoined(s)) return;
        g_rejoinedMs = ms;
        UE_LOGI("[PILE-REPILE] host: the client's second join is over");
        return;
    }
    if (g_driftDone && ms - g_rejoinedMs >= kAfterJoinMs) {
        UE_LOGI("[PILE-REPILE] host DONE");
        g_step = Step::Idle;
    }
}

void TickHost(coop::net::Session* s, uint64_t ms) {
    if (g_step == Step::Idle) return;
    if (g_step == Step::WaitJoin) {
        if (!AnySlotJoined(s)) return;
        UE_LOGI("[PILE-REPILE] host: the client's join is over");
        g_step = Step::Pick;
        return;
    }
    void* player = coop::players::Registry::Get().Local();
    if (!player) { Fail("the host's player is gone"); return; }
    switch (g_step) {
    case Step::Pick: {
        D::DirectorGoal goal;
        if (!D::PickReachablePile(player, kPickMinCm, kPickMaxCm, goal)) {
            UE_LOGW("[PILE-REPILE] FAIL: no reachable pile");
            g_step = Step::Idle;
            return;
        }
        const EL::ElementId eid = coop::prop_element_tracker::GetPropElementIdForActor(goal.targetActor);
        if (eid == EL::kInvalidId || eid == 0u) { Fail("the picked pile has no eid"); return; }
        g_eid = static_cast<uint32_t>(eid);
        g_grabbedAt = goal.targetPos;
        { ue_wrap::FVector me{}; E::TryGetActorLocation(player, me); UE_LOGI("[PILE-REPILE] host walks to eid=%u, %.0f m away", g_eid, Dist(me, goal.targetPos) / 100.f); }
        ue_wrap::FVector from{};
        E::TryGetActorLocation(player, from);
        g_walk = D::StartBackgroundWalk(goal.targetPos, kReachCm,
                                        kWalkDeadlineS + static_cast<int>(Dist(from, goal.targetPos) / kWalkFloorCmS));
        g_grab = std::make_unique<D::AimedGrab>(player, goal.targetActor);
        UE_LOGI("[PILE-REPILE] host: walking to eid=%u at (%.1f,%.1f,%.1f)", g_eid, g_grabbedAt.X, g_grabbedAt.Y,
                g_grabbedAt.Z);
        g_step = Step::Walk;
        return;
    }
    case Step::Walk: {
        const int w = g_walk ? g_walk->state.load() : 2;
        if (w == 0) return;
        if (w == 2) { Fail("walk"); return; }
        g_stepMs = ms;
        g_step = Step::Grab;
        return;
    }
    case Step::Grab: {
        const D::GrabState st = g_grab->Tick();
        if (st == D::GrabState::Working) {
            if (ms - g_stepMs >= kStepBoundMs) Fail("grab");
            return;
        }
        if (st == D::GrabState::Failed) {
            UE_LOGW("[PILE-REPILE] grab: %s", g_grab->Why());
            Fail("grab");
            return;
        }
        UE_LOGI("[PILE-REPILE] host: grabbed eid=%u", g_eid);
        g_stepMs = ms;
        g_sawCarry = false;
        g_step = Step::Hold;
        return;
    }
    case Step::Hold:
        if (TC::IsCarrying(static_cast<EL::ElementId>(g_eid))) g_sawCarry = true;
        if (ms - g_stepMs < kHoldMs) return;
        if (!g_sawCarry) { Fail("carry"); return; }
        g_clearing = true;
        g_stepMs = ms;
        g_step = Step::Land;
        return;
    case Step::Land: {
        const EL::ElementId e = static_cast<EL::ElementId>(g_eid);
        if (g_clearing) {
            const D::ClearState cs = g_clear.Tick();
            if (cs == D::ClearState::Failed) { UE_LOGW("[PILE-REPILE] land: %s", g_clear.Why()); Fail("land"); return; }
            if (cs == D::ClearState::Cleared) g_clearing = false;
        }
        // The commit of the land settle closes the carry; E is then a pile again.
        void* pile = LivePile();
        if (!g_clearing && pile && !TC::IsCarrying(e) && !TC::HasPendingSettle(e) && E::TryGetActorLocation(pile, g_landedAt)) {
            g_landedMs = ms;
            g_nextSampleMs = ms;
            UE_LOGI("[PILE-REPILE] host re-piled eid=%u at (%.1f,%.1f,%.1f) (grabbed at (%.1f,%.1f,%.1f))", g_eid,
                    g_landedAt.X, g_landedAt.Y, g_landedAt.Z, g_grabbedAt.X, g_grabbedAt.Y, g_grabbedAt.Z);
            g_step = Step::Watch;
            return;
        }
        if (ms - g_stepMs >= kStepBoundMs) Fail("land");
        return;
    }
    case Step::Watch:
        TickDrift(ms);
        TickRejoin(s, ms);
        return;
    default:
        return;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    if (!IsEnabled_()) return;
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!IsEnabled_()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Host) return;
    if (!g_saidArm) {
        g_saidArm = true;
        UE_LOGI("[PILE-REPILE] host armed");
    }
    TickHost(s, ::GetTickCount64());
}

void OnDisconnect() {
    g_saidArm = false;
    g_step = Step::WaitJoin;
    g_stepMs = 0;
    g_eid = 0;
    g_grabbedAt = ue_wrap::FVector{};
    g_walk.reset();
    g_grab.reset();
    g_clear = D::ClearHand{};
    g_clearing = false;
    g_sawCarry = false;
    g_landedAt = ue_wrap::FVector{};
    g_landedMs = g_nextSampleMs = 0;
    g_driftDone = false;
    g_sawLeave = false;
    g_rejoinedMs = 0;
}

}  // namespace coop::dev::pile_repile_drill
