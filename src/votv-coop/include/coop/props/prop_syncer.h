// coop/props/prop_syncer.h -- CLIENT-side: a client that syncs a prop its own interaction moves.
//
// The client asks the host to be a prop's syncer with one PropSyncClaim per claim episode; this module
// takes the host's PropSyncAnswer. Game thread only.

#pragma once

namespace coop::net {
struct PropSyncAnswerPayload;
}  // namespace coop::net

namespace coop::prop_syncer {

// CLIENT, game thread: the host's verdict on a claim, or the end of a row it granted. Validated at the
// router.
void OnAnswer(const coop::net::PropSyncAnswerPayload& p);

}  // namespace coop::prop_syncer
