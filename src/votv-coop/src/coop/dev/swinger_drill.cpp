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
enum class Step { WaitReady, Pick, WalkTo, Grab, Drag, Release, Tail, Regrab, Settle, Last, Done, Invalid };

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::swinger_drill);
        return v == "measure" ? Arm::Measure : v == "observe" ? Arm::Observe : Arm::Off;
    }();
    return a;
}

// The acting peer: the first client of the measure arm. The host, the observe arm and any later client only watch.
bool Acts() {
    return ArmOf() == Arm::Measure && !coop::roster::LocalIsHost() &&
           coop::players::Registry::Get().LocalPeerId() <= 1;
}
const char* Role() {
    if (coop::roster::LocalIsHost()) return "host";
    return ArmOf() == Arm::Observe || coop::players::Registry::Get().LocalPeerId() > 1 ? "observer" : "client";
}

constexpr int   kPickRoutes     = 6;      // the nearest lids whose route is asked (each is up to 16 path queries)
constexpr float kRouteEndReachCm = 160.f; // a route that ends further from the lid reaches no lid
constexpr float kReachCm        = 130.f;  // the director's stop distance from the lid, level
constexpr int   kWalkDeadlineS  = 300;    // a long first walk from a New Game start (the fireext drill's)
// The drag: the client turns its own door, held, kTurnDeg about its hinge over kTurnTime, and holds it there for
// kHoldTime before it lets go.
constexpr float kTurnDeg        = 30.f;
constexpr auto  kTurnTime       = std::chrono::milliseconds(1500);
constexpr auto  kHoldTime       = std::chrono::milliseconds(1000);
constexpr int   kSettleTicks    = 30;     // the second hold's ticks: the client's settled pose crosses
constexpr auto  kLastWait       = std::chrono::seconds(1);
constexpr auto  kScanEvery      = std::chrono::milliseconds(100);
constexpr auto  kSampleEvery    = std::chrono::milliseconds(250);
constexpr auto  kTail           = std::chrono::seconds(5);
constexpr auto  kReportEvery    = std::chrono::seconds(1);
constexpr float kMovedDeg       = 0.5f;   // a mesh rotation that changed by more than this has moved
// The verdict. The client's door must turn kSwingDeg or the run is INVALID; a peer passes when its own door turned
// kFollowDeg during the drag and rests within kSettleDeg of where the client's door rested.
constexpr float kSwingDeg       = 15.f;
constexpr float kFollowDeg      = 10.f;
constexpr float kSettleDeg      = 3.f;
constexpr auto  kHoldGap        = std::chrono::seconds(3);    // no pose for this long: the first hold is over
constexpr auto  kJudgeBound     = std::chrono::seconds(45);   // the second hold's pose never came

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

struct Quat { float x, y, z, w; };

// The unit quaternion of a rotator, the engine's own conversion.
Quat QuatOf(const ue_wrap::FRotator& r) {
    Quat q{};
    E::RotatorToQuat(r.Pitch, r.Yaw, r.Roll, q.x, q.y, q.z, q.w);
    return q;
}

// The angle between two orientations, degrees: how far a door turned, whatever axis it turned about.
float AngleDeg(const ue_wrap::FRotator& a, const ue_wrap::FRotator& b) {
    const Quat p = QuatOf(a), q = QuatOf(b);
    const float dot = std::fabs(p.x * q.x + p.y * q.y + p.z * q.z + p.w * q.w);
    return 2.f * std::acos(dot > 1.f ? 1.f : dot) * (180.f / 3.14159265f);
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
    ue_wrap::FRotator scanned{};           // the mesh's world rotation at the last scan that saw it move
    ue_wrap::FRotator restRot{};           // its rotation while the lid was not moving: where a move starts from
    float maxDev = 0.f;                    // the largest angle from restRot since the lid last started moving
    bool tracked = false;
    Clock::time_point lastMoveAt{}, nextSample{};
};

std::vector<Lid> g_lids;
bool g_armed = false;
Clock::time_point g_nextScan{};

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
        if (R::NameStartsWith(R::NameOf(o), L"Default__") || !PR::GetStaticMesh(o)) continue;   // a lid has a mesh
        Lid l;
        l.ref.Set(o);
        l.laneKey = coop::interactable_sync::ContainerKey(o);
        l.propKey = PR::GetInteractableKeyString(o);
        const Reading r = Read(o);
        l.scanned = l.restRot = r.meshWorld;
        g_lids.push_back(std::move(l));
        Say("lid", g_lids.back(), r);
        void* mesh = PR::GetStaticMesh(o);
        UE_LOGI("[SWING-DRILL] %s lid key='%ls' prop key='%ls' mesh=%p root=%p (the root is %s the mesh)", Role(),
                g_lids.back().laneKey.c_str(), g_lids.back().propKey.c_str(), mesh, E::RootComponentOf(o),
                mesh && E::RootComponentOf(o) == mesh ? "also" : "NOT");
    }
    g_armed = true;
    g_nextScan = Clock::now();
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

// ---- The verdict of a watching peer -------------------------------------------------------------------
//
// What the client's door did is read from the poses this peer takes for it (the held-item stream carries the
// client's own door transform every tick), so no message beyond the stream is needed. The first pose opens the
// first hold; a pose after kHoldGap of silence is the client's second hold, taken once the door has settled
// (client DONE's regrab), and carries the client's settled rotation. The verdict is read there.

struct Verdict {
    bool started = false, said = false;
    Clock::time_point lastPose{};
    ue_wrap::FRotator first{};   // the client's door at the first pose
    float clientDev = 0.f;       // the largest angle of the client's door from `first`
    int lid = -1;                // this peer's copy of the door, once a pose resolved to it
    uint32_t resolved = 0, unresolved = 0;
};
Verdict g_v;

void Judge(const ue_wrap::FRotator& clientSettled, bool timedOut) {
    g_v.said = true;
    PrintRx();
    if (g_v.clientDev < kSwingDeg) {
        UE_LOGW("[SWING-DRILL] %s INVALID the client's door turned only %.1f deg on the stream (need %.0f)", Role(),
                g_v.clientDev, kSwingDeg);
    } else {
        const Lid* l = g_v.lid >= 0 ? &g_lids[static_cast<size_t>(g_v.lid)] : nullptr;
        const float hostDev = l ? l->maxDev : 0.f;
        const float settle = l && !timedOut ? AngleDeg(l->scanned, clientSettled) : 180.f;
        const bool pass = l && !timedOut && hostDev >= kFollowDeg && settle <= kSettleDeg;
        UE_LOGI("[SWING-DRILL] %s %s: follow -- this peer's door turned %.1f deg of the client's %.1f (need >= %.0f) | "
                "settle -- %.1f deg from the client's settled rotation%s (need <= %.0f) | poses resolved %u, "
                "unresolved %u", Role(), pass ? "PASS" : "FAIL", hostDev, g_v.clientDev, kFollowDeg, settle,
                timedOut ? " (the second hold's pose never came)" : "", kSettleDeg, g_v.resolved, g_v.unresolved);
    }
    UE_LOGI("[SWING-DRILL] %s DONE", Role());
}

// A PropPose from the client arrived: `lid` is this peer's copy of the door when it resolved to one, else -1.
void PoseArrived(const ue_wrap::FRotator& rot, int lid) {
    if (Acts() || g_v.said) return;
    const Clock::time_point now = Clock::now();
    if (lid >= 0) { g_v.lid = lid; ++g_v.resolved; } else { ++g_v.unresolved; }
    if (!g_v.started) {
        g_v.started = true;
        g_v.first = rot;
    } else if (now - g_v.lastPose >= kHoldGap) {
        g_v.lastPose = now;
        Judge(rot, false);
        return;
    } else {
        g_v.clientDev = (std::max)(g_v.clientDev, AngleDeg(rot, g_v.first));
    }
    g_v.lastPose = now;
}

// Every lid every scan; a lid that moved is sampled every 250 ms until a tail after its last move.
void TickWatch(Clock::time_point now) {
    if (now >= g_nextScan) {
        g_nextScan = now + kScanEvery;
        for (Lid& l : g_lids) {
            void* d = l.ref.Get();
            void* mesh = d ? PR::GetStaticMesh(d) : nullptr;
            if (!mesh) continue;
            const ue_wrap::FRotator w = E::GetComponentWorldRotation(mesh);
            if (l.tracked) l.maxDev = (std::max)(l.maxDev, AngleDeg(w, l.restRot));
            else if (AngleDeg(w, l.scanned) <= kMovedDeg) l.restRot = w;
            if (AngleDeg(w, l.scanned) <= kMovedDeg) continue;
            l.scanned = w;
            l.lastMoveAt = now;
            if (!l.tracked) {
                l.tracked = true;
                l.maxDev = AngleDeg(w, l.restRot);
                l.nextSample = now;
            }
        }
    }
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
        }
    }
    static Clock::time_point nextRx{};
    if (now >= nextRx) {
        nextRx = now + kSampleEvery;
        PrintRx();
    }
    // The second hold's pose never came: the verdict is read without it, and fails.
    if (!Acts() && g_v.started && !g_v.said && now - g_v.lastPose > kJudgeBound) Judge(ue_wrap::FRotator{}, true);
}

// ---- The acting client ---------------------------------------------------------------------------------

Step g_step = Step::WaitReady;
int g_stepTicks = 0, g_dragTicks = 0;
ue_wrap::CachedObjRef g_target;
size_t g_targetLid = 0;
ue_wrap::FVector g_targetAt{};
std::shared_ptr<coop::director::BackgroundWalk> g_walk;
std::unique_ptr<coop::director::AimedGrab> g_grab;
Clock::time_point g_tailStart{}, g_nextReport{}, g_lastStart{};
ue_wrap::FRotator g_grabRot{};   // the client's door at the grab: what its swing is measured from
float g_peakDev = 0.f;           // the largest angle the client's door has turned from g_grabRot

// The held-prop stream's side, filled by local_streams through the Note* seams.
uint32_t g_heldTicks = 0, g_ticks = 0, g_poseEmits = 0, g_edgeEid = 0, g_poseEid = 0;
bool g_edgeSeen = false, g_edgeBroadcast = false;
std::wstring g_poseKey, g_otherHeldCls;
void* g_lastOtherHeld = nullptr;   // compared as an address only

const char* StepName() {
    static const char* const n[] = {"WaitReady", "Pick", "WalkTo", "Grab", "Drag", "Release", "Tail", "Regrab",
                                    "Settle", "Last", "Done", "Invalid"};
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

// ---- The drag --------------------------------------------------------------------------------------------
//
// The drill turns the client's own door itself while the client holds it: an injected mouse feed was measured not
// to swing this door (its own graph's torque never moved it), and what is proved here is that the held-item
// stream carries a dragged door's motion and the receivers follow it. The game's own drag with a real mouse is a
// hands-on check. The mesh goes kinematic and is turned about its hinge from its rest pose; the held-item stream
// then carries those poses as it carries any held actor's.

struct Turn {
    bool on = false;                          // the mesh is kinematic, and turned by this drill
    ue_wrap::FVector restLoc{}, pivot{}, axis{0.f, 0.f, 1.f};
    ue_wrap::FRotator restRot{};
    Clock::time_point start{};
};
Turn g_turn;

Quat Mul(const Quat& a, const Quat& b) {   // Hamilton product: b first, then a
    return {a.w * b.x + b.w * a.x + a.y * b.z - a.z * b.y, a.w * b.y + b.w * a.y + a.z * b.x - a.x * b.z,
            a.w * b.z + b.w * a.z + a.x * b.y - a.y * b.x, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
ue_wrap::FVector Rotate(const Quat& q, const ue_wrap::FVector& v) {
    const float tx = 2.f * (q.y * v.Z - q.z * v.Y), ty = 2.f * (q.z * v.X - q.x * v.Z),
                tz = 2.f * (q.x * v.Y - q.y * v.X);
    return {v.X + q.w * tx + (q.y * tz - q.z * ty), v.Y + q.w * ty + (q.z * tx - q.x * tz),
            v.Z + q.w * tz + (q.x * ty - q.y * tx)};
}

// Take the door's rest pose and its hinge, then stop its simulation. The pivot is the PhysicsConstraint component's
// world location and the axis the door's own getAxis() (ue_wrap/actors/swinger); a hinge part that does not read
// falls back to the root's own origin and the root's up vector, said in the log.
bool BeginTurn(void* door, Clock::time_point now) {
    void* mesh = PR::GetStaticMesh(door);
    if (!mesh || !E::TryGetActorLocation(door, g_turn.restLoc) || !E::TryGetActorRotation(door, g_turn.restRot))
        return false;
    SW::HingeFrame h;
    SW::ReadHinge(door, h);
    g_turn.pivot = h.pivotRead ? h.pivot : g_turn.restLoc;
    g_turn.axis = h.axisRead ? h.axis : E::GetActorUpVector(door);
    UE_LOGI("[SWING-DRILL] client hinge: pivot (%.1f, %.1f, %.1f) from %s | axis (%.3f, %.3f, %.3f) from %s",
            g_turn.pivot.X, g_turn.pivot.Y, g_turn.pivot.Z,
            h.pivotRead ? "the PhysicsConstraint component" : "NOTHING READ: the root's own origin",
            g_turn.axis.X, g_turn.axis.Y, g_turn.axis.Z,
            h.axisRead ? "getAxis()" : "NOTHING READ: the root's up vector");
    E::SetComponentSimulatePhysics(mesh, false);
    g_turn.on = true;
    g_turn.start = now;
    return true;
}

// The door `deg` degrees about its hinge from its rest pose: the root's location swings round the pivot and its
// rotation takes the same turn.
void ApplyTurn(void* door, float deg) {
    const float half = deg * (3.14159265f / 360.f), s = std::sin(half);
    const ue_wrap::FVector a = g_turn.axis;
    const float len = std::sqrt(a.X * a.X + a.Y * a.Y + a.Z * a.Z);
    if (len < 1e-4f) return;
    const Quat turn{a.X / len * s, a.Y / len * s, a.Z / len * s, std::cos(half)};
    const Quat q = Mul(turn, QuatOf(g_turn.restRot));
    const ue_wrap::FVector rel{g_turn.restLoc.X - g_turn.pivot.X, g_turn.restLoc.Y - g_turn.pivot.Y,
                               g_turn.restLoc.Z - g_turn.pivot.Z};
    const ue_wrap::FVector r = Rotate(turn, rel);
    E::SetActorLocation(door, ue_wrap::FVector{g_turn.pivot.X + r.X, g_turn.pivot.Y + r.Y, g_turn.pivot.Z + r.Z});
    E::SetActorRotation(door, E::QuatToRotator(q.x, q.y, q.z, q.w));
}

// Physics back on, so the door's own spring settles it from where it was let go.
void EndTurn(void* door) {
    if (!g_turn.on) return;
    g_turn.on = false;
    if (void* mesh = door ? PR::GetStaticMesh(door) : nullptr) E::SetComponentSimulatePhysics(mesh, true);
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
        g_grabRot = Read(g_target.Get()).meshWorld;
        g_peakDev = 0.f;
        LogTarget("grabbed");
        g_dragTicks = 0;
        Go(Step::Drag);
        return;
    }
    case Step::Drag: {
        void* d = g_target.Get();
        if (!d || coop::director::Grabbing(player) != d) {
            UE_LOGW("[SWING-DRILL] client the hand lost the door after %d tick(s) of the drag", g_dragTicks);
            Go(Step::Release);
            return;
        }
        if (!g_turn.on && !BeginTurn(d, now)) { Invalid("the door has no mesh or no readable transform to turn"); return; }
        const auto elapsed = now - g_turn.start;
        const float frac = elapsed >= kTurnTime ? 1.f
            : std::chrono::duration<float>(elapsed).count() / std::chrono::duration<float>(kTurnTime).count();
        ApplyTurn(d, kTurnDeg * frac);
        if (void* mesh = PR::GetStaticMesh(d))
            g_peakDev = (std::max)(g_peakDev, AngleDeg(E::GetComponentWorldRotation(mesh), g_grabRot));
        g_lids[g_targetLid].lastMoveAt = now;
        ++g_dragTicks;
        if (elapsed >= kTurnTime + kHoldTime) Go(Step::Release);
        return;
    }
    case Step::Release: {
        coop::director::CallOnPlayer(player, L"dropGrabObject");
        EndTurn(g_target.Get());
        LogTarget("released");
        g_lids[g_targetLid].lastMoveAt = g_tailStart = now;
        if (g_peakDev < kSwingDeg) {
            char why[160];
            snprintf(why, sizeof(why), "the client's own door turned only %.1f deg (need %.0f)", g_peakDev, kSwingDeg);
            Invalid(why);
            return;
        }
        UE_LOGI("[SWING-DRILL] client released: the door turned %.1f deg at its peak", g_peakDev);
        Go(Step::Tail);
        return;
    }
    case Step::Tail:
        if (now - g_tailStart < kTail) return;
        // The second hold: the door has settled, and held again with no feed its pose crosses on the stream, which
        // is the client's settled rotation every watching peer compares its own door to.
        g_grab = std::make_unique<coop::director::AimedGrab>(player, g_target.Get());
        Go(Step::Regrab);
        return;
    case Step::Regrab: {
        if (!g_target.Get()) { Invalid("the door died before the second hold"); return; }
        const coop::director::GrabState gs = g_grab->Tick();
        if (gs == coop::director::GrabState::Working) return;
        if (gs == coop::director::GrabState::Failed) { Invalid(g_grab->Why()); return; }
        LogTarget("regrabbed");
        Go(Step::Settle);
        return;
    }
    case Step::Settle:
        if (g_stepTicks < kSettleTicks) return;
        LogTarget("settled");
        coop::director::CallOnPlayer(player, L"dropGrabObject");
        g_lastStart = now;
        Go(Step::Last);
        return;
    case Step::Last:
        if (now - g_lastStart < kLastWait) return;
        ReportHeld(now);
        PrintRx();
        UE_LOGI("[SWING-DRILL] client DONE: the door turned %.1f deg at its peak", g_peakDev);
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
    g_v = Verdict{};
    g_step = Step::WaitReady;
    g_stepTicks = g_dragTicks = 0;
    g_peakDev = 0.f;
    g_turn = Turn{};
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
    int lid = -1;
    for (size_t i = 0; i < g_lids.size(); ++i)
        if (g_lids[i].ref.Raw() == actor) lid = static_cast<int>(i);
    if (lid >= 0) ++g_rx.door;
    else ++g_rx.other;
    g_rx.slot = slot;
    g_rx.key = key;
    g_rx.eid = eid;
    g_rx.actor = actor;
    g_rx.mesh = mesh;
    g_rx.loc = loc;
    g_rx.rot = rot;
    if (lid >= 0) PoseArrived(rot, lid);   // a pose for another prop is not the door's
}

void NotePropUnresolved(int slot, const std::wstring& key, uint32_t eid, const ue_wrap::FRotator& rot) {
    if (!IsEnabled()) return;
    ++g_rx.unresolved;
    g_rx.slot = slot;
    g_rx.key = key;
    g_rx.eid = eid;
    g_rx.actor = nullptr;
    PoseArrived(rot, -1);
}

}  // namespace coop::dev::swinger_drill
