// coop/interactables/swinger_join_pose.cpp -- see coop/interactables/swinger_join_pose.h.

#include "coop/interactables/swinger_join_pose.h"

#include "coop/interactables/interactable_sync.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/props/remote_prop.h"  // IsOwnedHere: a remote drive or this peer's own grab owns the lid

#include "ue_wrap/actors/swinger.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"

#include <chrono>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::swinger_join_pose {
namespace {

// The place the host's copy rests at, as it arrived.
struct Rest {
    ue_wrap::FVector  loc;
    ue_wrap::FRotator rot;
};

// A rest pose whose lid this peer's index does not hold yet, or whose copy does not read open yet (the
// ContainerState that opens it may still be parked in the lane's own retry). The latest pose for a key wins.
// `queuedAt` is the container index's pass count when it was parked, for the lane's drop rule.
struct Pending {
    Rest rest;
    uint64_t queuedAt;
    std::chrono::steady_clock::time_point deadline;
};

// An upper bound on the parked poses: the host sends one per open lid, a few dozen at most, and a key is parked
// without bound otherwise. A new key past it is refused, said once per session.
constexpr size_t kPendingCap = 256;
std::unordered_map<std::wstring, Pending> g_pending;
uint64_t g_lastPass = 0;
bool g_capSaid = false;

enum class Outcome { Applied, Owned, Wait };

// Puts the lid at the host's place when it resolves and reads open: a physics teleport with zero velocity that
// leaves it simulating, so it rests where the host's copy does.
Outcome TryApply(const std::wstring& key, const Rest& rest) {
    void* const lid = coop::interactable_sync::ContainerActor(key);
    bool open = false;
    if (!lid || !ue_wrap::swinger::TryReadOpen(lid, open) || !open) return Outcome::Wait;
    if (coop::remote_prop::IsOwnedHere(lid)) return Outcome::Owned;
    const bool moved = ue_wrap::engine::SetActorLocation(lid, rest.loc) &&
                       ue_wrap::engine::SetActorRotation(lid, rest.rot);
    ue_wrap::engine::SetActorRootPhysicsVelocity(lid, ue_wrap::FVector{}, ue_wrap::FVector{});
    UE_LOGI("swinger_join_pose: '%ls' placed at (%.1f, %.1f, %.1f) rot(%.1f, %.1f, %.1f) ok=%d", key.c_str(),
            rest.loc.X, rest.loc.Y, rest.loc.Z, rest.rot.Pitch, rest.rot.Yaw, rest.rot.Roll, moved ? 1 : 0);
    return Outcome::Applied;
}

}  // namespace

void QueueConnectBroadcastForSlot(coop::net::Session* s, int peerSlot) {
    if (!s || s->role() != coop::net::Role::Host) return;
    UE_ASSERT_GAME_THREAD("swinger_join_pose::QueueConnectBroadcastForSlot");
    std::vector<std::pair<std::wstring, void*>> lids;
    coop::interactable_sync::ContainerLids(lids);
    int sent = 0, skipped = 0;
    for (const auto& lid : lids) {
        bool open = false;
        if (!ue_wrap::swinger::TryReadOpen(lid.second, open) || !open) continue;  // a closed lid is locked at its default
        ue_wrap::FVector loc{};
        ue_wrap::FRotator rot{};
        if (lid.first.rfind(L"mv_", 0) != 0 || lid.first.size() > 31 ||
            !ue_wrap::engine::TryGetActorLocation(lid.second, loc) ||
            !ue_wrap::engine::TryGetActorRotation(lid.second, rot)) {
            ++skipped;
            continue;
        }
        coop::net::SwingerRestPosePayload p{};
        coop::net::WireKeyFromString(lid.first, p.key);
        p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
        p.rotPitch = rot.Pitch; p.rotYaw = rot.Yaw; p.rotRoll = rot.Roll;
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::SwingerRestPose, &p, sizeof(p));
        ++sent;
    }
    UE_LOGI("swinger_join_pose: slot %d -- %d open lid(s) sent, %d skipped (no portable key or pose unread)",
            peerSlot, sent, skipped);
}

void OnReliable(const coop::net::SwingerRestPosePayload& p) {
    UE_ASSERT_GAME_THREAD("swinger_join_pose::OnReliable");
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    const Rest rest{ue_wrap::FVector{p.locX, p.locY, p.locZ}, ue_wrap::FRotator{p.rotPitch, p.rotYaw, p.rotRoll}};
    switch (TryApply(key, rest)) {
    case Outcome::Applied:
        g_pending.erase(key);
        return;
    case Outcome::Owned:
        g_pending.erase(key);
        UE_LOGI("swinger_join_pose: '%ls' is driven here now -- its stream owns it, the rest pose is dropped", key.c_str());
        return;
    case Outcome::Wait:
        break;
    }
    if (g_pending.size() >= kPendingCap && g_pending.find(key) == g_pending.end()) {
        if (!g_capSaid) {
            g_capSaid = true;
            UE_LOGW("swinger_join_pose: %zu rest poses already wait -- '%ls' refused", kPendingCap, key.c_str());
        }
        return;
    }
    g_pending[key] = Pending{rest, coop::interactable_sync::ContainerIndexPasses(),
                             std::chrono::steady_clock::now() + coop::interactable_sync::ContainerWaitBackstop()};
}

void Tick() {
    if (g_pending.empty()) return;
    UE_ASSERT_GAME_THREAD("swinger_join_pose::Tick");
    const uint64_t pass = coop::interactable_sync::ContainerIndexPasses();
    if (pass == g_lastPass) return;   // retried once per index pass
    g_lastPass = pass;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        if (TryApply(it->first, it->second.rest) != Outcome::Wait) {
            it = g_pending.erase(it);
        } else if (coop::interactable_sync::ContainerWaitSettled(it->second.queuedAt)) {
            UE_LOGW("swinger_join_pose: '%ls' rest pose dropped -- the container index settled and this peer's lid "
                    "is absent or never read open", it->first.c_str());
            it = g_pending.erase(it);
        } else if (now >= it->second.deadline) {
            UE_LOGW("swinger_join_pose: '%ls' rest pose hit the backstop -- the container index never settled",
                    it->first.c_str());
            it = g_pending.erase(it);
        } else {
            ++it;
        }
    }
}

void OnDisconnect() {
    g_pending.clear();
    g_lastPass = 0;
    g_capSaid = false;
}

}  // namespace coop::swinger_join_pose
