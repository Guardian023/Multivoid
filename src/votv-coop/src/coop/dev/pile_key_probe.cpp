// coop/dev/pile_key_probe.cpp -- see coop/dev/pile_key_probe.h. [MEASURE sync-order-wp5, temporary]

#include "coop/dev/pile_key_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::dev::pile_key_probe {
namespace {

namespace E = ue_wrap::engine;

constexpr float kMovingCmS = 0.5f;   // a pile faster than this at the capture was not at rest

// HOST: this capture's counts.
int g_hostPiles = 0;
int g_hostMoving = 0;

// JOINER: where each native was first seen. A pointer the engine reuses for another object carries another
// internal index, so a changed index is a new native.
struct Seen {
    int32_t idx;
    float   x, y, z;
};
std::unordered_map<void*, Seen> g_firstSeen;
std::vector<Native> g_sweepNatives;                  // the latest pass's walk
std::unordered_map<uint32_t, int> g_missed;          // eid -> the last pass a line was logged for it (0 = the bind)
std::unordered_set<uint32_t> g_rowRotLogged;         // HOST: the eids whose snapshot row turn was logged

float Dist(float ax, float ay, float az, const ue_wrap::FVector& b) {
    const float dx = ax - b.X, dy = ay - b.Y, dz = az - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void Record(const Native* natives, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const Native& n = natives[i];
        auto it = g_firstSeen.find(n.actor);
        if (it != g_firstSeen.end() && it->second.idx == n.idx) continue;
        g_firstSeen[n.actor] = Seen{n.idx, n.x, n.y, n.z};
    }
}

// pass: >= 1 a sweep pass, 0 the bind's miss, -1 the give-up.
void LogLine(uint32_t eid, int pass, const ue_wrap::FVector& key, bool wantClump, uint8_t chipType,
             const Native* natives, size_t count) {
    char label[16];
    if (pass == 0) std::snprintf(label, sizeof(label), "BIND");
    else if (pass < 0) std::snprintf(label, sizeof(label), "GAVEUP");
    else std::snprintf(label, sizeof(label), "%d", pass);

    int typed = 0;
    const Native* bestTyped = nullptr;
    const Native* bestAny = nullptr;
    float bestTypedD = 0.f, bestAnyD = 0.f;
    for (size_t i = 0; i < count; ++i) {
        const Native& n = natives[i];
        const float d = Dist(n.x, n.y, n.z, key);
        if (!bestAny || d < bestAnyD) { bestAny = &n; bestAnyD = d; }
        if (n.isClump != wantClump || n.chipType != chipType) continue;
        ++typed;
        if (!bestTyped || d < bestTypedD) { bestTyped = &n; bestTypedD = d; }
    }
    const char* form = wantClump ? "clump" : "pile";
    if (!bestTyped) {
        if (bestAny)
            UE_LOGI("[PILE-KEY] eid=%u pass=%s key=(%.1f,%.1f,%.1f) form=%s chip=%d nearest-typed native NONE "
                    "(unbound=%d) nearest any native %.1f cm", eid, label, key.X, key.Y, key.Z, form,
                    static_cast<int>(chipType), typed, bestAnyD);
        else
            UE_LOGI("[PILE-KEY] eid=%u pass=%s key=(%.1f,%.1f,%.1f) form=%s chip=%d nearest-typed native NONE "
                    "(unbound=%d) no native at all", eid, label, key.X, key.Y, key.Z, form,
                    static_cast<int>(chipType), typed);
    } else {
        // Its place now: the caller's read may be older than this line (the bind index is built once).
        ue_wrap::FVector now{bestTyped->x, bestTyped->y, bestTyped->z};
        E::TryGetActorLocation(bestTyped->actor, now);
        const float live = Dist(now.X, now.Y, now.Z, key);
        float first = bestTypedD;
        if (auto it = g_firstSeen.find(bestTyped->actor); it != g_firstSeen.end() && it->second.idx == bestTyped->idx)
            first = Dist(it->second.x, it->second.y, it->second.z, key);
        UE_LOGI("[PILE-KEY] eid=%u pass=%s key=(%.1f,%.1f,%.1f) form=%s chip=%d nearest-typed native live=%.1f cm "
                "first-seen=%.1f cm drift=%.1f cm (unbound=%d)", eid, label, key.X, key.Y, key.Z, form,
                static_cast<int>(chipType), live, first, live - first, typed);
    }
    if (pass >= 0) g_missed[eid] = pass;
}

}  // namespace

bool On() {
    static const bool on = coop::config::ResolveInt(::coop::config_registry::rows::pile_key_probe) != 0;
    return on;
}

void BeginHostCapture() {
    g_hostPiles = 0;
    g_hostMoving = 0;
}

void NoteHostPile(uint32_t eid, void* actor, const ue_wrap::FVector& at) {
    ++g_hostPiles;
    const ue_wrap::FVector v = E::GetActorVelocity(actor);
    const float speed = std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
    if (speed <= kMovingCmS) return;
    ++g_hostMoving;
    UE_LOGI("[PILE-KEY] host capture eid=%u at (%.1f,%.1f,%.1f) speed=%.1f cm/s (not at rest at serialize)", eid,
            at.X, at.Y, at.Z, speed);
}

void EndHostCapture() {
    UE_LOGI("[PILE-KEY] host capture: %d piles, %d moving", g_hostPiles, g_hostMoving);
}

void NoteBindIndex(const Native* natives, size_t count) {
    Record(natives, count);
}

void NoteSweepNatives(const Native* natives, size_t count) {
    Record(natives, count);
    g_sweepNatives.assign(natives, natives + count);
}

void LogBindMiss(uint32_t eid, const ue_wrap::FVector& key, bool wantClump, uint8_t chipType,
                 const Native* unbound, size_t count) {
    LogLine(eid, 0, key, wantClump, chipType, unbound, count);
}

void LogPass(uint32_t eid, int pass, const ue_wrap::FVector& key, bool wantClump, uint8_t chipType) {
    if (pass != 1 && pass != 10 && pass != 20 && pass != 30 && pass != 39) return;
    LogLine(eid, pass, key, wantClump, chipType, g_sweepNatives.data(), g_sweepNatives.size());
}

void LogGaveUp(uint32_t eid, const ue_wrap::FVector& key, bool wantClump, uint8_t chipType) {
    LogLine(eid, -1, key, wantClump, chipType, g_sweepNatives.data(), g_sweepNatives.size());
}

void NoteAdopted(uint32_t eid, int pass) {
    if (!g_missed.count(eid)) return;
    UE_LOGI("[PILE-KEY] eid=%u adopted at pass %d", eid, pass);
}

void LogRowRot(uint32_t eid, const ue_wrap::FRotator& root, float lookYawDeg) {
    if (!g_rowRotLogged.insert(eid).second) return;
    UE_LOGI("[ROT-PROBE] row eid=%u root=(%.1f,%.1f,%.1f) look=%.1f", eid, root.Pitch, root.Yaw, root.Roll,
            lookYawDeg);
}

void LogHostNowRot(uint32_t eid, float pitch, float yaw, float roll) {
    UE_LOGI("[ROT-PROBE] host now eid=%u root=(%.1f,%.1f,%.1f)", eid, pitch, yaw, roll);
}

void LogLandRot(uint32_t eid, const ue_wrap::FRotator& settledRoot, const ue_wrap::FRotator& convertRot,
                bool fromRoot) {
    UE_LOGI("[ROT-PROBE] land eid=%u settled root=(%.1f,%.1f,%.1f) crot=(%.1f,%.1f,%.1f) crot read from the root=%d "
            "(the root itself is not written here)", eid, settledRoot.Pitch, settledRoot.Yaw, settledRoot.Roll,
            convertRot.Pitch, convertRot.Yaw, convertRot.Roll, fromRoot ? 1 : 0);
}

void OnDisconnect() {
    g_rowRotLogged.clear();
    g_firstSeen.clear();
    g_sweepNatives.clear();
    g_missed.clear();
    g_hostPiles = 0;
    g_hostMoving = 0;
}

}  // namespace coop::dev::pile_key_probe
