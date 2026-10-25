// coop/dev/octa_drill.cpp -- see coop/dev/octa_drill.h.

#include "coop/dev/octa_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/creature_hit.h"
#include "coop/creatures/geom_octa_sync.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/dev/drill_hold.h"
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
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/world_singleton.h"

#include <cmath>
#include <cstdint>
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
constexpr const wchar_t* kOctaClass   = L"geomOcta_C";
constexpr const wchar_t* kGuardMember = L"startInvinc";

constexpr uint64_t kEveryMs          = 250;
constexpr float    kSpawnRingCm      = 1500.f;  // HOST: the Octa stands this far from the host
constexpr float    kOutRingCm        = 1800.f;  // CLIENT: the walk-out ends this far from the Octa, outside minSphere
constexpr float    kSphereCm         = 1000.f;  // minSphere's radius
constexpr int      kStandpoints      = 8;
constexpr float    kStandReachCm     = 60.f;    // a standpoint is reached within this
constexpr float    kWalkReachCm      = 500.f;   // the walk to the mirror ends this near it
constexpr int      kFirstWalkDeadlineS = 400;
constexpr int      kOutWalkDeadlineS   = 120;
constexpr uint32_t kBlastsWanted     = 3;       // the blasts the client waits for, and the verdicts the host waits for
constexpr uint64_t kAggroBoundMs     = 30000;   // HOST: a puppet inside the sphere this long and no aggro is a dead Octa
constexpr uint64_t kFireBoundMs      = 90000;   // CLIENT: this long with no new blast applied is a stalled wait
constexpr uint64_t kBirthsBoundMs    = 60000;   // births: the enrol, and either arm's mirror, within this

enum class Arm : uint8_t { Off, Births, Measure };
Arm ArmOf() {
    static const Arm arm = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::octa_drill);
        return v == "births" ? Arm::Births : v == "measure" ? Arm::Measure : Arm::Off;
    }();
    return arm;
}

enum class HStep : uint8_t { Wait, Watch, Done };
enum class CStep : uint8_t { Ready, Find, Walk, Stand, Out, Done };
HStep g_hstep = HStep::Wait;
CStep g_cstep = CStep::Ready;

uint64_t g_nextMs = 0;
uint64_t g_stepMs = 0;            // the step began; a FAIL bound counts from here
int      g_invinc = -1;           // the value startInvinc is put back to (-1: not held)
ue_wrap::CachedObjRef g_octa;     // HOST: the Octa it spawned or the gamemode's spawn made
std::shared_ptr<D::BackgroundWalk> g_walk;

// HOST (measure)
GS::Counters g_base{};            // the lane's counters when the Octa stood
uint64_t g_nearAt = 0;            // the first tick a puppet stood inside the sphere
bool     g_aggroSaid = false;
uint32_t g_idleMark = 0;          // the idle count when the third verdict came
bool     g_third = false;
// CLIENT (measure)
uint32_t g_appliedBase = 0;       // the explosion lane's applied count when the client reached the Octa
uint32_t g_appliedSeen = 0;
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

// The first puppet's body that is live, or null.
void* LivePuppetBody() {
    for (uint8_t slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
        void* body = rp ? rp->GetActor() : nullptr;
        if (body && R::IsLive(body)) return body;
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
    if (ArmOf() == Arm::Measure && !DamageProbeOn()) {
        Fail("damage_probe=1 is required: each blast must still log its [DMG] line");
        return;
    }
    if (!Guard()) {
        Fail("startInvinc did not resolve on the host's player, so a blast would not leave it alive");
        return;
    }
    FVector at{};
    if (!RingPoint(me, at)) return;
    if (ArmOf() == Arm::Births) {
        if (!CallGamemodeSpawn(at)) {
            Fail("the gamemode's spawnPropThroughGamemode did not run");
            return;
        }
        UE_LOGI("[OCTA-DRILL] host: births called spawnPropThroughGamemode(cube_0) at=%.0f,%.0f,%.0f", at.X, at.Y,
                at.Z);
    } else {
        void* octa = coop::dev::spawn_npc::SpawnAt(kOctaClass, at);
        if (!octa) {
            Fail("the Octa did not spawn");
            return;
        }
        g_octa.Set(octa);
        g_base = GS::GetCounters();
        UE_LOGI("[OCTA-DRILL] host READY measure: Octa eid=%u at=%.0f,%.0f,%.0f", NpcEidOf(octa), at.X, at.Y, at.Z);
    }
    g_hstep = HStep::Watch;
    g_stepMs = coop::net::NowMs();
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

void HostWatchMeasure() {
    void* octa = g_octa.Get();
    if (!octa) {
        Fail("the Octa is gone before the arm ended");
        return;
    }
    Guard();
    const GS::Counters c = GS::GetCounters();
    FVector op{};
    FVector pp{};
    void* puppet = LivePuppetBody();
    const bool haveBoth = E::TryGetActorLocation(octa, op) && puppet && E::TryGetActorLocation(puppet, pp);
    const float dist = haveBoth ? FlatDist(op, pp) : -1.f;
    const uint64_t now = coop::net::NowMs();
    if (!g_nearAt && haveBoth && dist <= kSphereCm) {
        g_nearAt = now;
        UE_LOGI("[OCTA-DRILL] host: the client's puppet is %.0f cm from the Octa", dist);
    }
    if (!g_aggroSaid && c.hostAggro > g_base.hostAggro) {
        g_aggroSaid = true;
        UE_LOGI("[OCTA-DRILL] host: the Octa aggro'd target=%s", GS::LastAggroTarget());
    }
    if (g_nearAt && !g_aggroSaid && now - g_nearAt > kAggroBoundMs) {
        Fail("no host aggro (the client's puppet stood within 1000 cm of the Octa for 30 s)");
        return;
    }
    if (!g_third && c.hostVerdict - g_base.hostVerdict >= kBlastsWanted) {
        g_third = true;
        g_idleMark = c.idle;
    }
    if (g_third && c.idle > g_idleMark && haveBoth && dist > kSphereCm) {
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
    if (ArmOf() == Arm::Births) HostWatchBirths();
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
    FVector at{};
    if (!E::TryGetActorLocation(octa, at)) return;
    g_octa.Set(octa);
    g_walk = D::StartBackgroundWalk(at, kWalkReachCm, kFirstWalkDeadlineS);
    UE_LOGI("[OCTA-DRILL] client: walking to the Octa mirror eid=%u", eid);
    Go(CStep::Walk);
}

void ClientWalk() {
    const int state = g_walk ? g_walk->state.load() : 2;
    if (state == 0) return;
    if (state == 2) {
        Fail("no walk reached the Octa: the client's route to it ended with none");
        return;
    }
    g_appliedBase = g_appliedSeen = AppliedSum();
    UE_LOGI("[OCTA-DRILL] client at the Octa t=%llu applied=%u", Now(), g_appliedBase);
    Go(CStep::Stand);
}

// Stands until three host blasts have reached this client's explosion lane. The bound counts from the last blast that
// arrived (or from the arrival), so a wait that is still progressing is never cut.
void ClientStand(void* me) {
    Guard();
    const uint32_t now = AppliedSum();
    if (now > g_appliedSeen) {
        g_appliedSeen = now;
        g_stepMs = coop::net::NowMs();
        UE_LOGI("[OCTA-DRILL] client: blast %u of %u applied", now - g_appliedBase, kBlastsWanted);
    }
    if (g_appliedSeen - g_appliedBase >= kBlastsWanted) {
        void* octa = g_octa.Get();
        FVector at{};
        if (!octa || !E::TryGetActorLocation(octa, at)) {
            Fail("the Octa mirror is gone before the walk-out");
            return;
        }
        const std::vector<FVector> ring = D::ReachableStandpoints(me, at, kOutRingCm, kStandpoints, kStandReachCm);
        if (ring.empty()) {
            Fail("no NavMesh route from the Octa reaches a standpoint 1800 cm out: the client cannot leave its sphere");
            return;
        }
        g_walk = D::StartBackgroundWalk(ring[0], kStandReachCm, kOutWalkDeadlineS);
        Go(CStep::Out);
        return;
    }
    if (Expired(kFireBoundMs)) Fail("no host fire (the client's explosion lane applied no blast for 90 s)");
}

void ClientOut() {
    Guard();
    const int state = g_walk ? g_walk->state.load() : 2;
    if (state == 0) return;
    if (state == 2) {
        Fail("the walk out of the Octa's sphere ended with no route");
        return;
    }
    UE_LOGI("[OCTA-DRILL] client out t=%llu", Now());
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
        if (ArmOf() == Arm::Measure && !DamageProbeOn()) {
            Fail("damage_probe=1 is required: each blast must still log its [DMG] line");
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
        Go(CStep::Find);
        return;
    case CStep::Find:
        ClientFindMirror();
        return;
    case CStep::Walk:
        ClientWalk();
        return;
    case CStep::Stand:
        ClientStand(me);
        return;
    case CStep::Out:
        ClientOut();
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
    g_nearAt = 0;
    g_aggroSaid = false;
    g_idleMark = 0;
    g_third = false;
    g_appliedBase = g_appliedSeen = 0;
    g_calledEvent = false;
}

}  // namespace coop::dev::octa_drill
