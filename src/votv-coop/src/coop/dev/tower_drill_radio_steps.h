// coop/dev/tower_drill_radio_steps.h -- what the radio arms' two files share: what one client tick reads of the radio
// tower, the bound on a verdict, how an arm ends, and the reset lever's step (tower_drill_radio_arms.cpp), which the
// route in tower_drill_radio.cpp hands the client to once it stands at the top of the ladder. Impl-private: nothing
// outside coop/dev includes it. Game thread.

#pragma once

#include "tower_drill_detail.h"

#include "coop/net/protocol.h"

#include "ue_wrap/devices/radio_tower.h"

#include <cstdint>
#include <string>

namespace coop::dev::tower_drill::detail {

// The tower and the walker, this copy read once, and the host's last row of the tower.
struct RadioView {
    void*                          tower = nullptr;
    void*                          player = nullptr;
    ue_wrap::radio_tower::State    l;
    bool                           haveL = false;
    coop::net::FuseTowerRow        w{};
    bool                           haveW = false;
};

// Failure bound: a lever's effect reaching the host's row and this copy.
inline constexpr uint64_t kRadioVerdictMs = 20000;

// This copy's cells (or fuses) as a string of digits, and whether they are the host's row's cells.
std::string Digits(const ue_wrap::radio_tower::State& st, bool fuses);
std::string RowCells(const coop::net::FuseTowerRow& w);
bool CellsEqual(const ue_wrap::radio_tower::State& l, const coop::net::FuseTowerRow& w);

// How an arm ends (tower_drill_radio.cpp): `RadioFail` says "<arm> FAIL: <why>" then the DONE line.
void RadioDone(const char* verdict);
void RadioFail(const char* why);

// The reset arm's client step (tower_drill_radio_arms.cpp): the client at the top pulls the reset lever, and the
// host's copy re-rolls while this copy's own roll is refused.
void ResetArmTick(const RadioView& v);
void ResetArmReset();

}  // namespace coop::dev::tower_drill::detail
