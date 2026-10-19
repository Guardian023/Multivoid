// coop/dev/swinger_drill.cpp -- see coop/dev/swinger_drill.h.

#include "coop/dev/swinger_drill.h"

#include "coop/config/config.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/dev/toggle_drill.h"
#include "coop/interactables/interactable_sync.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/swinger.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/engine_physics.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace coop::dev::swinger_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace PR = ue_wrap::prop;
namespace SW = ue_wrap::swinger;
using Clock = std::chrono::steady_clock;

enum class Arm { Off, Measure, Observe };
enum class Step { WaitReady, Pick, WalkTo, Grab, Drag, Release, Tail, Done, Invalid };

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::swinger_drill);
        return v == "measure" ? Arm::Measure : v == "observe" ? Arm::Observe : Arm::Off;
    }();
    return a;
}

// The acting peer: the client of the measure arm. The host and the observe arm only watch.
bool Acts() { return ArmOf() == Arm::Measure && !coop::roster::LocalIsHost(); }
const char* Role() {
    return coop::roster::LocalIsHost() ? "host" : ArmOf() == Arm::Observe ? "observer" : "client";
}

constexpr int   kPickRoutes     = 6;      // the nearest lids whose route is asked (each is up to 16 path queries)
constexpr float kRouteEndReachCm = 160.f; // a route that ends further from the lid reaches no lid
constexpr float kReachCm        = 130.f;  // the director's stop distance from the lid, level
constexpr int   kWalkDeadlineS  = 300;    // a long first walk from a New Game start (the fireext drill's)
constexpr float kMousePerTick   = 3.f;    // the fed mouse delta, both axes, one direction
constexpr int   kDragTicks      = 180;    // about three seconds of feeds at 60 ticks a second
constexpr auto  kScanEvery      = std::chrono::milliseconds(100);
constexpr auto  kSampleEvery    = std::chrono::milliseconds(250);
constexpr auto  kTail           = std::chrono::seconds(5);
constexpr auto  kReportEvery    = std::chrono::seconds(1);
constexpr float kMovedDeg       = 0.5f;   // a mesh rotation that changed by more than this has moved

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float RotDelta(const ue_wrap::FRotator& a, const ue_wrap::FRotator& b) {
    return std::fabs(ue_wrap::NormalizeAxis(a.Pitch - b.Pitch)) + std::fabs(ue_wrap::NormalizeAxis(a.Yaw - b.Yaw)) +
           std::fabs(ue_wrap::NormalizeAxis(a.Roll - b.Roll));
}

// ---- The lids: every peer watches every non-lockable lid its world holds ------------------------------

struct Reading {
    bool ok = false;                       // the actor's transform read
    ue_wrap::FVector loc{};
    ue_wrap::FRotator actorRot{}, meshRel{}, meshWorld{};
    bool relOk = false;
    int opened = -1, sim = -1;             // -1: unread
};

struct Lid {
    ue_wrap::CachedObjRef ref;
    std::wstring laneKey, propKey;
    ue_wrap::FRotator scanned{};           // the mesh's world rotation at the last scan
    bool tracked = false;
    Clock::time_point lastMoveAt{}, nextSample{};
};

std::vector<Lid> g_lids;
bool g_armed = false;
Clock::time_point g_armedAt{}, g_nextScan{}, g_lastAnyMove{};
bool g_quiet = false;        // no lid moved for a tail since the arm: moves from here on are the run's
bool g_windowOpen = false;   // a lid has moved since the quiet
bool g_doneSaid = false;

Reading Read(void* d) {
    Reading r;
    r.ok = E::TryGetActorLocation(d, r.loc) && E::TryGetActorRotation(d, r.actorRot);
    if (void* mesh = PR::GetStaticMesh(d)) {
        r.meshWorld = E::GetComponentWorldRotation(mesh);
        r.sim = E::IsComponentSimulatingPhysics(mesh) ? 1 : 0;
        r.relOk = SW::TryReadMeshRelativeRotation(d, r.meshRel);
    }
    bool open = false;
    if (SW::TryReadOpen(d, open)) r.opened = open ? 1 : 0;
    return r;
}

void Say(const char* what, const Lid& l, const Reading& r) {
    UE_LOGI("[SWING-DRILL] %s %s key='%ls': actor rot (%.2f, %.2f, %.2f) loc (%.1f, %.1f, %.1f) | mesh rel rot "
            "(%.2f, %.2f, %.2f) world rot (%.2f, %.2f, %.2f) | opened %d simulating %d",
            Role(), what, l.laneKey.c_str(), r.actorRot.Pitch, r.actorRot.Yaw, r.actorRot.Roll, r.loc.X, r.loc.Y,
            r.loc.Z, r.meshRel.Pitch, r.meshRel.Yaw, r.meshRel.Roll, r.meshWorld.Pitch, r.meshWorld.Yaw,
            r.meshWorld.Roll, r.opened, r.sim);
}

// One walk of the object array, when the drill arms, never again.
void ArmWatch() {
    g_lids.clear();
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o) || !coop::dev::toggle_drill::IsSteadyLid(o)) continue;
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;
        Lid l;
        l.ref.Set(o);
        l.laneKey = coop::interactable_sync::ContainerKey(o);
        l.propKey = PR::GetInteractableKeyString(o);
        const Reading r = Read(o);
        l.scanned = r.meshWorld;
        g_lids.push_back(std::move(l));
        Say("lid", g_lids.back(), r);
        void* mesh = PR::GetStaticMesh(o);
        UE_LOGI("[SWING-DRILL] %s lid key='%ls' prop key='%ls' mesh=%p root=%p (the root is %s the mesh)", Role(),
                g_lids.back().laneKey.c_str(), g_lids.back().propKey.c_str(), mesh, E::RootComponentOf(o),
                mesh && E::RootComponentOf(o) == mesh ? "also" : "NOT");
    }
    g_armed = true;
    g_armedAt = g_lastAnyMove = g_nextScan = Clock::now();
    UE_LOGI("[SWING-DRILL] %s armed: %zu non-lockable lid(s) watched", Role(), g_lids.size());
}

// Held-prop receive counters since the last line (the host and the observer).
struct Rx {
    uint32_t door = 0, other = 0, unresolved = 0;
    int slot = -1;
    std::wstring key;
    uint32_t eid = 0;
    void* actor = nullptr;
    void* mesh = nullptr;
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
};
Rx g_rx;

void PrintRx() {
    if (g_rx.door == 0 && g_rx.other == 0 && g_rx.unresolved == 0) return;
    char now[160] = "";
    if (g_rx.actor && R::IsLive(g_rx.actor)) {
        ue_wrap::FVector l{};
        ue_wrap::FRotator r{};
        if (E::TryGetActorLocation(g_rx.actor, l) && E::TryGetActorRotation(g_rx.actor, r))
            snprintf(now, sizeof(now), " | the actor now: loc (%.1f, %.1f, %.1f) rot (%.2f, %.2f, %.2f) mesh simulating %d",
                     l.X, l.Y, l.Z, r.Pitch, r.Yaw, r.Roll, g_rx.mesh && E::IsComponentSimulatingPhysics(g_rx.mesh) ? 1 : 0);
    }
    UE_LOGI("[SWING-DRILL] %s propPose rx: to a swinger %u, to another prop %u, unresolved %u | last slot %d key='%ls' "
            "eid=%u -> the ACTOR %p (SetActorLocation + SetActorRotation on its root, mesh %p) target loc (%.1f, %.1f, "
            "%.1f) rot (%.2f, %.2f, %.2f)%s",
            Role(), g_rx.door, g_rx.other, g_rx.unresolved, g_rx.slot, g_rx.key.c_str(), g_rx.eid, g_rx.actor,
            g_rx.mesh, g_rx.loc.X, g_rx.loc.Y, g_rx.loc.Z, g_rx.rot.Pitch, g_rx.rot.Yaw, g_rx.rot.Roll, now);
    g_rx.door = g_rx.other = g_rx.unresolved = 0;
}

// Every lid every scan; a lid that moved is sampled every 250 ms until a tail after its last move. The
// non-acting peers end on the first quiet after a move that came after the world's own settling.
void TickWatch(Clock::time_point now) {
    if (now >= g_nextScan) {
        g_nextScan = now + kScanEvery;
        for (Lid& l : g_lids) {
            void* d = l.ref.Get();
            void* mesh = d ? PR::GetStaticMesh(d) : nullptr;
            if (!mesh) continue;
            const ue_wrap::FRotator w = E::GetComponentWorldRotation(mesh);
            if (RotDelta(w, l.scanned) <= kMovedDeg) continue;
            l.scanned = w;
            l.lastMoveAt = g_lastAnyMove = now;
            if (!l.tracked) {
                l.tracked = true;
                l.nextSample = now;
                if (g_quiet) {
                    g_windowOpen = true;
                    g_doneSaid = false;
                }
            }
        }
        if (!g_quiet && now - g_lastAnyMove >= kTail) {
            g_quiet = true;
            UE_LOGI("[SWING-DRILL] %s quiet: no lid moved for %lld s since the arm; moves from here are the run's",
                    Role(), static_cast<long long>(kTail.count()));
        }
    }
    bool anyTracked = false;
    for (Lid& l : g_lids) {
        if (!l.tracked) continue;
        void* d = l.ref.Get();
        if (!d) { l.tracked = false; continue; }
        if (now >= l.nextSample) {
            l.nextSample = now + kSampleEvery;
            Say("sample", l, Read(d));
        }
        if (now - l.lastMoveAt > kTail) {
            l.tracked = false;
            Say("settled", l, Read(d));
        } else {
            anyTracked = true;
        }
    }
    static Clock::time_point nextRx{};
    if (now >= nextRx) {
        nextRx = now + kSampleEvery;
        PrintRx();
    }
    if (!Acts() && g_windowOpen && !anyTracked && !g_doneSaid) {
        g_doneSaid = true;
        PrintRx();
        UE_LOGI("[SWING-DRILL] %s DONE", Role());
    }
}

// ---- The acting client ---------------------------------------------------------------------------------

Step g_step = Step::WaitReady;
int g_stepTicks = 0, g_dragTicks = 0;
ue_wrap::CachedObjRef g_target;
size_t g_targetLid = 0;
ue_wrap::FVector g_targetAt{};
std::shared_ptr<coop::director::BackgroundWalk> g_walk;
std::unique_ptr<coop::director::AimedGrab> g_grab;
Clock::time_point g_tailStart{}, g_nextReport{};

// The held-prop stream's side, filled by local_streams through the Note* seams.
uint32_t g_heldTicks = 0, g_ticks = 0, g_poseEmits = 0, g_edgeEid = 0, g_poseEid = 0;
bool g_edgeSeen = false, g_edgeBroadcast = false;
std::wstring g_poseKey, g_otherHeldCls;
void* g_lastOtherHeld = nullptr;   // compared as an address only

const char* StepName() {
    static const char* const n[] = {"WaitReady", "Pick", "WalkTo", "Grab", "Drag", "Release", "Tail", "Done", "Invalid"};
    return n[static_cast<int>(g_step)];
}

void Invalid(const char* why) {
    UE_LOGW("[SWING-DRILL] client INVALID %s", why);
    g_step = Step::Invalid;
}

void Go(Step s) {
    g_step = s;
    g_stepTicks = 0;
}

void LogTarget(const char* what) {
    void* d = g_target.Get();
    if (!d) { UE_LOGI("[SWING-DRILL] client %s -- the door is gone", what); return; }
    Say(what, g_lids[g_targetLid], Read(d));
}

// The nearest lid a NavMesh route reaches, by route length among the nearest few by straight line.
bool PickTarget(void* player, const char*& why) {
    ue_wrap::FVector me{};
    if (!E::TryGetActorLocation(player, me)) { why = "the player's location could not be read"; return false; }
    why = "no non-lockable lid among the nearest few that a route reaches";
    std::vector<std::pair<float, size_t>> order;
    for (size_t i = 0; i < g_lids.size(); ++i) {
        void* d = g_lids[i].ref.Get();
        ue_wrap::FVector at{};
        if (d && E::TryGetActorLocation(d, at)) order.emplace_back(Dist(me, at), i);
    }
    std::sort(order.begin(), order.end());
    if (static_cast<int>(order.size()) > kPickRoutes) order.resize(kPickRoutes);
    float best = 1e30f;
    for (const auto& o : order) {
        void* d = g_lids[o.second].ref.Get();
        ue_wrap::FVector centre{}, extent{};
        if (!d || (!E::GetActorBounds(d, /*onlyColliding=*/true, centre, extent) && !E::TryGetActorLocation(d, centre)))
            continue;
        float len = 0.f;
        if (!coop::director::RouteInLegs(player, me, centre, kRouteEndReachCm, &len)) continue;
        if (len < best) { best = len; g_target.Set(d); g_targetLid = o.second; g_targetAt = centre; }
    }
    if (!g_target.Raw()) return false;
    UE_LOGI("[SWING-DRILL] client target key='%ls' prop key='%ls' at (%.1f, %.1f, %.1f), a %.0f cm route (%.0f cm "
            "straight)", g_lids[g_targetLid].laneKey.c_str(), g_lids[g_targetLid].propKey.c_str(), g_targetAt.X,
            g_targetAt.Y, g_targetAt.Z, best, Dist(g_targetAt, me));
    return true;
}

// Once a second while the door is dragged and after: what the held-prop stream says of it.
void ReportHeld(Clock::time_point now) {
    if (now < g_nextReport) return;
    g_nextReport = now + kReportEvery;
    UE_LOGI("[SWING-DRILL] client held (%s): local_streams named the door the held actor on %u of the last %u ticks | "
            "new-hold edge seen %d, held-item broadcast %d, eid %u | held poses handed to the session %u, last key "
            "'%ls' eid %u | another held actor: '%ls'",
            StepName(), g_heldTicks, g_ticks, g_edgeSeen ? 1 : 0, g_edgeBroadcast ? 1 : 0, g_edgeEid, g_poseEmits,
            g_poseKey.c_str(), g_poseEid, g_otherHeldCls.c_str());
    g_heldTicks = g_ticks = 0;
}

void TickActor(Clock::time_point now) {
    if (g_step == Step::Invalid || g_step == Step::Done) return;
    void* player = coop::players::Registry::Get().Local();
    if (!player || !R::IsLive(player) || !E::GetController(player)) return;
    ++g_stepTicks;
    if (static_cast<int>(g_step) >= static_cast<int>(Step::Grab)) { ++g_ticks; ReportHeld(now); }
    switch (g_step) {
    case Step::WaitReady:
        Go(Step::Pick);
        return;
    case Step::Pick: {
        const char* why = "";
        if (!PickTarget(player, why)) { Invalid(why); return; }
        g_walk = coop::director::StartBackgroundWalk(g_targetAt, kReachCm, kWalkDeadlineS, /*carry=*/false);
        Go(Step::WalkTo);
        return;
    }
    case Step::WalkTo: {
        const int st = g_walk ? g_walk->state.load() : 2;
        if (st == 0) return;
        if (st == 2) { Invalid("the walk to the lid did not arrive"); return; }
        LogTarget("before-grab");
        g_grab = std::make_unique<coop::director::AimedGrab>(player, g_target.Get());
        Go(Step::Grab);
        return;
    }
    case Step::Grab: {
        if (!g_target.Get()) { Invalid("the door died before the grab"); return; }
        const coop::director::GrabState gs = g_grab->Tick();
        if (gs == coop::director::GrabState::Working) return;
        UE_LOGI("[SWING-DRILL] client the trace took the door after %d fan pose(s); grab chain: playerGrabbed_pre=%d "
                "useAction=%d playerGrabbed=%d", g_grab->AimPoses(), g_grab->ChainPre() ? 1 : 0,
                g_grab->ChainUse() ? 1 : 0, g_grab->ChainPost() ? 1 : 0);
        if (gs == coop::director::GrabState::Failed) { Invalid(g_grab->Why()); return; }
        Lid& l = g_lids[g_targetLid];
        l.tracked = true;
        l.lastMoveAt = l.nextSample = now;
        LogTarget("grabbed");
        g_dragTicks = 0;
        Go(Step::Drag);
        return;
    }
    case Step::Drag: {
        if (coop::director::Grabbing(player) != g_target.Get()) {
            UE_LOGW("[SWING-DRILL] client the hand lost the door after %d feed(s)", g_dragTicks);
            Go(Step::Release);
            return;
        }
        int delivered = 0;
        if (!E::BroadcastMainPlayerMouseXY(player, kMousePerTick, kMousePerTick, &delivered)) {
            Invalid("the player's input_mouseXY dispatcher did not resolve");
            return;
        }
        if (g_dragTicks == 0 || g_dragTicks % 60 == 0)
            UE_LOGI("[SWING-DRILL] client feed #%d: (%.1f, %.1f) ran %d bound entr%s", g_dragTicks, kMousePerTick,
                    kMousePerTick, delivered, delivered == 1 ? "y" : "ies");
        g_lids[g_targetLid].lastMoveAt = now;
        if (++g_dragTicks >= kDragTicks) Go(Step::Release);
        return;
    }
    case Step::Release:
        coop::director::CallOnPlayer(player, L"dropGrabObject");
        LogTarget("released");
        g_lids[g_targetLid].lastMoveAt = g_tailStart = now;
        Go(Step::Tail);
        return;
    case Step::Tail:
        if (now - g_tailStart < kTail) return;
        ReportHeld(now);
        PrintRx();
        UE_LOGI("[SWING-DRILL] client DONE");
        g_step = Step::Done;
        g_walk.reset();
        g_grab.reset();
        return;
    case Step::Done:
    case Step::Invalid:
        return;
    }
}

}  // namespace

bool IsEnabled() { return ArmOf() != Arm::Off; }

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || !session || !session->connected()) return;
    if (!SW::EnsureResolved()) return;
    if (!g_armed) {
        // The host's world is up at once; a client's once its join is over.
        if (!coop::roster::LocalIsHost() &&
            (!coop::net_pump::HasAnnouncedWorldReady() ||
             coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle))
            return;
        ArmWatch();
    }
    const Clock::time_point now = Clock::now();
    TickWatch(now);
    if (Acts()) TickActor(now);
}

void OnDisconnect() {
    g_lids.clear();
    g_armed = false;
    g_quiet = g_windowOpen = g_doneSaid = false;
    g_step = Step::WaitReady;
    g_stepTicks = g_dragTicks = 0;
    g_target.Reset();
    g_walk.reset();
    g_grab.reset();
    g_rx = Rx{};
    g_heldTicks = g_ticks = g_poseEmits = g_edgeEid = g_poseEid = 0;
    g_edgeSeen = g_edgeBroadcast = false;
    g_poseKey.clear();
    g_otherHeldCls.clear();
    g_lastOtherHeld = nullptr;
}

void NoteHeldActor(void* heldActor) {
    if (!IsEnabled() || !heldActor) return;
    if (heldActor == g_target.Raw()) {
        ++g_heldTicks;
    } else if (heldActor != g_lastOtherHeld) {
        g_lastOtherHeld = heldActor;
        g_otherHeldCls = R::ClassNameOf(heldActor);
    }
}

void NoteHeldEdge(void* heldActor, bool broadcast, uint32_t eid) {
    if (!IsEnabled() || !heldActor || heldActor != g_target.Raw()) return;
    g_edgeSeen = true;
    g_edgeBroadcast = broadcast;
    g_edgeEid = eid;
}

void NotePoseEmit(void* heldActor, const std::wstring& key, uint32_t eid) {
    if (!IsEnabled() || !heldActor || heldActor != g_target.Raw()) return;
    ++g_poseEmits;
    g_poseKey = key;
    g_poseEid = eid;
}

void NotePropPose(int slot, const std::wstring& key, uint32_t eid, void* actor, void* mesh,
                  const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot) {
    if (!IsEnabled()) return;
    if (SW::IsSwinger(actor)) ++g_rx.door;
    else ++g_rx.other;
    g_rx.slot = slot;
    g_rx.key = key;
    g_rx.eid = eid;
    g_rx.actor = actor;
    g_rx.mesh = mesh;
    g_rx.loc = loc;
    g_rx.rot = rot;
}

void NotePropUnresolved(int slot, const std::wstring& key, uint32_t eid) {
    if (!IsEnabled()) return;
    ++g_rx.unresolved;
    g_rx.slot = slot;
    g_rx.key = key;
    g_rx.eid = eid;
    g_rx.actor = nullptr;
}

}  // namespace coop::dev::swinger_drill
