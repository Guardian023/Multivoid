// coop/interactables/swinger_hold_index.cpp -- see coop/interactables/swinger_hold_index.h.

#include "coop/interactables/swinger_hold_index.h"

#include "coop/element/object_scan_hub.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/swinger.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::swinger_hold_index {
namespace {

namespace R = ue_wrap::reflection;

struct Ref {
    void*   actor;
    int32_t idx;   // GUObjectArray slot, for the purge-safe liveness check
};

std::unordered_map<std::wstring, Ref> g_byKey;
std::vector<std::pair<std::wstring, Ref>> g_scan;   // one pass's hits, swapped in at its end
uint32_t g_gen = 0;                                  // the world generation g_byKey was built in
bool g_built = false;
bool g_registered = false;

void PassBegin(void*, bool) { g_scan.clear(); }

void Match(void*, void* obj) {
    if (R::NameStartsWith(R::NameOf(obj), L"Default__") || !R::IsLive(obj)) return;
    std::wstring key = ue_wrap::prop::GetInteractableKeyString(obj);
    if (key.empty() || key == L"None") return;
    g_scan.emplace_back(std::move(key), Ref{obj, R::InternalIndexOf(obj)});
}

size_t PassComplete(void*, bool isFull, uint32_t worldGen) {
    if (isFull) g_byKey.clear();
    for (auto& f : g_scan) g_byKey[std::move(f.first)] = f.second;
    g_scan.clear();
    if (!isFull) {   // a tail pass adds the newborn and drops the dead
        for (auto it = g_byKey.begin(); it != g_byKey.end();) {
            if (!R::IsLiveByIndex(it->second.actor, it->second.idx)) it = g_byKey.erase(it);
            else ++it;
        }
    }
    g_gen = worldGen;
    g_built = true;
    return g_byKey.size();
}

}  // namespace

void Install() {
    if (g_registered) return;
    g_registered = true;
    coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
        "swinger_hold", nullptr, &ue_wrap::swinger::EnsureResolved, &ue_wrap::swinger::IsSwinger, &PassBegin,
        &Match, &PassComplete, /*settleScans*/ 15});
}

void* Find(const std::wstring& propKey) {
    if (!g_built || g_gen != ue_wrap::world_identity::Generation()) return nullptr;
    const auto it = g_byKey.find(propKey);
    if (it == g_byKey.end() || !R::IsLiveByIndex(it->second.actor, it->second.idx)) return nullptr;
    return it->second.actor;
}

}  // namespace coop::swinger_hold_index
