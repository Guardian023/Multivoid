// coop/dev/swinger_drill.cpp -- see coop/dev/swinger_drill.h.

#include "coop/dev/swinger_drill.h"

#include "coop/comms/chat_feed.h"
#include "coop/comms/chat_sync.h"
#include "coop/config/config.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/dev/drill_chat_pace.h"
#include "coop/element/portable_identity.h"
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
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
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
enum class Step { Pick, WalkTo, Grab, Drag, Release, Tail, Announce, Regrab, Settle, Last, Echo, Done, Invalid };

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
// The feed (see the drag below): it starts at kFeedStart per axis and doubles every kFeedEvery* ticks, kFeedDoublings
// times, to the cap; once the client's own door has turned kSwingDeg it holds for kHoldAfterSwingTicks more.
constexpr float kFeedStart      = 3.f;
constexpr int   kFeedDoublings  = 11;     // 3 * 2^11 = 6144
constexpr int   kFeedEveryFirst = 30;
constexpr int   kFeedEveryLater = 10;
constexpr int   kHoldAfterSwingTicks = 60;
constexpr auto  kScanEvery      = std::chrono::milliseconds(100);
constexpr auto  kSampleEvery    = std::chrono::milliseconds(250);
constexpr auto  kTail           = std::chrono::seconds(5);   // how long a lid that stopped moving is still sampled
constexpr auto  kReportEvery    = std::chrono::seconds(1);
constexpr float kMovedDeg       = 0.5f;   // a mesh rotation that changed by more than this has moved
constexpr float kRestDeg        = 0.05f;  // two reads this close are the same: the door has settled
constexpr int   kSecondHoldPoses = 3;     // poses the second hold hands to the session before it ends
constexpr const char* kTag      = "[swing-drill] ";   // the client's chat lines: grab, settled, end
// The verdict. The client's door must turn kSwingDeg or the run is INVALID; a peer passes when its own door turned
// kFollowDeg during the drag and rests within kSettleDeg of where the client's door rested.
constexpr float kSwingDeg       = 15.f;
constexpr float kFollowDeg      = 10.f;
constexpr float kSettleDeg      = 3.f;
constexpr auto  kJudgeBound     = std::chrono::seconds(60);   // a failure bound: the client's end line never came

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
    ue_wrap::FVector loc{};
    ue_wrap::FRotator actorRot{}, meshRel{}, meshWorld{};
    int opened = -1, sim = -1;             // -1: unread
};

struct Lid {
    ue_wrap::CachedObjRef ref;
    std::wstring laneKey, propKey, ident;   // the lane's key, the game's Key, the portable identity's readable form
    bool perProcessKey = false;            // a child of a keyed prop: its game Key differs between processes
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
    E::TryGetActorLocation(d, r.loc);
    E::TryGetActorRotation(d, r.actorRot);
    if (void* mesh = PR::GetStaticMesh(d)) {
        r.meshWorld = E::GetComponentWorldRotation(mesh);
        r.sim = E::IsComponentSimulatingPhysics(mesh) ? 1 : 0;
        SW::TryReadMeshRelativeRotation(d, r.meshRel);
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
        l.ident = coop::element::PortableIdentity(o);
        l.perProcessKey = l.ident.rfind(L"k:", 0) == 0;
        const Reading r = Read(o);
        l.scanned = l.restRot = r.meshWorld;
        g_lids.push_back(std::move(l));
        Say("lid", g_lids.back(), r);
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
    UE_LOGI("[SWING-DRILL] %s propPose rx: to a swinger %u, to another prop %u, unresolved %u | last slot %d key='%ls' "
            "eid=%u -> the ACTOR %p (SetActorLocation + SetActorRotation on its root, mesh %p, simulating %d) target "
            "loc (%.1f, %.1f, %.1f) rot (%.2f, %.2f, %.2f)",
            Role(), g_rx.door, g_rx.other, g_rx.unresolved, g_rx.slot, g_rx.key.c_str(), g_rx.eid, g_rx.actor,
            g_rx.mesh, g_rx.mesh && R::IsLive(g_rx.mesh) && E::IsComponentSimulatingPhysics(g_rx.mesh) ? 1 : 0,
            g_rx.loc.X, g_rx.loc.Y, g_rx.loc.Z, g_rx.rot.Pitch, g_rx.rot.Yaw, g_rx.rot.Roll);
    g_rx.door = g_rx.other = g_rx.unresolved = 0;
}

// ---- The client's chat lines ---------------------------------------------------------------------------
//
// The client marks its phases in the lobby chat, the one reliable channel every peer already reads: a line when it
// grabs the door (the lane's key for it and the game's own Key), one when its door has settled after the release,
// one when its second hold has ended. The poses are unreliable and carry no phase, so a watching peer judges on
// these lines and never on a gap in the stream. The host admits a client's chat at a fixed rate, so the lines go
// out one per kTalkEveryMs.

enum class Phase { Grab, Settled, End };
struct ChatEvent {
    unsigned long long key;
    Phase what;
    std::string mv, gameKey;   // a Grab line's two keys
};

std::vector<std::string> g_outbox;
size_t g_outNext = 0;
uint64_t g_nextSayMs = 0;
unsigned long long g_chatMark = 0;   // the feed's newest row when the drill armed: older rows are another run's
bool g_echoSettled = false, g_echoEnd = false;   // the client has seen its own line come back

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

void SayLine(const std::string& rest) { g_outbox.push_back(std::string(kTag) + rest); }

void PumpOutbox() {
    const uint64_t now = NowMs();
    if (g_outNext >= g_outbox.size() || now < g_nextSayMs) return;
    coop::chat_sync::QueueSend(g_outbox[g_outNext++]);
    g_nextSayMs = now + coop::dev::kTalkEveryMs;
    if (g_outNext == g_outbox.size()) { g_outbox.clear(); g_outNext = 0; }
}

void MarkChat() {
    coop::chat_feed::ForEachRow([](const coop::chat_feed::RowView& r) { g_chatMark = (std::max)(g_chatMark, r.key); });
}

// The drill's lines newer than the mark, oldest first.
std::vector<ChatEvent> NewChat() {
    std::vector<ChatEvent> out;
    unsigned long long top = g_chatMark;
    coop::chat_feed::ForEachRow([&](const coop::chat_feed::RowView& r) {
        const size_t at = r.line.find(kTag);
        if (r.key <= g_chatMark || at == std::string_view::npos) return;
        top = (std::max)(top, r.key);
        const std::string rest(r.line.substr(at + std::strlen(kTag)));
        ChatEvent e{r.key, Phase::End, {}, {}};
        if (rest.rfind("grab ", 0) == 0) {
            e.what = Phase::Grab;
            const size_t sp = rest.find(' ', 5);
            e.mv = rest.substr(5, sp == std::string::npos ? std::string::npos : sp - 5);
            if (sp != std::string::npos) e.gameKey = rest.substr(sp + 1);
        } else if (rest.rfind("settled", 0) == 0) {
            e.what = Phase::Settled;
        } else if (rest.rfind("end", 0) != 0) {
            return;
        }
        out.push_back(std::move(e));
    });
    g_chatMark = top;
    std::sort(out.begin(), out.end(), [](const ChatEvent& a, const ChatEvent& b) { return a.key < b.key; });
    return out;
}

// ---- The verdict of a watching peer -------------------------------------------------------------------
//
// What the client's door did is read from the poses this peer takes for it (the held-item stream carries the
// client's own door transform every tick). The poses before the client's `settled` line are its drag; the first
// pose after it opens the second hold, which carries the client's resting rotation; the `end` line is the verdict.

struct Verdict {
    bool started = false, said = false, settledSeen = false, secondSeen = false, hostRead = false, heard = false;
    Clock::time_point lastEvent{};
    ue_wrap::FRotator first{};          // the client's door at the first pose
    ue_wrap::FRotator hostSettled{};    // this peer's door as it stood when the second hold's first pose came
    ue_wrap::FRotator clientSettled{};  // the client's door at the second hold's newest pose
    float clientDev = 0.f;              // the largest angle of the client's door from `first`
    int lid = -1;                       // this peer's copy of the door, once a pose resolved to it
    uint32_t resolved = 0, unresolved = 0;
};
Verdict g_v;

void Judge(bool timedOut) {
    g_v.said = true;
    PrintRx();
    if (g_v.clientDev < kSwingDeg) {
        UE_LOGW("[SWING-DRILL] %s INVALID the client's door turned only %.1f deg on the stream (need %.0f)", Role(),
                g_v.clientDev, kSwingDeg);
    } else {
        const Lid* l = g_v.lid >= 0 ? &g_lids[static_cast<size_t>(g_v.lid)] : nullptr;
        const float hostDev = l ? l->maxDev : 0.f;
        const bool haveSettle = g_v.secondSeen && g_v.hostRead;
        const float settle = haveSettle ? AngleDeg(g_v.hostSettled, g_v.clientSettled) : 180.f;
        const bool pass = l && haveSettle && hostDev >= kFollowDeg && settle <= kSettleDeg;
        UE_LOGI("[SWING-DRILL] %s %s: follow -- this peer's door turned %.1f deg of the client's %.1f (need >= %.0f) | "
                "settle -- %.1f deg from the client's resting rotation%s (need <= %.0f) | poses resolved %u, "
                "unresolved %u", Role(), pass ? "PASS" : "FAIL", hostDev, g_v.clientDev, kFollowDeg, settle,
                timedOut ? " (the client's end line never came)"
                         : haveSettle ? "" : " (no second-hold pose reached this peer's door)",
                kSettleDeg, g_v.resolved, g_v.unresolved);
    }
    UE_LOGI("[SWING-DRILL] %s DONE", Role());
}

// A PropPose from the client arrived: `lid` is this peer's copy of the door when it resolved to one, else -1.
void PoseArrived(const ue_wrap::FRotator& rot, int lid) {
    if (Acts() || g_v.said) return;
    if (lid >= 0) { g_v.lid = lid; ++g_v.resolved; } else { ++g_v.unresolved; }
    g_v.lastEvent = Clock::now();
    g_v.heard = true;
    if (!g_v.started) {
        g_v.started = true;
        g_v.first = rot;
    } else if (!g_v.settledSeen) {
        g_v.clientDev = (std::max)(g_v.clientDev, AngleDeg(rot, g_v.first));
    }
    if (!g_v.settledSeen) return;
    if (!g_v.secondSeen) {   // the second hold's first pose: this peer's door has not taken it yet
        g_v.secondSeen = true;
        g_v.hostRead = lid >= 0;
        if (lid >= 0) g_v.hostSettled = g_lids[static_cast<size_t>(lid)].scanned;
    }
    g_v.clientSettled = rot;
}

// The client's key and this peer's, for the same lid: the proof that a lid whose game Key differs between
// processes is the one dragged.
void CheckKey(const ChatEvent& e) {
    for (const Lid& l : g_lids) {
        if (l.laneKey != std::wstring(e.mv.begin(), e.mv.end())) continue;
        const std::wstring theirs(e.gameKey.begin(), e.gameKey.end());
        UE_LOGI("[SWING-DRILL] %s key check: lane key '%s' -- this peer's game Key '%ls', the client's '%ls': %s", Role(),
                e.mv.c_str(), l.propKey.c_str(), theirs.c_str(), l.propKey == theirs ? "MATCH" : "DIFFER");
        return;
    }
    UE_LOGW("[SWING-DRILL] %s key check: no lid here carries the lane key '%s'", Role(), e.mv.c_str());
}

void OnChat(const ChatEvent& e) {
    if (Acts()) {   // the client sees its own lines come back
        if (e.what == Phase::Settled) g_echoSettled = true;
        else if (e.what == Phase::End) g_echoEnd = true;
        return;
    }
    if (g_v.said) return;
    g_v.lastEvent = Clock::now();
    g_v.heard = true;
    if (e.what == Phase::Grab) CheckKey(e);
    else if (e.what == Phase::Settled) g_v.settledSeen = true;
    else Judge(false);
}

// Every lid every scan; a lid that moved is sampled every 250 ms until a tail after its last move.
void TickWatch(Clock::time_point now) {
    if (now >= g_nextScan) {
        g_nextScan = now + kScanEvery;
        for (const ChatEvent& e : NewChat()) OnChat(e);
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
    // The client's end line never came: the verdict is read without it.
    if (!Acts() && g_v.heard && !g_v.said && now - g_v.lastEvent > kJudgeBound) Judge(true);
}

// ---- The acting client ---------------------------------------------------------------------------------

Step g_step = Step::Pick;
int g_dragTicks = 0;
ue_wrap::CachedObjRef g_target;
size_t g_targetLid = 0;
ue_wrap::FVector g_targetAt{};
std::shared_ptr<coop::director::BackgroundWalk> g_walk;
std::unique_ptr<coop::director::AimedGrab> g_grab;
Clock::time_point g_nextReport{}, g_nextRead{};
ue_wrap::FRotator g_lastRead{};   // the door at the last settle read
bool g_haveRead = false;
bool g_releaseEdge = false, g_wasTargetHeld = false;   // the held-item stream has let go of the door
uint32_t g_emitsAtRegrab = 0;
ue_wrap::FRotator g_grabRot{};   // the client's door at the grab: what its swing is measured from
float g_peakDev = 0.f;           // the largest angle the client's door has turned from g_grabRot

// The held-prop stream's side, filled by local_streams through the Note* seams.
uint32_t g_heldTicks = 0, g_ticks = 0, g_poseEmits = 0, g_edgeEid = 0, g_poseEid = 0;
bool g_edgeSeen = false, g_edgeBroadcast = false;
std::wstring g_poseKey, g_otherHeldCls;
void* g_lastOtherHeld = nullptr;   // compared as an address only

const char* StepName() {
    static const char* const n[] = {"Pick", "WalkTo", "Grab", "Drag", "Release", "Tail", "Announce", "Regrab",
                                    "Settle", "Last", "Echo", "Done", "Invalid"};
    return n[static_cast<int>(g_step)];
}

void Invalid(const char* why) {
    UE_LOGW("[SWING-DRILL] client INVALID %s", why);
    g_step = Step::Invalid;
}

void Go(Step s) {
    g_step = s;
}

void LogTarget(const char* what) {
    void* d = g_target.Get();
    if (!d) { UE_LOGI("[SWING-DRILL] client %s -- the door is gone", what); return; }
    Say(what, g_lids[g_targetLid], Read(d));
}

// ---- The drag --------------------------------------------------------------------------------------------
//
// The client feeds the player's input_mouseXY dispatcher while it holds the door. A non-lockable lid reads
// `opened 1` and its spring holds it at the open limit, so a feed that pushes into that limit moves nothing: the
// sign is searched. The first sign ramps slowly from kFeedStart to kFeedCap; each later one ramps faster. The
// door's physics stays on throughout, since the grab's physics handle needs a simulating body. The held-item
// stream then carries the door's motion as it carries any held actor's.

struct FeedSign { float x, y; };
constexpr FeedSign kSigns[] = {{-1.f, -1.f}, {1.f, -1.f}, {-1.f, 1.f}, {1.f, 1.f}};
constexpr int kSignCount = static_cast<int>(sizeof(kSigns) / sizeof(kSigns[0]));

int   g_sign = 0, g_signTicks = 0;   // the sign being fed, and how long
bool  g_swung = false;
int   g_swungAtTick = 0;
float g_feedX = 0.f, g_feedY = 0.f;  // the feed now

// Ticks per doubling of the feed on the sign now, and ticks the sign runs: the ramp to the cap, then one more
// doubling's worth at the cap.
int FeedEvery() { return g_sign == 0 ? kFeedEveryFirst : kFeedEveryLater; }
int FeedTicks() { return (kFeedDoublings + 1) * FeedEvery(); }

// The lid, among `ids` (nearest first, at most kPickRoutes of them), whose NavMesh route is shortest; false when
// none is reached.
bool PickAmong(void* player, const ue_wrap::FVector& me, std::vector<std::pair<float, size_t>>& ids, float& best) {
    std::sort(ids.begin(), ids.end());
    if (static_cast<int>(ids.size()) > kPickRoutes) ids.resize(kPickRoutes);
    for (const auto& o : ids) {
        void* d = g_lids[o.second].ref.Get();
        ue_wrap::FVector centre{}, extent{};
        if (!d || (!E::GetActorBounds(d, /*onlyColliding=*/true, centre, extent) && !E::TryGetActorLocation(d, centre)))
            continue;
        float len = 0.f;
        if (!coop::director::RouteInLegs(player, me, centre, kRouteEndReachCm, &len)) continue;
        if (len < best) { best = len; g_target.Set(d); g_targetLid = o.second; g_targetAt = centre; }
    }
    return g_target.Raw() != nullptr;
}

// The lid to drag. A lid whose game Key differs between the two processes (a child of a keyed prop, `k:` in its
// portable identity) goes first: it is the class the portable key exists for, so a green run covers it. Failing
// one a route reaches, the nearest others.
bool PickTarget(void* player, const char*& why) {
    ue_wrap::FVector me{};
    if (!E::TryGetActorLocation(player, me)) { why = "the player's location could not be read"; return false; }
    why = "no non-lockable lid among the nearest few that a route reaches";
    std::vector<std::pair<float, size_t>> perProcess, others;
    for (size_t i = 0; i < g_lids.size(); ++i) {
        void* d = g_lids[i].ref.Get();
        ue_wrap::FVector at{};
        if (d && E::TryGetActorLocation(d, at)) (g_lids[i].perProcessKey ? perProcess : others).emplace_back(Dist(me, at), i);
    }
    float best = 1e30f;
    if (!PickAmong(player, me, perProcess, best) && !PickAmong(player, me, others, best)) return false;
    UE_LOGI("[SWING-DRILL] client target key='%ls' identity '%ls' game Key '%ls'%s at (%.1f, %.1f, %.1f), a %.0f cm route "
            "(%.0f cm straight)", g_lids[g_targetLid].laneKey.c_str(), g_lids[g_targetLid].ident.c_str(),
            g_lids[g_targetLid].propKey.c_str(),
            g_lids[g_targetLid].perProcessKey ? " (a key that differs between processes)" : "", g_targetAt.X,
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
    PumpOutbox();
    if (static_cast<int>(g_step) >= static_cast<int>(Step::Grab)) { ++g_ticks; ReportHeld(now); }
    switch (g_step) {
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
        g_sign = g_signTicks = g_swungAtTick = 0;
        g_swung = false;
        LogTarget("grabbed");
        SayLine("grab " + std::string(l.laneKey.begin(), l.laneKey.end()) + " " +
                std::string(l.propKey.begin(), l.propKey.end()));
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
        // The feed on this sign now: the ramp, or the value the door turned at once it has.
        if (!g_swung) {
            const float mag = (std::min)(kFeedStart * std::exp2(static_cast<float>(g_signTicks / FeedEvery())),
                                         kFeedStart * std::exp2(static_cast<float>(kFeedDoublings)));
            g_feedX = kSigns[g_sign].x * mag;
            g_feedY = kSigns[g_sign].y * mag;
        }
        int delivered = 0;
        if (!E::BroadcastMainPlayerMouseXY(player, g_feedX, g_feedY, &delivered)) {
            Invalid("the player's input_mouseXY dispatcher did not resolve");
            return;
        }
        if (g_signTicks == 0 || g_signTicks % 30 == 0)
            UE_LOGI("[SWING-DRILL] client feed (%+.0f, %+.0f) tick %d: (%.1f, %.1f) ran %d bound entr%s, the door has "
                    "turned %.1f deg", kSigns[g_sign].x, kSigns[g_sign].y, g_signTicks, g_feedX, g_feedY, delivered,
                    delivered == 1 ? "y" : "ies", g_peakDev);
        if (void* mesh = PR::GetStaticMesh(d))
            g_peakDev = (std::max)(g_peakDev, AngleDeg(E::GetComponentWorldRotation(mesh), g_grabRot));
        g_lids[g_targetLid].lastMoveAt = now;
        ++g_dragTicks;
        ++g_signTicks;
        if (!g_swung && g_peakDev >= kSwingDeg) {
            g_swung = true;
            g_swungAtTick = g_dragTicks;
            UE_LOGI("[SWING-DRILL] client SWUNG: sign (%+.0f, %+.0f) moved the door %.1f deg at (%.1f, %.1f), tick %d "
                    "of that sign", kSigns[g_sign].x, kSigns[g_sign].y, g_peakDev, g_feedX, g_feedY, g_signTicks);
        }
        if (g_swung) {
            if (g_dragTicks >= g_swungAtTick + kHoldAfterSwingTicks) Go(Step::Release);
        } else if (g_signTicks >= FeedTicks()) {
            UE_LOGI("[SWING-DRILL] client sign (%+.0f, %+.0f) did not move the door (peak %.1f deg)", kSigns[g_sign].x,
                    kSigns[g_sign].y, g_peakDev);
            if (g_sign + 1 < kSignCount) { ++g_sign; g_signTicks = 0; }
            else Go(Step::Release);
        }
        return;
    }
    case Step::Release: {
        coop::director::CallOnPlayer(player, L"dropGrabObject");
        g_releaseEdge = false;   // the stream's release is the first tick after this that the door is not held
        LogTarget("released");
        g_lids[g_targetLid].lastMoveAt = now;
        if (!g_swung) {
            char why[160];
            snprintf(why, sizeof(why), "no feed sign moved the client's own door: it turned only %.1f deg (need %.0f)",
                     g_peakDev, kSwingDeg);
            Invalid(why);
            return;
        }
        UE_LOGI("[SWING-DRILL] client released: the door turned %.1f deg at its peak", g_peakDev);
        g_haveRead = false;
        Go(Step::Tail);
        return;
    }
    case Step::Tail: {   // settled: two reads a sample apart, after the stream's release, agree
        if (!g_releaseEdge || now < g_nextRead) return;
        g_nextRead = now + kSampleEvery;
        void* mesh = PR::GetStaticMesh(g_target.Get());
        if (!mesh) { Invalid("the door has no mesh to read"); return; }
        const ue_wrap::FRotator w = E::GetComponentWorldRotation(mesh);
        const bool same = g_haveRead && AngleDeg(w, g_lastRead) <= kRestDeg;
        g_lastRead = w;
        g_haveRead = true;
        if (!same) return;
        LogTarget("settled");
        SayLine("settled");
        Go(Step::Announce);
        return;
    }
    case Step::Announce:   // the line has come back from the lobby, so the watchers have it
        if (!g_echoSettled) return;
        g_grab = std::make_unique<coop::director::AimedGrab>(player, g_target.Get());
        Go(Step::Regrab);
        return;
    case Step::Regrab: {   // the second hold, with no feed: the pose it rests at crosses on the stream
        if (!g_target.Get()) { Invalid("the door died before the second hold"); return; }
        const coop::director::GrabState gs = g_grab->Tick();
        if (gs == coop::director::GrabState::Working) return;
        if (gs == coop::director::GrabState::Failed) { Invalid(g_grab->Why()); return; }
        LogTarget("regrabbed");
        g_emitsAtRegrab = g_poseEmits;
        Go(Step::Settle);
        return;
    }
    case Step::Settle:   // held until the stream has carried a few of its poses
        if (g_poseEmits - g_emitsAtRegrab < kSecondHoldPoses) return;
        LogTarget("second hold");
        coop::director::CallOnPlayer(player, L"dropGrabObject");
        g_releaseEdge = false;
        Go(Step::Last);
        return;
    case Step::Last:   // the stream has let go of the door
        if (!g_releaseEdge) return;
        SayLine("end");
        Go(Step::Echo);
        return;
    case Step::Echo:   // the end line has come back from the lobby: the watchers judge on it
        if (!g_echoEnd) return;
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
        MarkChat();
    }
    const Clock::time_point now = Clock::now();
    TickWatch(now);
    if (Acts()) TickActor(now);
}

void OnDisconnect() {
    g_lids.clear();
    g_armed = false;
    g_v = Verdict{};
    g_step = Step::Pick;
    g_dragTicks = 0;
    g_peakDev = 0.f;
    g_sign = g_signTicks = g_swungAtTick = 0;
    g_swung = false;
    g_feedX = g_feedY = 0.f;
    g_outbox.clear();
    g_outNext = 0;
    g_nextSayMs = 0;
    g_chatMark = 0;
    g_echoSettled = g_echoEnd = false;
    g_haveRead = g_releaseEdge = g_wasTargetHeld = false;
    g_emitsAtRegrab = 0;
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
    if (!IsEnabled()) return;
    const bool targetNow = heldActor && heldActor == g_target.Raw();
    if (g_wasTargetHeld && !targetNow) g_releaseEdge = true;   // local_streams sends the release on this tick
    g_wasTargetHeld = targetNow;
    if (!heldActor) return;
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
