// coop/dev/swinger_drill_join.cpp -- see coop/dev/swinger_drill_join.h.

#include "coop/dev/swinger_drill_join.h"

#include "coop/interactables/interactable_sync.h"
#include "coop/interactables/swinger_join_pose.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/swinger.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace coop::dev::swinger_drill_join {
namespace {

namespace E  = ue_wrap::engine;
namespace PR = ue_wrap::prop;
namespace JP = coop::swinger_join_pose;
using Clock = std::chrono::steady_clock;

std::array<bool, coop::net::kMaxPeers> g_greeted{};   // HOST: the slots that have heard their word this occupancy

bool g_active = false;
std::wstring g_key;
ue_wrap::FRotator g_restRot{};   // the host's copy at its verdict
ue_wrap::FVector  g_restAt{};
Clock::time_point g_since{}, g_nextRead{};
bool g_haveRead = false;
ue_wrap::FRotator g_lastRot{};   // this copy at the last read
ue_wrap::FVector  g_lastAt{};

void Finish(const char* result, const char* detail) {
    UE_LOGI("[SWING-DRILL] rejoin %s: %s", result, detail);
    UE_LOGI("[SWING-DRILL] rejoin DONE");
    g_active = false;
}

}  // namespace

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void GreetReadySlots(coop::net::Session& session, const std::wstring& verdictLaneKey, SayFn say) {
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
        const bool ready = session.IsSlotWorldReady(slot);
        if (!ready) { g_greeted[static_cast<size_t>(slot)] = false; continue; }
        if (g_greeted[static_cast<size_t>(slot)]) continue;
        g_greeted[static_cast<size_t>(slot)] = true;
        void* const door = verdictLaneKey.empty() ? nullptr : coop::interactable_sync::ContainerActor(verdictLaneKey);
        void* const mesh = door ? PR::GetStaticMesh(door) : nullptr;
        ue_wrap::FVector at{};
        if (!mesh || !E::TryGetActorLocation(door, at)) {
            if (!verdictLaneKey.empty())
                UE_LOGW("[SWING-DRILL] host cannot read the verdict's door for slot %d -- says go", slot);
            say("go");
            continue;
        }
        const ue_wrap::FRotator w = E::GetComponentWorldRotation(mesh);
        char line[160];
        std::snprintf(line, sizeof(line), "rest %ls %.2f %.2f %.2f %.1f %.1f %.1f", verdictLaneKey.c_str(), w.Pitch,
                      w.Yaw, w.Roll, at.X, at.Y, at.Z);
        say(line);
    }
}

void Begin(const std::string& laneKey, const ue_wrap::FRotator& rot, const ue_wrap::FVector& at) {
    if (g_active) return;
    g_key.assign(laneKey.begin(), laneKey.end());
    g_restRot = rot;
    g_restAt = at;
    g_since = Clock::now();
    g_nextRead = g_since;
    g_haveRead = false;
    g_active = true;
    UE_LOGI("[SWING-DRILL] rejoin judging key='%ls' against the host's (%.2f, %.2f, %.2f) place (%.1f, %.1f, %.1f)",
            g_key.c_str(), rot.Pitch, rot.Yaw, rot.Roll, at.X, at.Y, at.Z);
}

bool Active() { return g_active; }

void Tick(Clock::time_point now) {
    if (!g_active) return;
    const JP::dev::State st = JP::dev::Outcome(g_key);
    if (st == JP::dev::State::Dropped) { Finish("FAIL", "the pose was dropped"); return; }
    if (now - g_since > kClientBound) {
        Finish("FAIL", st == JP::dev::State::Applied ? "the placed copy never came to rest within the bound"
                       : st == JP::dev::State::Pending ? "the pose never applied within the bound"
                                                       : "no pose arrived within the bound");
        return;
    }
    if (st != JP::dev::State::Applied || now < g_nextRead) return;
    g_nextRead = now + kSampleEvery;
    void* const door = coop::interactable_sync::ContainerActor(g_key);
    void* const mesh = door ? PR::GetStaticMesh(door) : nullptr;
    ue_wrap::FVector at{};
    if (!mesh || !E::TryGetActorLocation(door, at) || !E::IsComponentSimulatingPhysics(mesh)) {
        g_haveRead = false;
        return;
    }
    const ue_wrap::FRotator w = E::GetComponentWorldRotation(mesh);
    if (!g_haveRead || E::RotatorAngleDeg(w, g_lastRot) > kRestDeg || Dist(at, g_lastAt) > kRestCm) {
        g_lastRot = w;
        g_lastAt = at;
        g_haveRead = true;
        return;
    }
    const float deg = E::RotatorAngleDeg(w, g_restRot);
    const float cm = Dist(at, g_restAt);
    bool open = false;
    const int opened = ue_wrap::swinger::TryReadOpen(door, open) ? (open ? 1 : 0) : -1;
    char detail[160];
    std::snprintf(detail, sizeof(detail), "%.2f deg, %.1f cm (need <= %.0f, <= %.0f) | opened %d", deg, cm,
                  kSettleDeg, kSettleCm, opened);
    Finish(deg <= kSettleDeg && cm <= kSettleCm ? "PASS" : "FAIL", detail);
}

void Reset() {
    g_greeted.fill(false);
    g_active = false;
    g_haveRead = false;
}

}  // namespace coop::dev::swinger_drill_join
