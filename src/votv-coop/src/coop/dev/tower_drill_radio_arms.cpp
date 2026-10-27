// coop/dev/tower_drill_radio_arms.cpp -- the radio tower drill's reset arm (radioreset, radioresetred) and fuse arm
// (radiofuse, radiofusered); see coop/dev/tower_drill.h. The route that brings the client to the top of the tower's
// ladder is tower_drill_radio.cpp's; what the two files share is tower_drill_radio_steps.h.

// The BP facts the reset arm stands on (bp_cpp radiotower_C, line numbers are its pseudo-C++): the reset lever's
// timeline event TL_reset__reset__EventFunc (the ubergraph's Label_2339, :297) calls scramble (:1496), which blows a
// good fuse if one is left and otherwise sets every cell, presses a random half of them and breaks the tower. A tower
// the drill broke has no good fuse, so the reset re-rolls its cells. The client never rolls a tower: the host runs the
// lever as the client's op and its own scramble is the roll, so the host's cells change and the rows carry the new
// ones to the client, whose copy plays the same timeline and has its own scramble refused at the script gate
// (fuse_tower_ops RollsRefused). A roll can return the cells it had (a tower with the puzzle off always does); the
// arm then pulls again, three pulls at most.

// The fuse arm: a pull of a blown fuse's slot (actionOptionIndex, the look at the slot, :296-:360) puts a fuse in
// the player's hand and empties the slot; an insert (playerUsedOn, :655-:701) takes a prop_fuse_C from the hand into
// an empty slot and, above the two easiest difficulties, destroys it.

#include "tower_drill_radio_steps.h"

#include "coop/world/fuse_tower_ops.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/radio_tower.h"
#include "ue_wrap/engine/engine_mainplayer.h"  // what the hand holds
#include "ue_wrap/world/game_rules.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::tower_drill {
namespace {

using namespace detail;

namespace RT = ue_wrap::radio_tower;
namespace R = ue_wrap::reflection;

constexpr int kMaxPulls = 3;   // the reset lever pulled again when the host's cells came back unchanged

bool        g_acted = false;
uint16_t    g_seq = 0;                 // the op's seq, answered when the rows' ack reaches it
uint64_t    g_pulledMs = 0;
uint64_t    g_rollsBefore = 0;         // this peer's refused rolls when the first pull was made
uint32_t    g_oldLights = 0;           // the host's cells at the latest pull
std::string g_oldCells;
int         g_pulls = 0;
int         g_repeats = 0;
bool        g_sawMoving = false;       // the host's row showed the reset lever running since the latest pull

bool Answered() {
    return static_cast<int16_t>(static_cast<uint16_t>(coop::fuse_tower_ops::LastAck() - g_seq)) >= 0;
}

bool ResetRed() { return ArmOf() == Arm::RadioResetRed; }

// ---- the fuse arm ----------------------------------------------------------------------------------------------

enum class FuseStep : uint8_t { Pull, PullWait, Insert, InsertWait };
FuseStep g_fuse = FuseStep::Pull;
uint64_t g_fuseMs = 0;
uint64_t g_reapedBefore = 0;
int      g_target = -1;                // the slot the fuse goes into
bool     g_classSaid = false;

bool FuseRed() { return ArmOf() == Arm::RadioFuseRed; }

void FuseGo(FuseStep s) {
    g_fuse = s;
    g_fuseMs = NowMs();
}

bool FuseOverdue(uint64_t bound) { return NowMs() - g_fuseMs > bound; }

// What the hand holds: the prop it grabs or carries, null when the hand is empty or the prop is gone.
void* HeldActor(void* player) {
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (!ue_wrap::engine::ReadMainPlayerGrabState(player, gs)) return nullptr;
    void* held = gs.grabbingActor ? gs.grabbingActor : gs.holdingActor;
    return (held && R::IsLive(held)) ? held : nullptr;
}

bool IsFuseProp(void* actor) { return R::ClassNameOf(actor) == L"prop_fuse_C"; }

// The host's slot as it reads, and what the client's act should have made of it.
void FailHostSlot(const RadioView& v, int slot, const char* act) {
    char why[128];
    std::snprintf(why, sizeof why, "the host's slot %d stayed %d after this client's %s", slot,
                  slot >= 0 && slot < v.w.fuseCount ? v.w.fuses[slot] : -1, act);
    RadioFail(why);
}

}  // namespace

namespace detail {

void ResetArmTick(const RadioView& v) {
    if (!v.haveL || !v.haveW) return;
    const bool red = ResetRed();
    if (!g_acted) {
        // A pull again waits for the lever to be at rest on both sides: a use is ignored while it runs.
        const uint8_t busy = coop::net::kFuseTowerAnimating | coop::net::kFuseTowerResetMoving;
        if (g_pulls > 0 && (v.l.animation || (v.w.flags & busy))) return;
        g_oldLights = v.w.lights;
        g_oldCells = RowCells(v.w);
        if (g_pulls == 0) g_rollsBefore = coop::fuse_tower_ops::RollsRefused();
        const uint64_t before = coop::fuse_tower_ops::OpsSent();
        RT::Use(v.tower, v.player, RT::LookAt{RT::Part::Reset, -1});
        if (!red && coop::fuse_tower_ops::OpsSent() == before) {
            RadioFail("the reset lever's press was not sent to the host");
            return;
        }
        g_seq = coop::fuse_tower_ops::LastSentSeq();
        g_acted = true;
        g_sawMoving = false;
        ++g_pulls;
        g_pulledMs = NowMs();
        UE_LOGI("[TOWER-DRILL] client pulled the reset lever (seq %u, pull %d, lightsout=%d): host cells %s, copy %s",
                g_seq, g_pulls, v.l.skipPuzzle ? 0 : 1, g_oldCells.c_str(), Digits(v.l, false).c_str());
        return;
    }
    const uint64_t refused = coop::fuse_tower_ops::RollsRefused() - g_rollsBefore;
    // The control opens the gates: no op goes to the host, so no answer comes and none is waited for.
    const bool answered = red || Answered();
    const bool rerolled = v.w.lights != g_oldLights;
    const bool shown = CellsEqual(v.l, v.w);
    const bool moving = (v.w.flags & coop::net::kFuseTowerResetMoving) != 0;
    if (moving) g_sawMoving = true;
    if (answered && rerolled && shown && refused >= 1) {
        UE_LOGI("[TOWER-DRILL] radioreset PASS (host cells %s -> %s, client rolls refused %llu)", g_oldCells.c_str(),
                RowCells(v.w).c_str(), static_cast<unsigned long long>(refused));
        RadioDone("PASS");
        return;
    }
    // The host's lever ran to its rest and its cells are the ones they were: a roll that repeated them, which the
    // lever is pulled again for (the roll is random; a 2x2 torus repeats about one time in six).
    if (!red && answered && g_sawMoving && !moving && !rerolled &&
        !(v.w.flags & coop::net::kFuseTowerAnimating)) {
        ++g_repeats;
        UE_LOGI("[TOWER-DRILL] radioreset: the host's cells came back as %s (repeat %d of %d)", g_oldCells.c_str(),
                g_repeats, kMaxPulls);
        if (g_repeats >= kMaxPulls) {
            char why[64];
            std::snprintf(why, sizeof why, "the host's cells repeated %d times", g_repeats);
            RadioInconclusive(why);
            return;
        }
        g_acted = false;
        return;
    }
    if (NowMs() - g_pulledMs <= kRadioVerdictMs) return;
    if (!answered) RadioFail("the host never answered the reset lever");
    else if (!rerolled) RadioFail("the host's cells did not change after the reset lever");
    else if (!shown) RadioFail("this copy never showed the host's new cells");
    else RadioFail("this copy's own roll was not refused");
}

void ResetArmReset() {
    g_acted = false;
    g_seq = 0;
    g_pulledMs = 0;
    g_rollsBefore = 0;
    g_oldLights = 0;
    g_oldCells.clear();
    g_pulls = 0;
    g_repeats = 0;
    g_sawMoving = false;
}

void FuseArmTick(const RadioView& v) {
    if (!v.haveL || !v.haveW) return;
    const bool red = FuseRed();
    const int slot = v.fuseSlot;
    switch (g_fuse) {
    case FuseStep::Pull: {
        // Above the two easiest difficulties an insert is a fuse move; on them the game spends no fuse and the
        // insert solves the puzzle, which is not what this arm judges.
        const int difficulty = ue_wrap::game_rules::ReadDifficulty();
        if (difficulty >= 0 && difficulty <= 1) {
            char why[96];
            std::snprintf(why, sizeof why, "difficulty %d: an insert there is no fuse move (the fixture)", difficulty);
            RadioAbandon(why);
            return;
        }
        g_reapedBefore = coop::fuse_tower_ops::Reaped();
        RT::Use(v.tower, v.player, RT::LookAt{RT::Part::Fuse, slot});
        g_seq = coop::fuse_tower_ops::LastSentSeq();
        RT::State now;
        const bool read = RT::Read(v.tower, now);
        UE_LOGI("[TOWER-DRILL] client pulled fuse slot %d (seq %u): this copy reads %d, the host's row %d", slot, g_seq,
                read && slot < now.fuseCount ? now.fuses[slot] : -1, slot < v.w.fuseCount ? v.w.fuses[slot] : -1);
        if (!red && (!read || slot >= now.fuseCount || now.fuses[slot] != 0)) {
            RadioAbandon("the pull did not run on this copy (its slot is not empty)");
            return;
        }
        FuseGo(FuseStep::PullWait);
        return;
    }
    case FuseStep::PullWait: {
        void* held = HeldActor(v.player);
        if (held && !g_classSaid) {
            g_classSaid = true;
            UE_LOGI("[TOWER-DRILL] client's pull put a %ls in the hand", R::ClassNameOf(held).c_str());
        }
        if (red) {
            // The gates are open: the pull ran on this copy alone and no claim went to the host. The host's slot is
            // what the verdict reads, after the window.
            if (held && IsFuseProp(held)) FuseGo(FuseStep::Insert);
            else if (FuseOverdue(kRadioVerdictMs)) FailHostSlot(v, slot, "pull");
            return;
        }
        if (coop::fuse_tower_ops::Reaped() != g_reapedBefore) {
            RadioFail("the host refused the pull and its fuse was reaped from the hand");
            return;
        }
        if (!held) {
            if (FuseOverdue(5000)) RadioAbandon("the hand holds nothing after the pull");
            return;
        }
        if (!IsFuseProp(held)) {
            std::wstring cls = R::ClassNameOf(held);
            char why[128];
            std::snprintf(why, sizeof why, "the pull put a %ls in the hand, not a prop_fuse_C", cls.c_str());
            RadioAbandon(why);
            return;
        }
        const bool taken = Answered() && coop::fuse_tower_ops::PendingClaims() == 0 && slot < v.w.fuseCount &&
                           v.w.fuses[slot] == 0;
        if (!taken) {
            if (FuseOverdue(kRadioVerdictMs))
                Answered() ? FailHostSlot(v, slot, "pull") : RadioFail("the host never answered the pull");
            return;
        }
        FuseGo(FuseStep::Insert);
        return;
    }
    case FuseStep::Insert: {
        void* held = HeldActor(v.player);
        g_target = -1;
        if (slot < v.l.fuseCount && v.l.fuses[slot] == 0) g_target = slot;
        for (int i = 0; i < v.l.fuseCount && g_target < 0; ++i)
            if (v.l.fuses[i] == 0) g_target = i;
        if (!held || g_target < 0) {
            RadioAbandon(!held ? "the hand holds no fuse to insert" : "no empty slot to insert into");
            return;
        }
        RT::InsertFuse(v.tower, v.player, g_target, held);
        g_seq = coop::fuse_tower_ops::LastSentSeq();
        UE_LOGI("[TOWER-DRILL] client inserted its fuse into slot %d (seq %u)", g_target, g_seq);
        FuseGo(FuseStep::InsertWait);
        return;
    }
    case FuseStep::InsertWait: {
        const int target = g_target >= 0 ? g_target : slot;
        // PASS: the host's slot reads as the client moved it (good), the fuse is spent from the hand, and the
        // host answered. The control's host is never touched, so its window runs out.
        if (!red && Answered() && coop::fuse_tower_ops::PendingClaims() == 0 && target < v.w.fuseCount &&
            v.w.fuses[target] == 1 && !HeldActor(v.player)) {
            UE_LOGI("[TOWER-DRILL] radiofuse PASS (slot %d pulled, slot %d inserted, host fuses %s, climbMs=%llu)", slot,
                    target, RowFuses(v.w).c_str(), static_cast<unsigned long long>(v.climbMs));
            RadioDone("PASS");
            return;
        }
        if (FuseOverdue(kRadioVerdictMs)) {
            if (red) FailHostSlot(v, target >= 0 ? target : slot, "insert");
            else if (!Answered()) RadioFail("the host never answered the insert");
            else if (target >= v.w.fuseCount || v.w.fuses[target] != 1) FailHostSlot(v, target, "insert");
            else RadioFail("the fuse is still in the hand after the insert");
        }
        return;
    }
    }
}

void FuseArmReset() {
    g_fuse = FuseStep::Pull;
    g_fuseMs = 0;
    g_reapedBefore = 0;
    g_target = -1;
    g_classSaid = false;
}

}  // namespace detail
}  // namespace coop::dev::tower_drill
