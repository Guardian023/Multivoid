// coop/dev/grid_drill_upgrade.cpp -- see grid_drill_upgrade.h.

#include "grid_drill_upgrade.h"

#include "grid_drill_checks.h"  // co-located private header (src tree, not include/)
#include "grid_drill_walk.h"  // the walk to the generator

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "coop/world/power_grid.h"
#include "coop/world/power_upgrade.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/generator.h"
#include "ue_wrap/engine/engine.h"            // TryGetActorLocation

#include <chrono>
#include <cstdint>
#include <cstdio>

namespace coop::dev::grid_drill {
namespace {

namespace GEN = ue_wrap::generator;

constexpr int32_t kMaxUpgrade = 6;      // the insert's own gate
constexpr float   kHandedUU   = 400.f;  // the host hands an upgrade where the client stands, and refunds one there
// Failure bounds only: each leg ends on the state it waits for, and these say it never came.
constexpr uint64_t kAckBoundMs = 20000;  // the bare install waits out the host's 10 s bound for its spend
constexpr uint64_t kRowBoundMs = 30000;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ---- the client's legs -----------------------------------------------------------------------------------------

// The bare install goes first: its refusal is also the host's cue to hand the first upgrade, so nothing the host
// hands can lie beside the client before it has counted what was there.
enum class Step : uint8_t { Walk, Arm, Bare, BareAck, Find, Ack, Refund, Done };
Step     g_step = Step::Walk;
uint64_t g_stepMs = 0;
int32_t  g_index = -1;      // the drill's generator's place in the rows
int32_t  g_level0 = 0;      // its upgrades as the legs began
int      g_installs = 0;    // the installs this client made
int      g_nearFloor = 0;   // the upgrades beside this client that no hand-over brought
int      g_nearBefore = 0;  // the upgrades beside this client as it made its last install

void Go(Step s) {
    g_step = s;
    g_stepMs = NowMs();
}

void Fail(const char* why) {
    UE_LOGW("[GRID-DRILL] FAIL on the client: %s", why);
    g_step = Step::Done;
}

void Abandon(const char* why) {
    UE_LOGW("[GRID-DRILL] ABANDONED on the client: %s", why);
    g_step = Step::Done;
}

// The drill's generator's upgrades on this copy, and the host's last word for them. False while either is unread.
bool Levels(int32_t& local, int32_t& host) {
    void* gen = DrillGen();
    GEN::Row r{};
    coop::net::PowerGridPayload rows{};
    if (!gen || g_index < 0 || !GEN::ReadRow(gen, r) || !coop::power_grid::LastRows(rows) || g_index >= rows.count ||
        !rows.rows[g_index].present)
        return false;
    local = r.upgradeLevel;
    host = rows.rows[g_index].upgradeLevel;
    return true;
}

// Once the host has answered every op of this client's: its upgrades here and on the host at the start plus `over`.
bool AnsweredAt(int32_t over, const char* failure) {
    int32_t local = 0, host = 0;
    if (!Levels(local, host)) {
        Abandon("the drill's generator's upgrades are unread");
        return false;
    }
    if (local != g_level0 + over || host != g_level0 + over) {
        char why[192];
        std::snprintf(why, sizeof(why), "%s: this copy's upgrades %d, the host's %d, wanted %d", failure, local, host,
                      g_level0 + over);
        Fail(why);
        return false;
    }
    return true;
}

void ClientTick(void* player) {
    const uint64_t now = NowMs();
    void* gen = DrillGen();
    ue_wrap::FVector me{};
    if (!gen || !ue_wrap::engine::TryGetActorLocation(player, me)) return;
    switch (g_step) {
    case Step::Walk: {
        // Down to the generator by the director: the host judges an install where the player stands.
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        switch (WalkBesideGenerator(player)) {
        case WalkState::Walking:
            return;
        case WalkState::Ended:
            g_step = Step::Done;
            return;
        case WalkState::Arrived:
            Go(Step::Arm);
            return;
        }
        return;
    }
    case Step::Arm: {
        g_index = GEN::IndexOf(gen);
        int32_t local = 0, host = 0;
        if (!Levels(local, host) || local != host) {
            if (now - g_stepMs > kRowBoundMs) Abandon("the host's rows never settled this copy's upgrades");
            return;
        }
        if (local > kMaxUpgrade - 2) {
            Abandon("the drill's generator has no room for two installs");
            return;
        }
        g_level0 = local;
        g_nearFloor = GEN::UpgradesNear(me, kHandedUU, nullptr);
        char step[96];
        std::snprintf(step, sizeof(step), "armed beside generator %d, its upgrades %d", g_index, local);
        if (!Say("client", step)) { g_step = Step::Done; return; }
        Go(Step::Bare);
        return;
    }
    case Step::Bare: {
        // What a forged client sends: an install op with no insert behind it, so no upgrade was spent for it.
        const uint64_t sent0 = coop::power_grid::ClientOpsSent();
        coop::power_grid::DevSendInstall(g_index);
        if (coop::power_grid::ClientOpsSent() == sent0) { Abandon("the bare install op did not go"); return; }
        UE_LOGI("[GRID-DRILL] client: sent an install op with no insert behind it");
        Go(Step::BareAck);
        return;
    }
    case Step::BareAck: {
        if (coop::power_grid::PendingOps() != 0) {
            if (now - g_stepMs > kAckBoundMs) Abandon("the host never answered the bare install op");
            return;
        }
        if (!AnsweredAt(0, "the host took an install nothing was spent for")) return;
        if (!Say("client", "an install nothing was spent for refused")) { g_step = Step::Done; return; }
        Go(Step::Find);
        return;
    }
    case Step::Find: {
        // The upgrade the host handed this client, installed as a player's use of a held one runs: the insert spends
        // it and raises this copy's level, and the op goes to the host. A hand-over is one more upgrade beside it
        // than before; the host hands the second only once it refuses installs.
        void* up = nullptr;
        const int nearby = GEN::UpgradesNear(me, kHandedUU, &up);
        if (!up || nearby <= g_nearFloor) {
            if (now - g_stepMs > kRowBoundMs) Abandon("the host never handed this client an upgrade");
            return;
        }
        g_nearBefore = nearby;
        int32_t before = 0, after = 0, host = 0;
        const uint64_t sent0 = coop::power_grid::ClientOpsSent();
        if (!Levels(before, host) || !GEN::InsertUpgrade(gen, player, up) || !Levels(after, host)) {
            Abandon("the install did not run");
            return;
        }
        if (after != before + 1) { Fail("the install left this copy's upgrades where they were"); return; }
        if (coop::power_grid::ClientOpsSent() == sent0) { Fail("the install sent the host nothing"); return; }
        ++g_installs;
        g_nearFloor = nearby - 1;
        UE_LOGI("[GRID-DRILL] client: installed upgrade %d, this copy's level %d -> %d", g_installs, before, after);
        Go(Step::Ack);
        return;
    }
    case Step::Ack: {
        if (coop::power_grid::PendingOps() != 0) {
            if (now - g_stepMs > kAckBoundMs) Abandon("the host never answered the install");
            return;
        }
        // Both installs end one upgrade over the start: the first taken, the second refused and rolled back.
        const bool first = g_installs == 1;
        if (!AnsweredAt(1, first ? "the host did not take an install whose upgrade was spent beside it"
                                 : "the refused install was not rolled back"))
            return;
        if (!Say("client", first ? "install taken" : "the refused install rolled back")) {
            g_step = Step::Done;
            return;
        }
        Go(first ? Step::Find : Step::Refund);
        return;
    }
    case Step::Refund: {
        // The refused install's upgrade comes back where this client stands: as many beside it as before it spent
        // one.
        if (GEN::UpgradesNear(me, kHandedUU, nullptr) < g_nearBefore) {
            if (now - g_stepMs > kAckBoundMs) Fail("the refused install's upgrade never came back");
            return;
        }
        if (Say("client", "the refund came back beside this client")) SayDone();
        g_step = Step::Done;
        return;
    }
    case Step::Done:
        return;
    }
}

// ---- the host's legs -------------------------------------------------------------------------------------------

// 0 takes the counts, 1 waits for the bare install's refusal and hands the first upgrade, 2 hands the second once the
// first install is taken, 3 judges the refund of the second's refusal; 4 is done.
int      g_hostStage = 0;
uint64_t g_taken0 = 0, g_refused0 = 0, g_refunds0 = 0;

void HostEnd(const char* verdict, const char* why) {
    UE_LOGW("[GRID-DRILL] %s on the host: %s", verdict, why);
    g_hostStage = 4;
}

// An upgrade where the host's puppet of the client stands, which the spawn watcher gives the client.
bool HandUpgrade() {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(1);
    void* body = rp ? rp->GetActor() : nullptr;
    ue_wrap::FVector at{};
    return body && ue_wrap::engine::TryGetActorLocation(body, at) && GEN::SpawnUpgrade(at) != nullptr;
}

void HostTick(coop::net::Session* s) {
    if (!s->IsSlotWorldReady(1)) return;
    const uint64_t taken = coop::power_grid::HostOpsTaken();
    const uint64_t refused = coop::power_grid::HostOpsRefused();
    const uint64_t refunds = coop::power_upgrade::RefundsSpawned();
    switch (g_hostStage) {
    case 0:
        g_taken0 = taken;
        g_refused0 = refused;
        g_refunds0 = refunds;
        Say("host", "armed");
        g_hostStage = 1;
        return;
    case 1:
        // The client's bare install: refused, and nothing refunded for it, since nothing was spent.
        if (refused == g_refused0) return;
        if (refunds != g_refunds0) { HostEnd("FAIL", "an install nothing was spent for was refunded"); return; }
        Say("host", "refused an install nothing was spent for, refunded nothing");
        if (!HandUpgrade()) { HostEnd("ABANDONED", "the upgrade's spawn failed"); return; }
        Say("host", "handed the client an upgrade");
        g_hostStage = 2;
        return;
    case 2:
        if (taken == g_taken0) return;
        coop::power_grid::DevRefuseInstalls(true);
        if (!HandUpgrade()) { HostEnd("ABANDONED", "the second upgrade's spawn failed"); return; }
        Say("host", "took the install, refuses the next, handed a second upgrade");
        g_hostStage = 3;
        return;
    case 3:
        // The second install's upgrade was spent, so its refusal refunds it.
        if (refused < g_refused0 + 2) return;
        if (refunds != g_refunds0 + 1) { HostEnd("FAIL", "the refused install's spent upgrade was not refunded"); return; }
        Say("host", "refused the second install and refunded its upgrade");
        g_hostStage = 4;
        return;
    default:
        return;
    }
}

}  // namespace

void UpgradeTick(coop::net::Session* session) {
    if (session->role() == coop::net::Role::Host) {
        HostTick(session);
        return;
    }
    if (g_step == Step::Done) return;
    if (void* player = coop::players::Registry::Get().Local()) ClientTick(player);
}

void UpgradeOnDisconnect() {
    g_step = Step::Walk;
    g_stepMs = 0;
    g_index = -1;
    g_level0 = 0;
    g_installs = 0;
    g_nearFloor = 0;
    g_nearBefore = 0;
    g_hostStage = 0;
    g_taken0 = g_refused0 = g_refunds0 = 0;
}

}  // namespace coop::dev::grid_drill
