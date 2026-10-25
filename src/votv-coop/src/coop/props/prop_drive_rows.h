// coop/props/prop_drive_rows.h -- IMPLEMENTATION-PRIVATE: the driven-prop row and the verbs on the table.
//
// NOT a public header (it lives under src/, not include/). The table is prop_drive_host's g_driven, one row
// per prop under a drive; the row says WHO authors the drive (`syncer`: 0 = the host, n = client slot n).
// prop_syncer_host judges and ends a client's rows through the declarations below, which prop_drive_host.cpp
// defines because it owns the table.
//
// Game-thread only (same contract as prop_drive_host).

#pragma once

#include "coop/net/intent_bucket.h"
#include "coop/net/protocol.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/types.h"

#include <cstdint>

namespace coop::prop_drive_host {

struct Driven {
    ue_wrap::CachedObjRef ref;
    uint32_t              eid = 0;
    coop::net::WireKey    key{};
    uint8_t               gen = 0;
    uint8_t               syncer   = 0;      // 0 = the host authors the row; n = client slot n
    bool                  claimed  = true;   // false: coasting after the verb let go
    bool                  everSent = false;  // a pose of this drive went out; sentLoc/sentRot hold the last
    bool                  resend   = false;  // a joiner needs the pose again, moved or not
    bool                  unreadSaid = false;  // the unreadable-location line went out for this row
    bool                  dead     = false;  // closed this tick; erased after the pass
    ue_wrap::FVector      sentLoc{};
    ue_wrap::FRotator     sentRot{};
    uint64_t              lastMoveMs  = 0;
    uint64_t              nextProbeMs = 0;   // 0 = every tick; set while resting

    // A client row (syncer != 0): the last pose the host admitted from the syncer, when, and whether
    // the row's turn in Tick published it yet. The host never reads the actor for such a row.
    ue_wrap::FVector      admitLoc{};
    ue_wrap::FRotator     admitRot{};
    uint64_t              admitMs     = 0;
    bool                  admittedEver = false;  // a pose of this gen was admitted
    bool                  freshAdmit   = false;  // admitted, not yet published
    coop::net::IntentBucket streamBucket;        // the syncer's rows only
    uint16_t              claimSeq    = 0;       // the claim that opened the row, echoed in its answers
};

// The live row of the prop with element id `eid`, null if none. Game thread.
Driven* FindByEid(uint32_t eid);

// The next claim generation of `eid`, one counter per prop for both authors; 0 is skipped, the stream's
// "no generation". Game thread.
uint8_t BumpGen(uint32_t eid);

// A row for `actor` authored by client `slot`, under the Open refusals (a keyed prop with a host element id,
// in no hand, of a class peers hold); null otherwise. Fresh admission state: nothing admitted, a full
// bucket, the silence clock at now. Game thread.
Driven* OpenClientRow(void* actor, uint8_t slot, const char* why);

// Close `row`: it is skipped by every reader at once and swept after the tick's pass. Game thread.
void EraseRow(Driven& row);

}  // namespace coop::prop_drive_host
