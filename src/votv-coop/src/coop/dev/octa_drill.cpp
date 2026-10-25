// coop/dev/octa_drill.cpp -- see coop/dev/octa_drill.h.

#include "coop/dev/octa_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/creature_hit.h"
#include "coop/creatures/geom_octa_relay.h"
#include "coop/creatures/geom_octa_sync.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/dev/drill_hold.h"
#include "coop/dev/octa_probe.h"
#include "coop/dev/spawn_npc.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "coop/world/explosion_sync.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"  // NpcClass_GeomOcta
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/world_singleton.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::octa_drill {
namespace {

namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace D  = coop::director;
namespace DH = coop::dev::drill_hold;
namespace GS = coop::geom_octa_sync;
using ue_wrap::FVector;

constexpr const char*    kTag         = "[OCTA-DRILL]";
constexpr const wchar_t* kOctaClass   = ue_wrap::profile::name::NpcClass_GeomOcta;
constexpr const wchar_t* kGuardMember = L"startInvinc";

constexpr uint64_t kEveryMs          = 250;
constexpr float    kSpawnRingCm      = 1500.f;  // births: the Octa stands this far from the host
constexpr float    kPlaceFromCm      = 2200.f;  // measure: the walker takes its place this far from the other player
constexpr float    kSpawnNearCm      = 600.f;   // measure: the Octa spawns about this far from the walker, inside minSphere
constexpr float    kSpawnAwayCm      = 1300.f;  // measure: and at least this far from the other player, outside minSphere
constexpr uint64_t kStillMs          = 2000;    // measure: a puppet that has not moved for this long is in place
constexpr float    kStillCm          = 30.f;    // ... where "not moved" is less than this between two polls
// The walk-out ends this far from the Octa, outside minSphere: the first ring with a route, else the next.
constexpr float    kOutRingsCm[]     = {1800.f, 2400.f, 3000.f};
constexpr size_t   kOutRingCount     = sizeof(kOutRingsCm) / sizeof(kOutRingsCm[0]);
constexpr float    kSphereCm         = 1000.f;  // minSphere's radius
constexpr int      kStandpoints      = 8;
constexpr float    kStandReachCm     = 60.f;    // a standpoint is reached within this
constexpr int      kFirstWalkDeadlineS = 400;
constexpr int      kOutWalkDeadlineS   = 120;
constexpr uint32_t kBlastsWanted     = 3;       // the volleys the walker waits for, and the verdicts the host waits for
// One laser makes several explosions, which land within a tick of each other on loopback, and a re-fire follows about
// 1.8 s after. The client cannot see verdicts before the relay, so an explosion applied later than this after the
// previous one starts a new volley. It is above the 250 ms poll, so two explosions straddling a poll stay one volley.
constexpr uint64_t kVolleyGapMs      = 400;
constexpr uint64_t kAggroBoundMs     = 30000;   // HOST: no aggro this long after the spawn is a dead Octa
constexpr uint64_t kFireBoundMs      = 90000;   // CLIENT: this long with no new blast applied is a stalled wait
constexpr uint64_t kBirthsBoundMs    = 60000;   // births: the enrol, and either arm's mirror, within this

enum class Arm : uint8_t { Off, Births, Measure, Attack };
Arm ArmOf() {
    static const Arm arm = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::octa_drill);
        return v == "births" ? Arm::Births : v == "measure" ? Arm::Measure : v == "attack" ? Arm::Attack : Arm::Off;
    }();
    return arm;
}
// The arms that place a walker and let the Octa fire: measure (no relay judged) and attack (the relay judged).
bool Fires() { return ArmOf() == Arm::Measure || ArmOf() == Arm::Attack; }
const char* FiringArmName() { return ArmOf() == Arm::Attack ? "attack" : "measure"; }

// Where the measure and attack arms spawn the Octa (octa_spawn_route): the NPC spawn of the dev tool, or the gamemode's
// own spawn verb with the spawn menu's Octa row, whose Octa the host's EX catch enrols at the drain.
bool SpawnByGamemode() {
    static const bool gm = coop::config::ResolveEnum(::coop::config_registry::rows::octa_spawn_route) == "gamemode";
    return gm;
}

// Who walks into the Octa's sphere (the control row octa_measure_target): the client, or the host's own player.
bool HostWalks() {
    static const bool host =
        coop::config::ResolveEnum(::coop::config_registry::rows::octa_measure_target) == "host";
    return host;
}

enum class HStep : uint8_t { Wait, Place, Watch, Done };
enum class WStep : uint8_t { Stand, Out, Finished };   // the host's own legs after the spawn (octa_measure_target=host)
enum class CStep : uint8_t { Ready, Place, Find, Stand, Out, Done };
HStep g_hstep = HStep::Wait;
CStep g_cstep = CStep::Ready;

uint64_t g_nextMs = 0;
uint64_t g_stepMs = 0;            // the step began; a FAIL bound counts from here
int      g_invinc = -1;           // the value startInvinc is put back to (-1: not held)
ue_wrap::CachedObjRef g_octa;     // HOST: the Octa it spawned or the gamemode's spawn made
std::shared_ptr<D::BackgroundWalk> g_walk;

// HOST (measure)
GS::Counters g_base{};            // the lane's counters when the Octa stood
uint64_t g_spawnMs = 0;           // the Octa was spawned; the aggro bound counts from here
bool     g_aggroSaid = false;
bool     g_eidSaid = false;       // the Octa's NPC element id has been said (the gamemode route enrols it a tick late)
uint8_t  g_walkerSlot = 0;        // the slot the Octa must aggro: the host's own (0), or the client's puppet's
FVector  g_puppetLast{};          // the client puppet's place at the last poll, and since when it has not moved
uint64_t g_puppetStillAt = 0;
WStep    g_wstep = WStep::Stand;  // octa_measure_target=host: the host's legs after the spawn
uint32_t g_volleysSeen = 0;       // the host's own verdicts (one per fire) since the Octa was spawned, as last polled
// CLIENT (measure)
uint32_t g_appliedBase = 0;       // the explosion lane's applied count when the Octa mirror appeared
uint32_t g_appliedSeen = 0;
uint32_t g_volleys = 0;           // the volleys applied since then
coop::geom_octa_relay::Counters g_relay0{};   // attack: the relay's counters when the mirror appeared
uint32_t g_verdictsSeen = 0;      // attack: the verdicts it has applied since
uint32_t g_laserSeen = 0;         // attack: this client's own lasers counted so far
uint64_t g_laserPendingMs = 0;    // attack: the stamp of a laser still waiting for the blast that follows it
size_t   g_outRing = 0;           // the walk-out ring in use (kOutRingsCm)
uint64_t g_lastAppliedMs = 0;     // when the last explosion was seen applied
bool     g_calledEvent = false;   // CLIENT (births): the night roll's event has been called

bool Due() {
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextMs) return false;
    g_nextMs = now + kEveryMs;
    return true;
}
bool Expired(uint64_t bound) { return coop::net::NowMs() - g_stepMs > bound; }
unsigned long long Now() { return static_cast<unsigned long long>(coop::net::NowMs()); }

// ---- the victim guard ----

// startInvinc held true on this peer's own player, asserted again each tick: its one writer is a Delay(5) after
// begin-play that clears it, and a blast must find it set. False when the player or the member does not resolve.
bool Guard() {
    return DH::Hold(coop::players::Registry::Get().Local(), kGuardMember, g_invinc, 0, kTag);
}
void Unguard() { DH::Put(coop::players::Registry::Get().Local(), kGuardMember, g_invinc, kTag); }

// The arm ends on a FAIL line; the guard goes back with it.
void Fail(const char* what) {
    UE_LOGW("[OCTA-DRILL] FAIL: %s", what);
    g_hstep = HStep::Done;
    g_cstep = CStep::Done;
    Unguard();
}

void Go(CStep s) { g_cstep = s; g_stepMs = coop::net::NowMs(); }

bool DamageProbeOn() {
    static const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::damage_probe);
    return on;
}

// ---- finding things ----

struct Found { void* obj = nullptr; };

// The first live instance of the class, the default object left out; null when none.
void* FirstInstance(const wchar_t* className) {
    void* cls = ue_wrap::object_index::ClassByName(className);
    if (!cls) return nullptr;
    Found found;
    ue_wrap::object_index::ForEachInstance(cls,
        [](void* ctx, void* obj, int32_t) {
            auto* out = static_cast<Found*>(ctx);
            if (!out->obj && !R::NameStartsWith(R::NameOf(obj), L"Default__")) out->obj = obj;
        },
        &found);
    return found.obj;
}

// The Octa element's id of `actor` (the host's own or a client's mirror), 0 when the registry names none.
uint32_t NpcEidOf(void* actor) {
    if (!actor || !coop::creature_hit::IsCreature(actor)) return 0;
    return static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(actor));
}

float FlatDist(const FVector& a, const FVector& b) { return std::hypot(a.X - b.X, a.Y - b.Y); }

// The first puppet's body that is live, with its slot; null when there is none.
void* LivePuppetBody(uint8_t& slot) {
    for (uint8_t s = 0; s < coop::players::kMaxPeers; ++s) {
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(s);
        void* body = rp ? rp->GetActor() : nullptr;
        if (body && R::IsLive(body)) {
            slot = s;
            return body;
        }
    }
    return nullptr;
}

// ---- HOST ----

// The host's player and the first standpoint on a ring about it that a route reaches; false once the arm has ended.
bool RingPoint(void* me, FVector& out) {
    FVector at{};
    if (!E::TryGetActorLocation(me, at)) return false;
    const std::vector<FVector> ring = D::ReachableStandpoints(me, at, kSpawnRingCm, kStandpoints, kStandReachCm);
    if (ring.empty()) {
        Fail("no NavMesh route from the host reaches a standpoint 1500 cm away, so the Octa has no ground to stand on");
        return false;
    }
    out = ring[0];
    return true;
}

// The live Octas of the world, to tell the one a spawn made from the rest.
struct OctaSet {
    void*  octas[16] = {};
    size_t count = 0;
};

void CollectOctas(OctaSet& out) {
    out.count = 0;
    void* cls = ue_wrap::object_index::ClassByName(kOctaClass);
    if (!cls) return;
    ue_wrap::object_index::ForEachInstance(cls,
        [](void* ctx, void* obj, int32_t) {
            auto* set = static_cast<OctaSet*>(ctx);
            if (set->count < 16 && !R::NameStartsWith(R::NameOf(obj), L"Default__")) set->octas[set->count++] = obj;
        },
        &out);
}

// The Octa a spawn made: the live one that was not among `before`, else the one nearest `at` within 200 cm; null when
// there is neither.
void* NewOctaSince(const OctaSet& before, const FVector& at) {
    OctaSet now;
    CollectOctas(now);
    void* nearest = nullptr;
    float nearestD = 200.f;
    for (size_t i = 0; i < now.count; ++i) {
        bool known = false;
        for (size_t j = 0; j < before.count; ++j)
            if (before.octas[j] == now.octas[i]) known = true;
        if (!known) return now.octas[i];
        FVector p{};
        if (E::TryGetActorLocation(now.octas[i], p) && FlatDist(p, at) <= nearestD) {
            nearest = now.octas[i];
            nearestD = FlatDist(p, at);
        }
    }
    return nearest;
}

// births: the gamemode's own spawn verb with the spawn menu's Octa row, the call the night roll shares.
bool CallGamemodeSpawn(const FVector& at) {
    void* gm = ue_wrap::world_singleton::Gamemode();
    void* fn = gm ? R::FindFunction(R::ClassOf(gm), L"spawnPropThroughGamemode") : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid()) return false;
    const R::FName row = ue_wrap::fname_utils::StringToFName(L"cube_0");
    ue_wrap::FTransform xform{};
    xform.TX = at.X;
    xform.TY = at.Y;
    xform.TZ = at.Z;
    return f.SetRaw(L"prop", &row, sizeof(row)) && f.SetRaw(L"InputPin", &xform, sizeof(xform)) &&
           f.Set<int32_t>(L"amount", 1) && ue_wrap::Call(gm, f);
}

void HostWait(coop::net::Session* s, void* me) {
    if (!s->AnyWorldReadyPeer()) return;
    float worldNow = 0.f;
    if (!E::TryGetWorldTimeSeconds(worldNow)) return;
    if (Fires() && !DamageProbeOn()) {
        Fail("damage_probe=1 is required: each blast must still log its [DMG] line");
        return;
    }
    if (ArmOf() == Arm::Attack && HostWalks()) {
        Fail("the attack arm judges the client's relay: octa_measure_target=host belongs to measure");
        return;
    }
    if (!Guard()) {
        Fail("startInvinc did not resolve on the host's player, so a blast would not leave it alive");
        return;
    }
    g_stepMs = coop::net::NowMs();
    if (Fires()) {
        g_hstep = HStep::Place;   // the walker takes its place first; the Octa is spawned beside it
        return;
    }
    FVector at{};
    if (!RingPoint(me, at)) return;
    if (!CallGamemodeSpawn(at)) {
        Fail("the gamemode's spawnPropThroughGamemode did not run");
        return;
    }
    UE_LOGI("[OCTA-DRILL] host: births called spawnPropThroughGamemode(cube_0) at=%.0f,%.0f,%.0f", at.X, at.Y, at.Z);
    g_hstep = HStep::Watch;
}

// measure: spawns the Octa about 600 cm from the walker, on the side away from the other player (a reachable point of a
// ring about the walker, the one farthest from the other, at least 1300 cm from it). The Octa's sphere registers inside
// its own finish-spawn and reports the walker's capsule as an overlap there, which is how it aggros, with the other
// player out of the sphere's reach.
void SpawnOctaBeside(void* me, const FVector& walker, const FVector& other) {
    const std::vector<FVector> ring = D::ReachableStandpoints(me, walker, kSpawnNearCm, kStandpoints, kStandReachCm);
    const FVector* best = nullptr;
    float bestD = 0.f;
    for (const FVector& p : ring) {
        const float d = FlatDist(p, other);
        if (d >= kSpawnAwayCm && (!best || d > bestD)) {
            best = &p;
            bestD = d;
        }
    }
    if (!best) {
        Fail("no reachable point 600 cm from the walker lies 1300 cm or more from the other player");
        return;
    }
    // The baseline is read before the spawn: the sphere reports the walker standing inside it as it registers, inside
    // the spawn's finish, so the first aggro lands before SpawnAt returns.
    g_base = GS::GetCounters();
    // The dev tool's NPC spawn is enrolled at its interceptor, which also sends the spawn; the gamemode's spawn verb is
    // caught at the EX catch and enrolled, and sent, at the drain a tick later: the Octa's first aggro, inside the
    // spawn's finish, then precedes both, and the lane keeps it until the spawn has been sent.
    void* octa = nullptr;
    if (SpawnByGamemode()) {
        OctaSet before;
        CollectOctas(before);
        if (CallGamemodeSpawn(*best)) octa = NewOctaSince(before, *best);
    } else {
        octa = coop::dev::spawn_npc::SpawnAt(kOctaClass, *best);
    }
    if (!octa) {
        Fail("the Octa did not spawn");
        return;
    }
    g_octa.Set(octa);
    GS::OpenVerdictWindow(kBlastsWanted, octa);
    g_volleysSeen = 0;
    g_wstep = WStep::Stand;
    g_spawnMs = g_stepMs = coop::net::NowMs();
    g_hstep = HStep::Watch;
    const uint32_t eid = NpcEidOf(octa);
    g_eidSaid = eid != 0;   // the gamemode's Octa is enrolled at the drain, a tick on: its id is said then
    char eidText[24];
    if (eid) std::snprintf(eidText, sizeof(eidText), "%u", eid);
    else std::snprintf(eidText, sizeof(eidText), "pending");
    UE_LOGI("[OCTA-DRILL] host READY %s: Octa (%s route) eid=%s at=%.0f,%.0f,%.0f, %.0f cm from the walker (slot %u), "
            "%.0f from the other player", FiringArmName(), SpawnByGamemode() ? "gamemode" : "sat", eidText, best->X,
            best->Y, best->Z, FlatDist(*best, walker), static_cast<unsigned>(g_walkerSlot), bestD);
}

// measure: the walker takes its place. The client's: its director walks it (the client's own half) and the host spawns
// the Octa once the puppet has stood still for two seconds, 1300 cm or more from the host. The host's: its own director
// walks it 2200 cm from the client's puppet, and the Octa is spawned beside it.
void HostPlace(void* me) {
    uint8_t slot = 0;
    void* puppet = LivePuppetBody(slot);
    FVector hp{}, pp{};
    if (!E::TryGetActorLocation(me, hp)) return;
    if (!puppet || !E::TryGetActorLocation(puppet, pp)) {
        if (Expired(kBirthsBoundMs)) Fail("the client's puppet never resolved on the host");
        return;
    }
    if (HostWalks()) {
        if (!g_walk) {
            const std::vector<FVector> ring = D::ReachableStandpoints(me, pp, kPlaceFromCm, kStandpoints, kStandReachCm);
            if (ring.empty()) {
                Fail("no NavMesh route from the host reaches a standpoint 2200 cm from the client's puppet");
                return;
            }
            g_walk = D::StartBackgroundWalk(ring[0], kStandReachCm, kFirstWalkDeadlineS);
            UE_LOGI("[OCTA-DRILL] host: walking to its place, 2200 cm from the client's puppet");
            return;
        }
        const int state = g_walk->state.load();
        if (state == 0) return;
        if (state == 2) {
            Fail("no walk reached the host's place: its route ended with none");
            return;
        }
        g_walk.reset();
        g_walkerSlot = 0;
        UE_LOGI("[OCTA-DRILL] host in place t=%llu", Now());
        if (!E::TryGetActorLocation(me, hp)) return;
        SpawnOctaBeside(me, hp, pp);
        return;
    }
    // The client is in place when its puppet is far from the host and has not moved for two seconds.
    const uint64_t now = coop::net::NowMs();
    if (!g_puppetStillAt || FlatDist(pp, g_puppetLast) >= kStillCm) {
        g_puppetLast = pp;
        g_puppetStillAt = now;
        return;
    }
    if (now - g_puppetStillAt < kStillMs || FlatDist(pp, hp) < kSpawnAwayCm) return;
    g_walkerSlot = slot;
    SpawnOctaBeside(me, pp, hp);
}

void HostWatchBirths() {
    void* octa = FirstInstance(kOctaClass);
    const uint32_t eid = NpcEidOf(octa);
    if (eid) {
        UE_LOGI("[OCTA-DRILL] births PASS role=host eid=%u t=%llu", eid, Now());
        g_hstep = HStep::Done;
        return;
    }
    if (Expired(kBirthsBoundMs)) Fail("births host: the Octa the gamemode spawned was never enrolled as an NPC element");
}

// The walker's way out of the sphere: a standpoint on a ring about the Octa's live place, the next ring when the last
// has none or the walk to it ended with no route (an earlier host walk-out failed once on a place with no route 1800 cm
// out). False, said, once the last ring has failed.
bool BeginWalkOut(void* me, void* octa, const char* who) {
    FVector at{};
    if (!octa || !E::TryGetActorLocation(octa, at)) {
        Fail("the Octa is gone before the walk-out");
        return false;
    }
    for (; g_outRing < kOutRingCount; ++g_outRing) {
        const std::vector<FVector> ring = D::ReachableStandpoints(me, at, kOutRingsCm[g_outRing], kStandpoints, kStandReachCm);
        if (ring.empty()) continue;
        g_walk = D::StartBackgroundWalk(ring[0], kStandReachCm, kOutWalkDeadlineS);
        UE_LOGI("[OCTA-DRILL] %s: walking out to a standpoint %.0f cm from the Octa", who, kOutRingsCm[g_outRing]);
        return true;
    }
    Fail("no NavMesh route from the Octa reaches a standpoint 1800, 2400 or 3000 cm out: the walker cannot leave its sphere");
    return false;
}

// octa_measure_target=host: the host's own player, in place beside the Octa it spawned, stands until the Octa has fired
// three volleys and walks out, from the Octa's live place. A volley is one fire, which is one host verdict: a laser
// makes several explosions, so the host counts its verdicts, not its explosions. The bound is the client's.
void HostWalkTick(void* octa, void* me) {
    switch (g_wstep) {
    case WStep::Stand: {
        const uint32_t now = GS::GetCounters().hostVerdict - g_base.hostVerdict;
        if (now > g_volleysSeen) {
            g_volleysSeen = now;
            g_stepMs = coop::net::NowMs();
            UE_LOGI("[OCTA-DRILL] host: volley %u of %u fired", now, kBlastsWanted);
        }
        if (g_volleysSeen >= kBlastsWanted) {
            if (BeginWalkOut(me, octa, "host")) g_wstep = WStep::Out;
            return;
        }
        if (Expired(kFireBoundMs)) Fail("no host fire (the host's Octa fired no volley for 90 s)");
        return;
    }
    case WStep::Out: {
        const int state = g_walk ? g_walk->state.load() : 2;
        if (state == 0) return;
        if (state == 2) {   // the next ring
            ++g_outRing;
            BeginWalkOut(me, octa, "host");
            return;
        }
        UE_LOGI("[OCTA-DRILL] host out t=%llu", Now());
        g_wstep = WStep::Finished;
        return;
    }
    default:
        return;
    }
}

void HostWatchMeasure() {
    void* octa = g_octa.Get();
    if (!octa) {
        Fail("the Octa is gone before the arm ended");
        return;
    }
    Guard();
    void* me = coop::players::Registry::Get().Local();
    const GS::Counters c = GS::GetCounters();
    FVector op{};
    FVector pp{};
    uint8_t puppetSlot = 0;
    void* puppet = LivePuppetBody(puppetSlot);
    // The body that stands in the sphere: the client's puppet, or the host's own player for the control.
    void* walker = HostWalks() ? me : puppet;
    const bool haveBoth = E::TryGetActorLocation(octa, op) && walker && E::TryGetActorLocation(walker, pp);
    const float dist = haveBoth ? FlatDist(op, pp) : -1.f;
    const uint64_t now = coop::net::NowMs();
    coop::dev::octa_probe::Tick(octa, me, puppet);
    if (HostWalks()) {
        HostWalkTick(octa, me);
        if (g_hstep == HStep::Done) return;
    }
    if (!g_eidSaid) {
        if (const uint32_t eid = NpcEidOf(octa)) {
            g_eidSaid = true;
            UE_LOGI("[OCTA-DRILL] host: the Octa is enrolled, eid=%u", eid);
        }
    }
    if (!g_aggroSaid && c.hostAggro > g_base.hostAggro) {
        g_aggroSaid = true;
        char want[16];
        std::snprintf(want, sizeof(want), "slot:%u", static_cast<unsigned>(g_walkerSlot));
        UE_LOGI("[OCTA-DRILL] host: the Octa aggro'd target=%s (the walker is %s) %llu ms after the spawn",
                GS::LastAggroTarget(), want, static_cast<unsigned long long>(now - g_spawnMs));
        if (std::strcmp(GS::LastAggroTarget(), want) != 0) {
            Fail("aggro'd the wrong player");
            return;
        }
    }
    if (!g_aggroSaid && now - g_spawnMs > kAggroBoundMs) {
        Fail("no host aggro (30 s from the spawn)");
        return;
    }
    // An idle after the third verdict, with the puppet out of the sphere. The lane keeps the idle count at the moment
    // it counted the third verdict, so an idle between that and this poll is not absorbed.
    if (c.windowReached && c.idle > c.idleAtWindow && haveBoth && dist > kSphereCm) {
        if (ArmOf() == Arm::Attack)
            UE_LOGI("[OCTA-DRILL] attack DONE aggros=%u verdicts=%u idles=%u lasers=%u t=%llu",
                    c.hostAggro - g_base.hostAggro, c.hostVerdict - g_base.hostVerdict, c.idle - g_base.idle,
                    c.laser - g_base.laser, Now());
        else
            UE_LOGI("[OCTA-DRILL] measure DONE aggros=%u verdicts=%u idles=%u lasers=%u t=%llu",
                    c.hostAggro - g_base.hostAggro, c.hostVerdict - g_base.hostVerdict, c.idle - g_base.idle,
                    c.laser - g_base.laser, Now());
        g_hstep = HStep::Done;
    }
}

void HostTick(coop::net::Session* s) {
    if (!s->running() || g_hstep == HStep::Done) return;
    void* me = coop::players::Registry::Get().Local();
    if (!me || !Due()) return;
    if (g_hstep == HStep::Wait) {
        HostWait(s, me);
        return;
    }
    if (g_hstep == HStep::Place) HostPlace(me);
    else if (ArmOf() == Arm::Births) HostWatchBirths();
    else HostWatchMeasure();
    if (g_hstep == HStep::Done) Unguard();
}

// ---- CLIENT ----

uint32_t AppliedSum() {
    const coop::explosion_sync::Counters c = coop::explosion_sync::GetCounters();
    return c.applied[0] + c.applied[1] + c.applied[2];
}

void ClientFindMirror() {
    void* octa = FirstInstance(kOctaClass);
    const uint32_t eid = NpcEidOf(octa);
    if (!eid) {
        if (Expired(kBirthsBoundMs)) Fail("the host's Octa never materialised as a mirror on this client");
        return;
    }
    if (ArmOf() == Arm::Births) {
        UE_LOGI("[OCTA-DRILL] births PASS role=client eid=%u t=%llu", eid, Now());
        Go(CStep::Done);
        Unguard();
        return;
    }
    // In place already: the host spawned the Octa beside it, and it stands until the host's blasts have come.
    g_octa.Set(octa);
    g_appliedBase = g_appliedSeen = AppliedSum();
    g_relay0 = coop::geom_octa_relay::GetCounters();
    g_laserSeen = GS::GetCounters().laser;
    UE_LOGI("[OCTA-DRILL] client: the Octa mirror eid=%u stands beside it t=%llu applied=%u", eid, Now(), g_appliedBase);
    Go(CStep::Stand);
}

// measure, the client's place: its director walks it 2200 cm from the host's puppet; the host spawns the Octa beside it
// once it has stood still.
void ClientPlace(void* me) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(coop::players::kPeerIdHost);
    void* hostBody = rp ? rp->GetActor() : nullptr;
    FVector hp{};
    if (!hostBody || !R::IsLive(hostBody) || !E::TryGetActorLocation(hostBody, hp)) {
        if (Expired(kBirthsBoundMs)) Fail("the host's puppet never resolved on this client");
        return;
    }
    if (!g_walk) {
        const std::vector<FVector> ring = D::ReachableStandpoints(me, hp, kPlaceFromCm, kStandpoints, kStandReachCm);
        if (ring.empty()) {
            Fail("no NavMesh route from the client reaches a standpoint 2200 cm from the host");
            return;
        }
        g_walk = D::StartBackgroundWalk(ring[0], kStandReachCm, kFirstWalkDeadlineS);
        UE_LOGI("[OCTA-DRILL] client: walking to its place, 2200 cm from the host");
        return;
    }
    const int state = g_walk->state.load();
    if (state == 0) return;
    if (state == 2) {
        Fail("no walk reached the client's place: its route ended with none");
        return;
    }
    g_walk.reset();
    UE_LOGI("[OCTA-DRILL] client in place t=%llu", Now());
    Go(CStep::Find);
}

// The §5.5 spread: each of this client's own lasers against the first blast the explosion lane applies after it, both
// stamped from the net clock. The client's chain runs one latency behind the host's, so a blast can come before it.
void NoteSpread() {
    const GS::Counters c = GS::GetCounters();
    if (c.laser > g_laserSeen) {
        g_laserSeen = c.laser;
        g_laserPendingMs = c.lastLaserMs;
    }
    const uint64_t applyMs = coop::explosion_sync::GetCounters().lastApplyMs;
    if (g_laserPendingMs && applyMs >= g_laserPendingMs) {
        UE_LOGI("[OCTA-DRILL] client: laser -> first blast applied %llu ms",
                static_cast<unsigned long long>(applyMs - g_laserPendingMs));
        g_laserPendingMs = 0;
    }
}

// The attack arm's green is a clean relay: since the mirror appeared, no Aggro skipped, no verdict timed out or dropped,
// no event dropped, and as many answers written into the overlap read as verdicts applied. The first thing that is not,
// said as the arm's failure; null when clean.
const char* RelayDirty() {
    const coop::geom_octa_relay::Counters r = coop::geom_octa_relay::GetCounters();
    const coop::geom_octa_relay::Counters& b = g_relay0;
    if (r.aggroSkipped > b.aggroSkipped) return "the relay skipped an Aggro";
    if (r.verdictTimeout > b.verdictTimeout) return "a verdict timed out";
    if (r.verdictDropped > b.verdictDropped) return "a verdict was dropped";
    if (r.eventDropped > b.eventDropped) return "an event was dropped";
    if (r.overwriteWrites - b.overwriteWrites < r.verdictApplied - b.verdictApplied)
        return "the overwrite did not write: fewer answers were written than verdicts applied";
    return nullptr;
}

// Stands until the Octa has fired three volleys. measure counts them as volleys of the explosions this client's
// explosion lane applies (one laser makes several, so an explosion more than kVolleyGapMs after the previous one starts
// a new volley); attack counts the verdicts the relay has applied, which it can now see. The bound counts from the last
// sign of progress, so a wait that is still going is never cut.
void ClientStand(void* me) {
    Guard();
    const uint32_t now = AppliedSum();
    if (now > g_appliedSeen) {
        const uint64_t at = coop::net::NowMs();
        g_appliedSeen = now;
        g_stepMs = at;
        if (g_volleys == 0 || at - g_lastAppliedMs > kVolleyGapMs) {
            ++g_volleys;
            if (ArmOf() == Arm::Measure)
                UE_LOGI("[OCTA-DRILL] client: volley %u of %u applied (%u explosions so far)", g_volleys, kBlastsWanted,
                        now - g_appliedBase);
        }
        g_lastAppliedMs = at;
    }
    if (ArmOf() == Arm::Attack) {
        NoteSpread();
        if (const char* dirty = RelayDirty()) {
            Fail(dirty);
            return;
        }
        const uint32_t applied = coop::geom_octa_relay::GetCounters().verdictApplied - g_relay0.verdictApplied;
        if (applied > g_verdictsSeen) {
            g_verdictsSeen = applied;
            g_stepMs = coop::net::NowMs();
            UE_LOGI("[OCTA-DRILL] client: verdict %u of %u applied", applied, kBlastsWanted);
        }
    }
    const uint32_t reached = ArmOf() == Arm::Attack ? g_verdictsSeen : g_volleys;
    if (reached >= kBlastsWanted) {
        if (BeginWalkOut(me, g_octa.Get(), "client")) Go(CStep::Out);
        return;
    }
    if (Expired(kFireBoundMs)) Fail("no host fire (the client's explosion lane applied no blast for 90 s)");
}

void ClientOut(void* me) {
    Guard();
    const int state = g_walk ? g_walk->state.load() : 2;
    if (state == 0) return;
    if (state == 2) {   // the next ring
        ++g_outRing;
        BeginWalkOut(me, g_octa.Get(), "client");
        return;
    }
    if (ArmOf() == Arm::Attack) {
        if (const char* dirty = RelayDirty()) {   // a last look, over the walk-out
            Fail(dirty);
            return;
        }
        const coop::geom_octa_relay::Counters r = coop::geom_octa_relay::GetCounters();
        UE_LOGI("[OCTA-DRILL] client out t=%llu", Now());
        UE_LOGI("[OCTA-DRILL] attack DONE role=client replayed=%u applied=%u wrote=%u t=%llu",
                r.aggroReplayed - g_relay0.aggroReplayed, r.verdictApplied - g_relay0.verdictApplied,
                r.overwriteWrites - g_relay0.overwriteWrites, Now());
    } else {
        UE_LOGI("[OCTA-DRILL] client out t=%llu", Now());
    }
    Unguard();
    Go(CStep::Done);
}

void ClientTick() {
    void* me = coop::players::Registry::Get().Local();
    if (!me) return;
    switch (g_cstep) {
    case CStep::Ready:
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        if (Fires() && !DamageProbeOn()) {
            Fail("damage_probe=1 is required: each blast must still log its [DMG] line");
            return;
        }
        if (ArmOf() == Arm::Attack && HostWalks()) {
            Fail("the attack arm judges the client's relay: octa_measure_target=host belongs to measure");
            return;
        }
        if (!Guard()) {
            Fail("startInvinc did not resolve on the client's player, so a blast would not leave it alive");
            return;
        }
        if (ArmOf() == Arm::Births && !g_calledEvent) {
            // The night roll's event, which the spawn authority refuses on a client: the call, and the line it logs.
            void* gm = ue_wrap::world_singleton::Gamemode();
            void* fn = gm ? R::FindFunction(R::ClassOf(gm), L"CustomEvent_5") : nullptr;
            ue_wrap::ParamFrame f(fn);
            if (!fn || !f.valid() || !ue_wrap::Call(gm, f)) {
                Fail("the gamemode's CustomEvent_5 did not run");
                return;
            }
            g_calledEvent = true;
            UE_LOGI("[OCTA-DRILL] births client: called CustomEvent_5");
        }
        if (Fires() && HostWalks()) {
            // The control: the host's player takes its place and meets the Octa; this client stands where it spawned,
            // guarded, and has nothing to wait for.
            UE_LOGI("[OCTA-DRILL] client: stands where it spawned, the host walks");
            Go(CStep::Done);
            return;
        }
        // measure: the client takes its place first; the Octa is spawned beside it after.
        Go(Fires() ? CStep::Place : CStep::Find);
        return;
    case CStep::Place:
        ClientPlace(me);
        return;
    case CStep::Find:
        ClientFindMirror();
        return;
    case CStep::Stand:
        ClientStand(me);
        return;
    case CStep::Out:
        ClientOut(me);
        return;
    default:
        return;
    }
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (ArmOf() == Arm::Off || !s) return;
    if (s->role() == coop::net::Role::Host) {
        HostTick(s);
        return;
    }
    if (!s->connected() || g_cstep == CStep::Done) return;
    if (g_cstep == CStep::Ready || Due()) ClientTick();
}

void OnDisconnect() {
    if (ArmOf() == Arm::Off) return;
    if (g_walk && g_walk->state.load() == 0) D::CancelBackgroundWalk(*g_walk);
    g_walk.reset();
    Unguard();
    g_hstep = HStep::Wait;
    g_cstep = CStep::Ready;
    g_nextMs = g_stepMs = 0;
    g_octa.Reset();
    g_base = GS::Counters{};
    g_spawnMs = 0;
    g_aggroSaid = false;
    g_eidSaid = false;
    g_walkerSlot = 0;
    g_puppetStillAt = 0;
    g_wstep = WStep::Stand;
    g_volleysSeen = 0;
    coop::dev::octa_probe::Reset();
    g_appliedBase = g_appliedSeen = 0;
    g_volleys = 0;
    g_relay0 = coop::geom_octa_relay::Counters{};
    g_verdictsSeen = g_laserSeen = 0;
    g_laserPendingMs = 0;
    g_outRing = 0;
    g_lastAppliedMs = 0;
    g_calledEvent = false;
}

}  // namespace coop::dev::octa_drill
