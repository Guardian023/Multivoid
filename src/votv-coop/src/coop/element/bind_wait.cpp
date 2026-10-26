// coop/element/bind_wait.cpp -- see coop/element/bind_wait.h.

#include "coop/element/bind_wait.h"

#include "coop/config/config.h"           // ResolveInt, the red arm
#include "coop/config/config_registry.h"  // rows::bind_wait_red
#include "ue_wrap/core/hot_path_guard.h"  // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace coop::element::bind_wait {
namespace {

constexpr size_t kMaxHoldsPerEid = 64;
constexpr size_t kMaxHeldBytes   = 1024 * 1024;

struct Held {
    uint16_t kind;
    int slot;
    Replay fn;
    std::vector<uint8_t> bytes;
};
struct Due {
    uint32_t eid;
    std::vector<Held> holds;
};

std::unordered_set<uint32_t> g_deferred;
std::unordered_map<uint32_t, std::vector<Held>> g_holds;   // eid -> holds, in arrival order
std::vector<Due> g_due;                                    // bound, waiting for the next Pump
std::unordered_set<uint32_t> g_overWarned;                 // eids whose bound was already said
std::unordered_set<uint16_t> g_neverBoundKinds;            // kinds whose first never-bound drop was said
size_t g_heldBytes = 0;                                    // bytes in g_holds and g_due, not yet replayed
uint32_t g_epoch = 0;                                      // bumped by Clear: a Pump in flight stops

uint32_t g_deferredN = 0, g_heldN = 0, g_replayedN = 0, g_givenUpN = 0, g_neverBoundN = 0;
uint32_t g_replayEvents = 0;                               // eids replayed: the log cadence

// The first and every 100th, the log shape of the other receive counters.
bool Loud(uint32_t n) { return n == 1 || n % 100 == 0; }

// The red arm of the drills: Hold always says no, so the caller drops the message as before. Resolved once.
bool Red() {
    static const bool red = coop::config::ResolveInt(coop::config_registry::rows::bind_wait_red) != 0;
    return red;
}

void Release(size_t n) { g_heldBytes = g_heldBytes >= n ? g_heldBytes - n : 0; }

size_t BytesOf(const std::vector<Held>& v) {
    size_t n = 0;
    for (const Held& h : v) n += h.bytes.size();
    return n;
}

}  // namespace

void MarkDeferred(uint32_t eid, const char* why) {
    UE_ASSERT_GAME_THREAD("bind_wait::MarkDeferred");
    if (!g_deferred.insert(eid).second) return;
    if (Loud(++g_deferredN))
        UE_LOGI("[BIND-WAIT] deferred eid=%u (%s) -- %u marked so far", eid, why, g_deferredN);
}

bool IsDeferred(uint32_t eid) {
    UE_ASSERT_GAME_THREAD("bind_wait::IsDeferred");
    return g_deferred.count(eid) > 0;
}

bool Hold(uint32_t eid, uint16_t kind, int senderSlot, Replay fn, const void* bytes, size_t n, bool coalesce) {
    UE_ASSERT_GAME_THREAD("bind_wait::Hold");
    if (Red() || !fn || g_deferred.count(eid) == 0) return false;
    const uint8_t* b = static_cast<const uint8_t*>(bytes);
    std::vector<Held>& v = g_holds[eid];
    // Past a bound the hold is refused but still taken: the caller must not drop it a second way.
    auto refuse = [&]() {
        if (g_overWarned.insert(eid).second)
            UE_LOGW("[BIND-WAIT] eid=%u over its bound -- hold refused (%zu hold(s), %zu B held in all)", eid,
                    v.size(), g_heldBytes);
        return true;
    };
    if (coalesce) {
        for (Held& h : v) {
            if (h.kind != kind) continue;
            if (g_heldBytes - h.bytes.size() + n > kMaxHeldBytes) return refuse();
            Release(h.bytes.size());
            g_heldBytes += n;
            h.slot = senderSlot;
            h.fn = fn;
            h.bytes.assign(b, b + n);
            ++g_heldN;
            return true;
        }
    }
    if (v.size() >= kMaxHoldsPerEid || g_heldBytes + n > kMaxHeldBytes) return refuse();
    v.push_back(Held{kind, senderSlot, fn, std::vector<uint8_t>(b, b + n)});
    g_heldBytes += n;
    ++g_heldN;
    return true;
}

void OnBound(uint32_t eid) {
    UE_ASSERT_GAME_THREAD("bind_wait::OnBound");
    if (g_deferred.erase(eid) == 0) return;
    g_overWarned.erase(eid);
    auto it = g_holds.find(eid);
    if (it == g_holds.end()) return;
    if (!it->second.empty()) g_due.push_back(Due{eid, std::move(it->second)});
    g_holds.erase(it);
}

void Pump() {
    UE_ASSERT_GAME_THREAD("bind_wait::Pump");
    if (g_due.empty()) return;
    // Swapped out first: a replay can defer, hold or bind again, and that lands in the next Pump.
    std::vector<Due> due;
    due.swap(g_due);
    const uint32_t epoch = g_epoch;
    for (Due& d : due) {
        const size_t count = d.holds.size();
        for (Held& h : d.holds) {
            if (g_epoch != epoch) return;   // a replay tore the session down
            Release(h.bytes.size());
            h.fn(h.bytes.data(), h.bytes.size(), h.slot);
            ++g_replayedN;
        }
        if (Loud(++g_replayEvents))
            UE_LOGI("[BIND-WAIT] replayed %zu hold(s) for eid=%u -- %u replayed so far", count, d.eid,
                    g_replayedN);
    }
}

void GiveUp(uint32_t eid, const char* why) {
    UE_ASSERT_GAME_THREAD("bind_wait::GiveUp");
    const bool wasDeferred = g_deferred.erase(eid) > 0;
    g_overWarned.erase(eid);
    size_t dropped = 0;
    if (auto it = g_holds.find(eid); it != g_holds.end()) {
        dropped = it->second.size();
        Release(BytesOf(it->second));
        g_holds.erase(it);
    }
    if (!wasDeferred && dropped == 0) return;
    g_givenUpN += static_cast<uint32_t>(dropped);
    UE_LOGI("[BIND-WAIT] gave up eid=%u -- %zu hold(s) dropped (%s)", eid, dropped, why);
}

void OnSlotLeft(int slot) {
    UE_ASSERT_GAME_THREAD("bind_wait::OnSlotLeft");
    size_t dropped = 0;
    auto drop = [&](std::vector<Held>& v) {
        const size_t before = v.size();
        v.erase(std::remove_if(v.begin(), v.end(),
                               [&](const Held& h) {
                                   if (h.slot != slot) return false;
                                   Release(h.bytes.size());
                                   return true;
                               }),
                v.end());
        dropped += before - v.size();
    };
    for (auto& kv : g_holds) drop(kv.second);
    for (Due& d : g_due) drop(d.holds);
    if (dropped == 0) return;
    g_givenUpN += static_cast<uint32_t>(dropped);
    UE_LOGI("[BIND-WAIT] slot %d left -- %zu hold(s) dropped", slot, dropped);
}

void Clear(const char* why) {
    UE_ASSERT_GAME_THREAD("bind_wait::Clear");
    size_t dropped = 0;
    for (const auto& kv : g_holds) dropped += kv.second.size();
    for (const Due& d : g_due) dropped += d.holds.size();
    if (dropped > 0 || !g_deferred.empty())
        UE_LOGI("[BIND-WAIT] cleared (%s) -- %zu eid(s) deferred, %zu hold(s) dropped", why, g_deferred.size(),
                dropped);
    g_givenUpN += static_cast<uint32_t>(dropped);
    g_deferred.clear();
    g_holds.clear();
    g_due.clear();
    g_overWarned.clear();
    g_heldBytes = 0;
    ++g_epoch;
}

void NoteNeverBound(uint16_t kind, uint32_t eid) {
    UE_ASSERT_GAME_THREAD("bind_wait::NoteNeverBound");
    ++g_neverBoundN;
    if (g_neverBoundKinds.insert(kind).second || Loud(g_neverBoundN))
        UE_LOGW("[BIND-WAIT] kind %u names eid=%u, a prop this peer never bound -- dropped (%u so far)",
                static_cast<unsigned>(kind), eid, g_neverBoundN);
}

uint32_t DropsNeverBound() { UE_ASSERT_GAME_THREAD("bind_wait::DropsNeverBound"); return g_neverBoundN; }
uint32_t DeferredCount()   { UE_ASSERT_GAME_THREAD("bind_wait::DeferredCount");   return g_deferredN; }
uint32_t HeldCount()       { UE_ASSERT_GAME_THREAD("bind_wait::HeldCount");       return g_heldN; }
uint32_t ReplayedCount()   { UE_ASSERT_GAME_THREAD("bind_wait::ReplayedCount");   return g_replayedN; }
uint32_t GivenUpCount()    { UE_ASSERT_GAME_THREAD("bind_wait::GivenUpCount");    return g_givenUpN; }

}  // namespace coop::element::bind_wait
