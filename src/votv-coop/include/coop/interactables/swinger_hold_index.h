// coop/interactables/swinger_hold_index.h -- the live swingers (prop_swinger_C: the doors and lids of fridges,
// cabinets, safes) by their prop key, for the receiver of a held-item pose.
//
// A player who drags a swinger leaves it in the player's grabbing_actor, so the held-item stream carries its pose
// keyed by the swinger's own prop key (coop/player/local_streams). A swinger is a child actor, which
// coop/props/prop_key_index leaves out by rule, so that index cannot answer for it. This index is the swinger
// lane's own answer to the same question: a scan-hub consumer over the swinger class, rebuilt by the hub's
// full passes (a world load starts one) and stamped with the world generation, so a dead world's entries read as
// empty. The key is the one the sender reads, GetInteractableKeyString of the actor.
// Game thread, all of it.

#pragma once

#include <string>

namespace coop::swinger_hold_index {

// Register the scan-hub consumer. Idempotent.
void Install();

// The live swinger of the current world holding `propKey`, or null: not indexed yet, no such swinger, a dead
// world's.
void* Find(const std::wstring& propKey);

}  // namespace coop::swinger_hold_index
