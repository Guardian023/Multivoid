// coop/world/fuse_tower_rows.h -- the coordinate towers' state as rows (FuseTowerState, op 0). The host reads
// its towers each tick and sends every tower's row whenever one or an acknowledgement changed, and all of them to a
// joiner at its world-ready. A client never rolls a tower (coop/world/fuse_tower_ops refuses it), so the rows are
// its towers' only author: it applies each change as the tower's own graph makes it, the painters for the state,
// the repair through solvePuzzle, the lever through moveLever, the panel through its own retract once its montage
// is idle, with its own untaken fuse claims on top. The element-data shape: a client applies the server's value
// and hands its handlers the old one with the new (reference/mtasa-blue/Client/mods/deathmatch/logic/
// CClientEntity.cpp:470-499). Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::fuse_tower_rows {

void Install(coop::net::Session* session);

// HOST: the rows when a tower or an acknowledgement changed, and the joiners owed theirs. CLIENT: rows that came
// before the towers resolved, and each panel brought to the host's.
void Tick();

// CLIENT: the host's rows (router: event_dispatch_world.cpp).
void OnRows(const coop::net::FuseTowerPayload& p);

// HOST: every tower's row to a joiner at its world-ready, or as soon as the towers resolve.
void QueueConnectBroadcastForSlot(int slot);

// HOST: an acknowledgement changed. The rows go now, when a tower or an acknowledgement changed since the last
// ones: a claim's refusal is answered before the sender's next op is taken.
void HostSendNow();

// CLIENT: the rows are being run on this client's towers; a use then is theirs, not a player's.
bool Applying();

// [dev] the tower drill's reads, on a client: the host's last rows (false before any), and the changes it applied as
// the tower's own graph makes them.
bool LastRows(coop::net::FuseTowerPayload& out);
struct Edges {
    uint32_t breaks = 0, repairs = 0, presses = 0, pulls = 0, inserts = 0, levers = 0, fails = 0, panels = 0;
};
Edges AppliedEdges();

void OnDisconnect();

}  // namespace coop::fuse_tower_rows
