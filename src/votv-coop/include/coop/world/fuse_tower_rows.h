
// coop/world/fuse_tower_rows.h -- the fuse towers' state as rows (FuseTowerState, op 0), the coordinate towers'
// and the radio tower's. The host reads its towers each tick and sends every tower's row whenever one or an
// acknowledgement changed, and all of them to a joiner at its world-ready. A client never rolls a tower
// (coop/world/fuse_tower_ops refuses it), so the rows are its towers' only author: it keeps the host's last row of
// each tower and applies it when that tower resolves, as the tower's own graph makes it (what differs by kind is
// coop/world/fuse_tower_kind's), with its own untaken fuse claims on top. The element-data shape: a client applies
// the server's value and hands its handlers the old one with the new
// (reference/mtasa-blue/Client/mods/deathmatch/logic/CClientEntity.cpp:470-499). Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::fuse_tower_rows {

void Install(coop::net::Session* session);

// HOST: the rows when a tower or an acknowledgement changed, and the joiners owed theirs. CLIENT: the kept rows of
// towers that have not resolved (the towers looked up again once a second), and each panel brought to the host's.
void Tick();

// The receive edge of the host's rows (router: event_dispatch_world.cpp): a row no tower sends is dropped, with
// one warning per kind of fault: a kind that is neither tower, more lights than a puzzle has, more fuses than a
// row holds, panel flags on the radio tower's row.
void AdmitRows(coop::net::FuseTowerPayload& p);

// CLIENT: the host's rows, admitted.
void OnRows(const coop::net::FuseTowerPayload& p);

// HOST: every tower's row to a joiner at its world-ready, or as soon as the towers resolve.
void QueueConnectBroadcastForSlot(int slot);

// HOST: an op was taken and its acknowledgement changed. The rows go to every peer now, as the taken op changed a
// tower for all of them. A refusal changes none, and is answered by HostAnswerOwed.
void HostSendNow();

// HOST: a refusal's answer is owed to its sender alone (the rows hold no news for the others). It is sent once at
// the next tick, however many refusals came; Flush sends it now, when a claim's refusal is about to overwrite the
// refused seq the unsent answer carries.
void HostAnswerOwed(uint8_t slot);
void HostAnswerFlush(uint8_t slot);

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
