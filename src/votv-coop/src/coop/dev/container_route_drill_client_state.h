// coop/dev/container_route_drill_client_state.h -- what the client half of the route census drill shares between
// container_route_drill_client.cpp (the session steps) and container_route_drill_client_hand.cpp (the hand routes R5
// and R4). A private header of coop/dev/container_route_drill_client*.cpp (src tree, not include/).

#pragma once

#include "ue_wrap/core/cached_obj_ref.h"

#include <cstdint>

namespace coop::dev::container_route_drill::detail {

inline constexpr uint64_t kHostBoundMs = 60000;   // the host's raw write, its round trip and the host's chat

enum class Step : uint8_t {
    Arm, Find, Settle, Open, OpenWait, R1, R1Wait, R1Done, R2Refill, R2, R2Call, R2Wait, R3, R3Use, R3Wait,
    R5Close, R5, R5Use, R5Settle, R5Placed, R5Aim, R5Wait,
    R4Close, R4, R4HoldWait, R4Settle, R4Armed, R4Aim, R4Wait,
    ViewOpen, ViewOpenWait, ViewWait, BeforeReady, BeforeWait, BeforeTake, BeforeSettle, DoneWait, Done
};

// The fixtures: the backpack, and the loose drives (the drive the host placed at the vacuum's intake [0], R4's [1]).
extern ue_wrap::CachedObjRef g_pack, g_drive[2];

// The step machine: enter a step, the time since it was entered, the bound that passed (the FAIL line, and the drill
// ends), the census after a route's step and on, and the actor in the player's hand.
void Next(Step s);
bool Expired(uint64_t bound);
void FailAt(const char* phase);
void Finish(const char* label, Step next);
void* HeldActor(void* player);

// The hand routes (the hand file): each is one step of ClientTick's switch; HandReset starts them over.
void StepR5Close();
void StepR5();
void StepR5Use(void* player);
void StepR5Settle(void* player);
void StepR5Placed();
void StepR5Aim(void* player);
void StepR5Wait();
void StepR4Close();
void StepR4(void* player);
void StepR4HoldWait();
void StepR4Settle(void* player);
void StepR4Armed();
void StepR4Aim(void* player);
void StepR4Wait();
void HandReset();

}  // namespace coop::dev::container_route_drill::detail
