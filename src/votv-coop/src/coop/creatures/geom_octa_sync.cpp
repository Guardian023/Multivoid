// coop/creatures/geom_octa_sync.cpp -- see coop/creatures/geom_octa_sync.h.

#include "coop/creatures/geom_octa_sync.h"

#include "coop/creatures/npc_mirror_brain.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/actors/geom_octa.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace coop::geom_octa_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace EL = coop::element;
namespace GO = ue_wrap::geom_octa;

// The rows: the bodies this lane watches, each declared on the class that owns it. The two notify functions are the
// class's export names for the "fire" and "close" montage sections' finish notify, one GUID each.
UE_FACT_FN(kRowAggro, "geom_octa_sync", L"geomOcta_C", L"aggro", Declared);
UE_FACT_FN(kRowFireFinish, "geom_octa_sync", L"geomOcta_C", L"OnNotifyBegin_1DFC28904024A8FB71C3ACA9DBD440F9",
           Declared);
UE_FACT_FN(kRowCloseFinish, "geom_octa_sync", L"geomOcta_C", L"OnNotifyBegin_9B2108B443F09BA08A3C20ADBECCC562",
           Declared);
UE_FACT_FN(kRowLaserBeginPlay, "geom_octa_sync", L"geomOctaLazer_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowOctaGraph, "geom_octa_sync", L"geomOcta_C", L"ExecuteUbergraph_geomOcta", Declared);

constexpr int kTagAggro = 0x4F435430;   // 'OCT0'
constexpr int kTagFire  = 0x4F435431;   // 'OCT1'
constexpr int kTagClose = 0x4F435432;   // 'OCT2'
constexpr int kTagLaser = 0x4F435433;   // 'OCT3'
constexpr int kTagGraph = 0x4F435434;   // 'OCT4'

// The entry of the Octa's event graph that is the wander (`move`), a cooked byte offset: a mirror that runs it was not
// parked. The stub `move` function runs whether or not the graph's entry does, so the graph is what is watched.
constexpr int32_t kMoveEntry = 5538;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched = false;

Counters g_counters{};
char     g_lastTarget[40] = {};

// The host's count of "fire" finish verdicts per Octa, for its life, keyed by the Octa's element id.
std::unordered_map<uint32_t, uint32_t> g_verdicts;

// `aggro`'s entry as the PRE saw it: whether the Octa had a target, kept for the POST of the same call. The body's
// guard tests the old target, not the argument, so the POST alone cannot tell a passed guard from a refused one.
struct AggroEntry {
    void* octa = nullptr;
    bool  hadTarget = false;
};
AggroEntry g_aggroEntry;

R::ParamOffset g_aggroArg{L"actor"};
R::ParamOffset g_graphEntry{L"EntryPoint"};

bool IsHost() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

// The element id of `actor` when the registry names it an Npc element, else the invalid id.
EL::ElementId NpcEid(void* actor) {
    if (!actor) return EL::kInvalidId;
    const EL::Registry& reg = EL::Registry::Get();
    const EL::ElementId eid = reg.EidForActor(actor);
    if (eid == EL::kInvalidId) return EL::kInvalidId;
    const EL::Element* el = reg.Get(eid);
    return el && el->GetType() == EL::ElementType::Npc ? eid : EL::kInvalidId;
}

void EidText(EL::ElementId eid, char (&out)[16]) {
    if (eid == EL::kInvalidId) std::snprintf(out, sizeof(out), "none");
    else std::snprintf(out, sizeof(out), "%u", static_cast<unsigned>(eid));
}

void TargetText(void* target, char (&out)[40]) {
    if (!target) {
        std::snprintf(out, sizeof(out), "none");
        return;
    }
    const uint8_t peer = coop::players::Registry::Get().PeerIdOfActor(target);
    if (peer != coop::players::kPeerIdUnknown) {
        std::snprintf(out, sizeof(out), "slot:%u", static_cast<unsigned>(peer));
        return;
    }
    const EL::ElementId eid = EL::Registry::Get().EidForActor(target);
    if (eid != EL::kInvalidId) std::snprintf(out, sizeof(out), "element:%u", static_cast<unsigned>(eid));
    else std::snprintf(out, sizeof(out), "other");
}

unsigned long long Now() { return static_cast<unsigned long long>(coop::net::NowMs()); }

// The argument `aggro` was called with, when live; null for None.
void* AggroArgument(const sg::Call& call) {
    const int32_t off = g_aggroArg.Of(call.function);
    if (off < 0 || !call.locals) return nullptr;
    void* arg = nullptr;
    std::memcpy(&arg, call.locals + off, sizeof(arg));
    return arg && R::IsLive(arg) ? arg : nullptr;
}

sg::Verdict OnAggroPre(const sg::Call& call) {
    if (!IsHost()) return sg::Verdict::Run;
    g_aggroEntry = AggroEntry{call.object, GO::TargetActor(call.object) != nullptr};
    return sg::Verdict::Run;
}

void OnAggroPost(const sg::Call& call) {
    if (!IsHost() || g_aggroEntry.octa != call.object) return;
    const bool hadTarget = g_aggroEntry.hadTarget;
    g_aggroEntry = AggroEntry{};
    if (hadTarget) return;   // the guard refused: nothing was decided
    char eid[16], target[40];
    EidText(NpcEid(call.object), eid);
    TargetText(AggroArgument(call), target);
    std::snprintf(g_lastTarget, sizeof(g_lastTarget), "%s", target);
    ++g_counters.hostAggro;
    UE_LOGI("[OCTA] host aggro eid=%s target=%s t=%llu", eid, target, Now());
}

void OnFireFinishPost(const sg::Call& call) {
    if (!IsHost()) return;
    const EL::ElementId id = NpcEid(call.object);
    const uint32_t n = id != EL::kInvalidId ? ++g_verdicts[static_cast<uint32_t>(id)] : 0;
    const int refire = GO::TargetActor(call.object) ? 1 : 0;
    char eid[16];
    EidText(id, eid);
    ++g_counters.hostVerdict;
    UE_LOGI("[OCTA] host verdict eid=%s n=%u refire=%d t=%llu", eid, static_cast<unsigned>(n), refire, Now());
}

void OnCloseFinishPost(const sg::Call& call) {
    char eid[16];
    EidText(NpcEid(call.object), eid);
    ++g_counters.idle;
    UE_LOGI("[OCTA] idle eid=%s t=%llu", eid, Now());
}

void OnLaserPost(const sg::Call& call) {
    char eid[16];
    EidText(NpcEid(GO::LaserOwner(call.object)), eid);
    ++g_counters.laser;
    UE_LOGI("[OCTA] laser eid=%s t=%llu", eid, Now());
}

// After an Octa's event graph ran an entry: a post fires only for a body that ran, so this sees the wander on a
// marked mirror exactly when npc_mirror_brain did not park it.
void OnGraphPost(const sg::Call& call) {
    const int32_t off = g_graphEntry.Of(call.function);
    if (off < 0 || !call.locals) return;
    int32_t entry = -1;
    std::memcpy(&entry, call.locals + off, sizeof(entry));
    if (entry != kMoveEntry || !coop::npc_mirror_brain::IsMarkedMirror(call.object)) return;
    char eid[16];
    EidText(NpcEid(call.object), eid);
    ++g_counters.mirrorMove;
    UE_LOGI("[OCTA] mirror ran move eid=%s", eid);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watched) return;
    g_watched = true;
    struct Watch {
        const ue_wrap::facts::Function* row;
        int                             tag;
        sg::PreFn                       pre;
        sg::PostFn                      post;
    };
    const Watch watches[] = {
        {&kRowAggro, kTagAggro, &OnAggroPre, &OnAggroPost},
        {&kRowFireFinish, kTagFire, nullptr, &OnFireFinishPost},
        {&kRowCloseFinish, kTagClose, nullptr, &OnCloseFinishPost},
        {&kRowLaserBeginPlay, kTagLaser, nullptr, &OnLaserPost},
        {&kRowOctaGraph, kTagGraph, nullptr, &OnGraphPost},
    };
    int registered = 0;
    for (const Watch& w : watches) {
        if (sg::WatchName(*w.row, w.tag, w.pre, w.post)) {
            ++registered;
        } else {
            UE_LOGE("geom_octa_sync: the watch on %ls::%ls did not register -- its [OCTA] line is not printed",
                    w.row->Class(), w.row->Name());
        }
    }
    UE_LOGI("geom_octa_sync: %d/%zu watches registered by class and name, each live once the class loads", registered,
            sizeof(watches) / sizeof(watches[0]));
}

Counters GetCounters() { return g_counters; }

const char* LastAggroTarget() { return g_lastTarget; }

void OnDisconnect() {
    g_verdicts.clear();
    g_counters = Counters{};
    g_lastTarget[0] = '\0';
    g_aggroEntry = AggroEntry{};
}

}  // namespace coop::geom_octa_sync
