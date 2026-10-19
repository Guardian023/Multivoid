// coop/dev/swinger_drill.h -- [dev] a MEASUREMENT drill, no verdict: what a dragged hinge door
// (prop_swinger_C) shows on the other peers today. While a player drags a swinger the game leaves it in the
// player's grabbing_actor, so the held-prop stream already treats the door as the held item; this drill
// records what that stream and the receivers do with it.
//   ini swinger_drill=off|measure|observe / env VOTVCOOP_SWINGER_DRILL.
//   measure -- the host and the acting client. The client walks (bot director) to the nearest non-lockable lid
//              its NavMesh reaches, grabs it through the player's own grab chain, feeds the player's
//              input_mouseXY dispatcher (the one its tick broadcasts from the mouse axes, to which the grabbed
//              door binds mouseAssign) for a few seconds in one direction, lets go, and watches the tail.
//   observe -- a second client, which only watches.
// Every peer prints a baseline line per lid, then every 250 ms a sample line for each lid that moved until five
// seconds after its last move. The client also says once a second whether local_streams names the door as the
// held actor and whether a held-item broadcast was made; the host and the observer count each PropPose they
// take and say what it was applied to. Lines are tagged [SWING-DRILL]; each peer's DONE line ends its part.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::dev::swinger_drill {

// True unless the row is `off`.
bool IsEnabled();

// Game thread, once per pump tick; a single check when off.
void Tick(coop::net::Session* session);

// The session ended: the lids, the walk and the counters belong to one world.
void OnDisconnect();

// Seams of the held-prop stream, called by local_streams (the sender) and remote_prop (the receivers). Each is
// a predicate test and a return when the drill is off. Game thread.
//   NoteHeldActor     -- each tick the local player's held actor, as local_streams resolved it (null for none).
//   NoteHeldEdge      -- a new hold began on `heldActor`; `broadcast` is what EnsureHeldItemBroadcast answered.
//   NotePoseEmit      -- a held pose with this key and eid was handed to the session.
//   NotePropPose      -- a PropPose from `slot` drove `actor` (its mesh `mesh`) toward the target pose.
//   NotePropUnresolved -- a PropPose from `slot` named a key and eid this peer resolved to nothing.
void NoteHeldActor(void* heldActor);
void NoteHeldEdge(void* heldActor, bool broadcast, uint32_t eid);
void NotePoseEmit(void* heldActor, const std::wstring& key, uint32_t eid);
void NotePropPose(int slot, const std::wstring& key, uint32_t eid, void* actor, void* mesh,
                  const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot);
void NotePropUnresolved(int slot, const std::wstring& key, uint32_t eid);

}  // namespace coop::dev::swinger_drill
