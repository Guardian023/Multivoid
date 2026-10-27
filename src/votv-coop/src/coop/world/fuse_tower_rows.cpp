// coop/world/fuse_tower_rows.cpp -- see coop/world/fuse_tower_rows.h.

#include "coop/world/fuse_tower_rows.h"

#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/world/fuse_tower_kind.h"
#include "coop/world/fuse_tower_ops.h"  // the acknowledgements, and my untaken claims

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

namespace coop::fuse_tower_rows {
namespace {

namespace FK = coop::fuse_tower_kind;
namespace R  = ue_wrap::reflection;
using coop::net::FuseTowerPayload;
using coop::net::FuseTowerRow;
using coop::net::kFuseTowers;

static_assert(coop::net::kMaxPeers <= 8, "the owed rows are one bit a slot");

// A client brings a panel to the host's with its own retract; a retract that left the panel idle (a montage
// that would not start) is asked again no sooner than this.
constexpr uint64_t kPanelRetryMs = 2000;

std::atomic<coop::net::Session*> g_session{nullptr};

coop::net::Session* Connected() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->connected()) ? s : nullptr;
}

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// A tower that was not there, or a kept row's tower that has not resolved, is looked for again at most this often:
// the radio tower is the gamemode's, which no class-set move announces.
constexpr uint64_t kLookupMs = 1000;

// This world's towers, by (kind, id); re-read when one dies or the class set moves, and when the radio tower is
// missing, once a second.
FK::Tower g_towers[kFuseTowers] = {};
int32_t   g_towerCount = 0;
uint64_t  g_classVersion = ~0ull;
uint64_t  g_radioLookMs = 0;

bool HaveRadio() {
    for (int32_t i = 0; i < g_towerCount; ++i)
        if (g_towers[i].kind == FK::Kind::Radio) return true;
    return false;
}

bool Towers(bool force = false) {
    const uint64_t v = ue_wrap::object_index::ClassSetVersion();
    bool stale = force || v != g_classVersion || g_towerCount <= 0;
    for (int32_t i = 0; !stale && i < g_towerCount; ++i) stale = !R::IsLive(g_towers[i].obj);
    if (!stale && !HaveRadio()) {
        const uint64_t now = NowMs();
        if (now - g_radioLookMs >= kLookupMs) {
            g_radioLookMs = now;
            stale = true;
        }
    }
    if (stale) {
        const int32_t n = FK::ReadAll(g_towers, kFuseTowers);
        g_towerCount = n < 0 ? 0 : n;
        g_classVersion = v;
    }
    return g_towerCount > 0;
}

void* TowerById(FK::Kind kind, int32_t id) {
    for (int32_t i = 0; i < g_towerCount; ++i)
        if (g_towers[i].kind == kind && g_towers[i].id == id) return g_towers[i].obj;
    return nullptr;
}

bool ReadRows(FuseTowerPayload& p) {
    if (!Towers()) return false;
    p.op = coop::net::kFuseTowerOpRows;
    p.count = 0;
    for (int32_t i = 0; i < g_towerCount && p.count < kFuseTowers; ++i)
        if (FK::ReadRow(g_towers[i], p.rows[p.count])) ++p.count;
    return p.count > 0;
}

bool SameRows(const FuseTowerPayload& a, const FuseTowerPayload& b) {
    return a.count == b.count && std::memcmp(a.rows, b.rows, sizeof(FuseTowerRow) * a.count) == 0;
}

// ---- the host ------------------------------------------------------------------------------------------------

FuseTowerPayload g_lastSent{};
bool     g_haveSent = false;
bool     g_acksDirty = false;
uint8_t  g_owed = 0;
uint8_t  g_answerOwed = 0;  // a refusal's answer, owed to these slots alone
uint64_t g_broadcasts = 0;

// The rows with every slot's acknowledgement: to `onlySlot`, or to every peer when a tower or an acknowledgement
// changed since the last broadcast. False when the towers did not read (nothing was sent).
bool HostSendRows(coop::net::Session* s, int onlySlot = -1) {
    FuseTowerPayload p{};
    if (!ReadRows(p)) return false;
    coop::fuse_tower_ops::FillAcks(p);
    if (onlySlot >= 0) {
        s->SendReliableToSlot(onlySlot, coop::net::ReliableKind::FuseTowerState, &p, sizeof(p));
        return true;
    }
    if (!g_acksDirty && g_haveSent && SameRows(p, g_lastSent)) return true;
    s->SendReliable(coop::net::ReliableKind::FuseTowerState, &p, sizeof(p));
    g_answerOwed = 0;  // every slot's acknowledgement rode this broadcast
    if (++g_broadcasts <= 3 || !SameRows(p, g_lastSent))
        UE_LOGI("fuse_tower: host rows #%llu (%u towers)", static_cast<unsigned long long>(g_broadcasts), p.count);
    g_lastSent = p;
    g_haveSent = true;
    g_acksDirty = false;
    return true;
}

// The answers owed to refused senders, each to its slot alone; one the towers could not be read for stays owed.
void HostPayAnswers(coop::net::Session* s) {
    for (int slot = 1; g_answerOwed && slot < coop::net::kMaxPeers; ++slot)
        if ((g_answerOwed & (1u << slot)) && HostSendRows(s, slot))
            g_answerOwed = static_cast<uint8_t>(g_answerOwed & ~(1u << slot));
}

void HostServeOwed(coop::net::Session* s) {
    if (!g_owed || !Towers()) return;
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot)
        if (g_owed & (1u << slot)) {
            HostSendRows(s, slot);
            UE_LOGI("fuse_tower: host sent slot %d every tower's row at its world-ready", slot);
        }
    g_owed = 0;
}

// ---- a client ------------------------------------------------------------------------------------------------

FuseTowerPayload g_rows{};
bool     g_haveRows = false;
bool     g_rowsWaiting = false;
bool     g_applying = false;
bool     g_sizeWarned = false;
uint64_t g_applied = 0;
uint64_t g_panelAskedMs[kFuseTowers] = {};
uint64_t g_waitLookMs = 0;
Edges    g_edges;

// The host's last row of each tower, by (kind, id), kept until its tower resolves and applied then: a tower may
// resolve long after the rows came (the radio tower is the gamemode's), and each is applied on its own. A tower's
// first applied row is its state, not a player's act, so its changes make no sound of their own.
struct Kept {
    bool         used = false;
    bool         pending = false;   // a row the tower has not taken yet
    bool         applied = false;   // a row has been applied to this tower
    uint8_t      appliedFlags = 0;  // the flags of the row last applied
    FuseTowerRow row{};
};
Kept g_kept[kFuseTowers];

Kept* KeptFor(const FuseTowerRow& r) {
    Kept* spare = nullptr;
    for (Kept& k : g_kept) {
        if (k.used && k.row.kind == r.kind && k.row.id == r.id) return &k;
        if (!k.used && !spare) spare = &k;
    }
    if (!spare) return nullptr;
    *spare = Kept{};
    spare->used = true;
    spare->row = r;
    return spare;
}

// One tower brought to the host's row with my untaken claims on it, each change made as the tower's own graph
// makes it, so it paints, sounds and animates as the host's did.
void ApplyOne(FK::Kind kind, void* tower, Kept& k) {
    FuseTowerRow c = k.row;
    coop::fuse_tower_ops::Overlay(c);
    FK::ApplyCtx ctx;
    ctx.live = k.applied;
    ctx.prevFlags = k.appliedFlags;
    ctx.edges = &g_edges;
    ctx.sizeWarned = &g_sizeWarned;
    FK::ApplyRow(kind, tower, c, ctx);
    k.applied = true;
    k.pending = false;
    k.appliedFlags = c.flags;
}

void ApplyAll() {
    Towers();
    bool waiting = false, any = false;
    g_applying = true;
    for (Kept& k : g_kept) {
        if (!k.used || !k.pending) continue;
        const auto kind = static_cast<FK::Kind>(k.row.kind);
        void* t = TowerById(kind, k.row.id);
        if (!t) {
            waiting = true;
            continue;
        }
        ApplyOne(kind, t, k);
        any = true;
    }
    g_applying = false;
    g_rowsWaiting = waiting;
    if (any) ++g_applied;
}

// Each panel toward the host's: open, closed, or where its montage heads. A client's own montage runs to its end,
// which flips `opened`, before it is asked again.
void ConvergePanels() {
    if (!Towers()) return;
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    const uint64_t now = NowMs();
    for (int i = 0; i < g_rows.count && i < kFuseTowers; ++i) {
        const FuseTowerRow& w = g_rows.rows[i];
        if (w.kind != coop::net::kFuseTowerKindCoordinate) continue;  // the radio tower has no panel
        void* t = TowerById(FK::Kind::Coordinate, w.id);
        if (!t || now - g_panelAskedMs[i] < kPanelRetryMs) continue;
        g_applying = true;
        bool opens = false, ran = false;
        const bool asked = FK::ConvergePanel(FK::Kind::Coordinate, t, player, w, opens, ran);
        g_applying = false;
        if (!asked) continue;
        g_panelAskedMs[i] = now;
        ++g_edges.panels;
        UE_LOGI("fuse_tower: tower %d's panel %s as the host's (retract ran=%d)", w.id, opens ? "opens" : "closes",
                ran ? 1 : 0);
    }
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    auto* s = Connected();
    if (!s) return;
    if (s->role() == coop::net::Role::Host) {
        HostServeOwed(s);
        HostSendRows(s);
        HostPayAnswers(s);
        return;
    }
    if (!g_haveRows || coop::fuse_tower_ops::RedOpen()) return;
    if (g_rowsWaiting) {
        // A kept row whose tower has not resolved: the towers are looked up again, once a second.
        const uint64_t now = NowMs();
        if (now - g_waitLookMs >= kLookupMs) {
            g_waitLookMs = now;
            Towers(true);
            ApplyAll();
        }
    }
    ConvergePanels();
}

void OnRows(const FuseTowerPayload& p) {
    const uint8_t me = coop::players::Registry::Get().LocalPeerId();
    if (me < coop::net::kMaxPeers) coop::fuse_tower_ops::OnAcks(p.ack[me], p.refused[me]);
    g_rows = p;
    g_rows.count = static_cast<uint8_t>(std::min<int>(p.count, kFuseTowers));
    g_haveRows = true;
    for (int i = 0; i < g_rows.count; ++i)
        if (Kept* k = KeptFor(g_rows.rows[i])) {
            k->row = g_rows.rows[i];
            k->pending = true;
        }
    if (coop::fuse_tower_ops::RedOpen()) return;  // [dev] the control: kept for the drill's read, never applied
    ApplyAll();
}

void AdmitRows(FuseTowerPayload& p) {
    // One warning per kind of fault: a host that sends one sends it every tick.
    static bool warned[4] = {};
    int kept = 0;
    const int n = std::min<int>(p.count, kFuseTowers);
    for (int i = 0; i < n; ++i) {
        const FuseTowerRow& r = p.rows[i];
        int fault = -1;
        const char* what = nullptr;
        if (r.kind >= coop::net::kFuseTowerKinds) { fault = 0; what = "a kind that is neither tower"; }
        else if (r.lightCount > coop::net::kFuseTowerMaxLights) { fault = 1; what = "more lights than a puzzle has"; }
        else if (r.fuseCount > coop::net::kFuseTowerMaxFuses) { fault = 2; what = "more fuses than a row holds"; }
        else if (r.kind == coop::net::kFuseTowerKindRadio &&
                 (r.flags & (coop::net::kFuseTowerOpened | coop::net::kFuseTowerAnim))) {
            fault = 3;
            what = "panel flags on the radio tower's row";
        }
        if (fault >= 0) {
            if (!warned[fault]) {
                warned[fault] = true;
                UE_LOGW("fuse_tower: the host's rows carry %s -- that row is dropped", what);
            }
            continue;
        }
        if (kept != i) p.rows[kept] = r;
        ++kept;
    }
    p.count = static_cast<uint8_t>(kept);
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = Connected();
    if (!s || s->role() != coop::net::Role::Host || slot <= 0 || slot >= coop::net::kMaxPeers) return;
    g_owed = static_cast<uint8_t>(g_owed | (1u << slot));
    HostServeOwed(s);
    if (g_owed & (1u << slot)) UE_LOGI("fuse_tower: slot %d's rows wait for the host's towers to resolve", slot);
}

void HostSendNow() {
    g_acksDirty = true;
    if (auto* s = Connected(); s && s->role() == coop::net::Role::Host) HostSendRows(s);
}

void HostAnswerOwed(uint8_t slot) {
    if (slot > 0 && slot < coop::net::kMaxPeers) g_answerOwed = static_cast<uint8_t>(g_answerOwed | (1u << slot));
}

void HostAnswerFlush(uint8_t slot) {
    auto* s = Connected();
    if (!s || s->role() != coop::net::Role::Host || slot == 0 || slot >= coop::net::kMaxPeers) return;
    if ((g_answerOwed & (1u << slot)) && HostSendRows(s, slot))
        g_answerOwed = static_cast<uint8_t>(g_answerOwed & ~(1u << slot));
}

bool Applying() { return g_applying; }

bool LastRows(FuseTowerPayload& out) {
    if (!g_haveRows) return false;
    out = g_rows;
    return true;
}

Edges AppliedEdges() { return g_edges; }

void OnDisconnect() {
    if (g_broadcasts || g_applied)
        UE_LOGI("fuse_tower: session end -- rows broadcast %llu, applied %llu",
                static_cast<unsigned long long>(g_broadcasts), static_cast<unsigned long long>(g_applied));
    g_lastSent = FuseTowerPayload{};
    g_rows = FuseTowerPayload{};
    g_haveSent = g_acksDirty = g_haveRows = g_rowsWaiting = g_applying = g_sizeWarned = false;
    g_owed = g_answerOwed = 0;
    g_broadcasts = g_applied = 0;
    g_edges = Edges{};
    for (auto& t : g_panelAskedMs) t = 0;
    for (Kept& k : g_kept) k = Kept{};
    g_waitLookMs = g_radioLookMs = 0;
    g_towerCount = 0;
    g_classVersion = ~0ull;
}

}  // namespace coop::fuse_tower_rows
