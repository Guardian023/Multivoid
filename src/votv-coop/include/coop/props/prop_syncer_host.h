// coop/props/prop_syncer_host.h -- HOST-side: a client that syncs a prop its own interaction moves.
//
// A client whose interaction moves a prop asks to be its syncer (PropSyncClaim); the host judges the claim
// and answers it (PropSyncAnswer), a grant opens a driven row that names the client as the row's author, and
// the client's PropDrivePose rows and its PropDriveEnd are admitted against that row before the host's copy
// follows them and every other client receives them as a host-authored row (prop_drive_host). MTA's
// unoccupied-vehicle syncer, with the push judged on the server, and the host's own copy kept in step.
//
// HOST-ONLY, game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct PropSyncClaimPayload;
struct PropDriveEndPayload;
}  // namespace coop::net

namespace coop::prop_syncer_host {

// A client's PropSyncClaim from peer slot `slot` (1..kMaxPeers-1). Game thread.
void OnClaim(coop::net::Session& s, const coop::net::PropSyncClaimPayload& p, uint8_t slot);

// A client's PropDriveEnd from peer slot `slot`: the end of the drive it syncs. Validated at the router.
// Game thread.
void OnClientEnd(coop::net::Session& s, const coop::net::PropDriveEndPayload& p, uint8_t slot);

// Per gameplay tick, before prop_drive_host::Tick: drain every origin's PropDrivePose rows. Game thread.
void Admit(coop::net::Session& s);

}  // namespace coop::prop_syncer_host
