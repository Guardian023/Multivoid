// coop/dev/tower_drill_radio_arms.cpp -- the radio tower drill's reset arm (radioreset); see coop/dev/tower_drill.h.
// The route that brings the client to the top of the tower's ladder is tower_drill_radio.cpp's; what the two files
// share is tower_drill_radio_steps.h.
//
// The BP fact the arm stands on (bp_cpp radiotower_C, line numbers are its pseudo-C++): the reset lever's timeline
// event TL_reset__reset__EventFunc (the ubergraph's Label_2339, :297) calls scramble (:1496), which blows a good fuse
// if one is left and otherwise sets every cell, presses a random half of them and breaks the tower. A tower the
// drill broke has no good fuse, so the reset re-rolls its cells. The client never rolls a tower: the host runs the
// lever as the client's op and its own scramble is the roll, so the host's cells change and the rows carry the new
// ones to the client, whose copy plays the same timeline and has its own scramble refused at the script gate
// (fuse_tower_ops RollsRefused).

#include "tower_drill_radio_steps.h"

#include "coop/world/fuse_tower_ops.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/radio_tower.h"

#include <cstdint>
#include <string>

namespace coop::dev::tower_drill {
namespace {

using namespace detail;

namespace RT = ue_wrap::radio_tower;

bool        g_acted = false;
uint16_t    g_seq = 0;                 // the op's seq, answered when the rows' ack reaches it
uint64_t    g_pulledMs = 0;
uint64_t    g_rollsBefore = 0;         // this peer's refused rolls when the lever was pulled
uint32_t    g_oldLights = 0;           // the host's cells then
std::string g_oldCells;

bool Answered() {
    return static_cast<int16_t>(static_cast<uint16_t>(coop::fuse_tower_ops::LastAck() - g_seq)) >= 0;
}

}  // namespace

namespace detail {

void ResetArmTick(const RadioView& v) {
    if (!v.haveL || !v.haveW) return;
    if (!g_acted) {
        g_oldLights = v.w.lights;
        g_oldCells = RowCells(v.w);
        g_rollsBefore = coop::fuse_tower_ops::RollsRefused();
        const uint64_t before = coop::fuse_tower_ops::OpsSent();
        RT::Use(v.tower, v.player, RT::LookAt{RT::Part::Reset, -1});
        if (coop::fuse_tower_ops::OpsSent() == before) {
            RadioFail("the reset lever's press was not sent to the host");
            return;
        }
        g_seq = coop::fuse_tower_ops::LastSentSeq();
        g_acted = true;
        g_pulledMs = NowMs();
        UE_LOGI("[TOWER-DRILL] client pulled the reset lever (seq %u): the host's cells read %s, this copy's %s", g_seq,
                g_oldCells.c_str(), Digits(v.l, false).c_str());
        return;
    }
    const uint64_t refused = coop::fuse_tower_ops::RollsRefused() - g_rollsBefore;
    const bool answered = Answered();
    const bool rerolled = v.w.lights != g_oldLights;
    const bool shown = CellsEqual(v.l, v.w);
    if (answered && rerolled && shown && refused >= 1) {
        UE_LOGI("[TOWER-DRILL] radioreset PASS (host cells %s -> %s, client rolls refused %llu)", g_oldCells.c_str(),
                RowCells(v.w).c_str(), static_cast<unsigned long long>(refused));
        RadioDone("PASS");
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
}

}  // namespace detail
}  // namespace coop::dev::tower_drill
