// coop/props/container_writer_watch.cpp -- the addObject / takeObj watch on a container's inventory:
// a change notice, never an action. See coop/props/container_writer_watch.h.

#include "coop/props/container_writer_watch.h"

#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/props/container_contents_sync.h"
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>

namespace coop::props::container_writer_watch {
namespace {

namespace sg = ue_wrap::script_gate;
namespace ci = ue_wrap::container_inventory;

// Verb ids: addObject marks dirty; takeObj also arms the container-extraction birth latch that
// prop_drop_intent consumes (a client-extracted item's actor is admitted as a host-authoritative
// drop intent).
constexpr int kVerbDirty   = 1;   // addObject
constexpr int kVerbTakeObj = 2;   // takeObj

UE_FACT_FN(kRowAddObject, "container_writer_watch", nullptr, L"addObject", AnyClass);
UE_FACT_FN(kRowTakeObj,   "container_writer_watch", nullptr, L"takeObj",   AnyClass);

std::atomic<coop::net::Session*> g_session{nullptr};

bool g_verbsRegistered = false;
bool g_verbEntered = false;   // the watch has fired at least once on this peer

// The takeObj-in-flight latch: armed at the verb's entry, consumed by prop_drop_intent at the
// extracted item's FinishSpawn enqueue (the item spawns inside the takeObj call, so the latch is
// live exactly then). Atomic exchange, so the consume is one-shot. A personal-inventory take arms
// it too, harmlessly; the drain's own gates filter those.
std::atomic<bool> g_takeObjInFlight{false};

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

// Fires at the entry of addObject and takeObj on the game thread, a change notice rather than an
// action: it remembers which component was touched. No role gate: the verb is a local virtual
// call, so by the time it is seen the item has already moved on this machine, and an intent the
// host could deny cannot exist here. Gated on the host it dropped every client extraction: the
// client took one of two burgers, the host's slot stayed at two, and the world gained a burger.
sg::Verdict OnVerbEntry(const sg::Call& br) {
    // The first statement, ahead of every filter: a resolved verb name does not prove this callback
    // runs, and a registration once returned true with the callback inert for a whole session. If
    // this line is absent from a log, the lane is dead.
    if (!g_verbEntered) {
        g_verbEntered = true;
        UE_LOGI("container_contents: the verb watch ENTERED for the first time on this peer "
                "(role=%s) -- the addObject/takeObj edge is LIVE",
                IsHost() ? "HOST" : "CLIENT");
    }
    if (!br.object) return sg::Verdict::Run;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return sg::Verdict::Run;
    // The verb filter matches on the name alone, so the context is discriminated here: the first
    // non-propInventory context carrying an addObject would otherwise poison the offset cache for
    // the session.
    if (!ci::IsInventory(br.object)) return sg::Verdict::Run;
    // A takeObj on any inventory component arms the extraction latch; addObject must not.
    if (br.tag == kVerbTakeObj) g_takeObjInFlight.store(true, std::memory_order_relaxed);
    void* owner = ci::OwnerOf(br.object);
    if (!owner) return sg::Verdict::Run;
    const uint32_t eid =
        static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(owner));
    if (eid == static_cast<uint32_t>(coop::element::kInvalidId)) return sg::Verdict::Run;
    // Resolve identity AT THE EDGE; the lane derefs nothing later.
    coop::props::container_contents_sync::MarkHostWrite(eid);
    return sg::Verdict::Run;
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    if (g_verbsRegistered) return;
    g_verbsRegistered =
        sg::WatchName(kRowAddObject, kVerbDirty, &OnVerbEntry, nullptr) &&
        sg::WatchName(kRowTakeObj,   kVerbTakeObj, &OnVerbEntry, nullptr);
    static bool s_saidFailed = false;
    if (!g_verbsRegistered && !s_saidFailed) {
        // Once. This block retries every tick, and the failure it reports is the gate
        // refusing permanently (a full table, or no detour), so an unlatched line here is a
        // warning per tick for the rest of the session.
        s_saidFailed = true;
        UE_LOGW("container_contents: verb registration FAILED -- the lane is inert");
    }
}

void OnDisconnect() { g_verbEntered = false; }

bool TakeObjInFlight() { return g_takeObjInFlight.exchange(false, std::memory_order_relaxed); }

bool VerbWatchEntered() { return g_verbEntered; }

}  // namespace coop::props::container_writer_watch
