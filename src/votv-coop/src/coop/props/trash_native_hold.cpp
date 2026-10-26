// coop/props/trash_native_hold.cpp -- see coop/props/trash_native_hold.h.

#include "coop/props/trash_native_hold.h"

#include "coop/element/element.h"
#include "coop/net/session.h"
#include "coop/props/prop_element_tracker.h"  // GetPropElementIdForActor (a bound or minted actor is not a bare load spawn)
#include "coop/session/world_load_episode.h"  // InEpisode: the join's world load
#include "ue_wrap/actors/prop.h"              // IsGarbageClump
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace coop::trash_native_hold {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace PT = coop::prop_element_tracker;

UE_FACT_FN(kRowFinishSpawningActor, "trash_native_hold", L"GameplayStatics", L"FinishSpawningActor", Declared);

// The session the catch reads. The hook carries no session of its own.
std::atomic<coop::net::Session*> g_session{nullptr};

bool     g_hookInstalled = false;   // game thread only
bool     g_gaveUp        = false;
int      g_tries         = 0;
uint64_t g_nextTryMs     = 0;
constexpr uint64_t kResolveRetryMs  = 1000;
constexpr int      kMaxResolveTries = 30;

uint32_t g_heldCount = 0;           // game thread only; per process

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The catch: a post-hook on FinishSpawningActor, which every actor spawn in the process passes, so
// the gates run cheapest first and the host and a solo game leave at the first.
//
// Why a clump held here lands before it moves, and a pile needs no hold: the save load spawns each
// object deferred and finishes it at its saved transform (the gamemode's objectsData loop), so at
// this POST the clump has its saved pose and has run BeginPlay, and no physics step runs inside
// FinishSpawningActor, so the hold lands before its first move. The collider reports its overlaps
// inside the Finish, before this POST; a pile's overlap handler calls `toClump`, which
// trash_morph_gate refuses on a connected client (a name watch resolved from the session's first
// ticks), so nothing has morphed by now. A mirror the host's rows spawn arrives after the load tail
// quiesced, when the episode is over, so this window holds only the save's own bodies.
void OnFinishSpawn(void* /*context*/, void* /*srcObj*/, void* result) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Client || !s->connected()) return;
    if (!coop::world_load_episode::InEpisode()) return;
    if (!result || !GT::IsGameThread()) return;
    if (!ue_wrap::prop::IsGarbageClump(result)) return;
    if (!R::IsLive(result)) return;
    if (PT::GetPropElementIdForActor(result) != coop::element::kInvalidId) return;
    Hold(result);
    if (++g_heldCount == 1 || g_heldCount % 100 == 0)
        UE_LOGI("trash_native_hold: held save-loaded clump %p at its load spawn (#%u) -- tick, physics off, "
                "until the bind or the divergence sweep", result, g_heldCount);
}

}  // namespace

void Hold(void* actor) {
    ue_wrap::engine::SetActorTickEnabled(actor, false);      // a clump's tick wakes its body every second
    ue_wrap::engine::SetActorSimulatePhysics(actor, false);  // kinematic: the host drives it
    ue_wrap::engine::SetActorRootMovable(actor);             // else SetActorLocation silently no-ops on a Static root
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_hookInstalled || g_gaveUp) return;
    const uint64_t now = NowMs();
    if (now < g_nextTryMs) return;
    g_nextTryMs = now + kResolveRetryMs;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowFinishSpawningActor);
    if (fn && ue_wrap::ufunction_hook::InstallPostHook(fn, &OnFinishSpawn)) {
        g_hookInstalled = true;
        UE_LOGI("trash_native_hold: load catch installed on FinishSpawningActor");
    } else if (++g_tries >= kMaxResolveTries) {
        g_gaveUp = true;
        UE_LOGE("trash_native_hold: FinishSpawningActor did not resolve or would not take a post hook in %d "
                "tries; a joiner's save-loaded clumps are not held", kMaxResolveTries);
    }
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::trash_native_hold
