// coop/dev/swinger_drill_join.h -- [dev] the joiner arm of the swinger drill (swinger_drill=rejoin): a client that
// comes back into a session whose host's lid is already at rest judges its own copy of that lid against the host's.
//
// Life 1 is the measure arm. The host says a word at each client's world-ready edge, live in the lobby chat: `go`
// while it has given no verdict, else `rest <lane key> <pitch> <yaw> <roll> <x> <y> <z>`, its copy of the verdict's
// door now. The client waits for that word before it picks a lid, and acts on no chat row of a past life (a seeded
// row is retained, a live one is not), so no clock decides which life it is. On `rest` the client does not drag:
// it waits for coop::swinger_join_pose to place its copy, waits for the copy to be still, and judges it against the
// host's rest pose with the settle tolerances. See swinger_drill.h.

#pragma once

#include "ue_wrap/core/types.h"

#include <chrono>
#include <string>

namespace coop::net { class Session; }

namespace coop::dev::swinger_drill_join {

// The tolerances swinger_drill.cpp and this arm share: two reads this close in orientation and place are one rest, a
// copy that rests within the settle pair of another is the same, and a wait on an edge fails after the bound.
inline constexpr float kRestDeg   = 0.3f;
inline constexpr float kRestCm    = 0.5f;
inline constexpr float kSettleDeg = 3.f;
inline constexpr float kSettleCm  = 5.f;
inline constexpr auto  kSampleEvery = std::chrono::milliseconds(250);
inline constexpr auto  kClientBound = std::chrono::seconds(60);

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b);

// HOST: for each client slot that became world-ready since the last call, say its word through `say`: `rest` with
// the pose of the door `verdictLaneKey` names when that is not empty and readable, else `go`.
using SayFn = void (*)(const std::string&);
void GreetReadySlots(coop::net::Session& session, const std::wstring& verdictLaneKey, SayFn say);

// CLIENT: the host's `rest` word arrived: judge the copy of `laneKey` against this place and orientation.
void Begin(const std::string& laneKey, const ue_wrap::FRotator& rot, const ue_wrap::FVector& at);

// True from Begin until the verdict is given: the client does not act while it judges.
bool Active();

// CLIENT: one step of the judge, every drill tick.
void Tick(std::chrono::steady_clock::time_point now);

// The session ended.
void Reset();

}  // namespace coop::dev::swinger_drill_join
