// coop/creatures/geom_octa_relay.h -- the client's half of the Geom Octa lane: the host's decisions replayed into the
// client's own Octa Blueprint. Coop layer; game thread. The host's half is geom_octa_sync.
//
// The Octa's mirror keeps its Blueprint running but for the bodies that decide (coop/creatures/npc_mirror_brain). The
// host's Aggro and FireVerdict arrive as GeomOctaEvent; this module checks each at the receive boundary and refuses
// what no honest host sends, counting each reason and saying it once.

#pragma once

namespace coop::net { struct GeomOctaEventPayload; }

namespace coop::geom_octa_relay {

// One GeomOctaEvent as the dispatcher hands it, on the game thread. Refused, counted and said once per reason: an op
// outside {0, 1}; a targetKind outside {0, 1, 2}; a refire outside {0, 1} or set on an Aggro; a target or targetKind on
// a FireVerdict, or a target on kind 0; a slot at or past kMaxPeers; an Octa or element id that is not host-allocated.
void OnEvent(const coop::net::GeomOctaEventPayload& payload);

}  // namespace coop::geom_octa_relay
