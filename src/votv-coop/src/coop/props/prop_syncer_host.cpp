// coop/props/prop_syncer_host.cpp -- see coop/props/prop_syncer_host.h.

#include "coop/props/prop_syncer_host.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <cstdint>
#include <vector>

namespace coop::prop_syncer_host {
namespace {

// Each outcome is counted and said at the first and at every 100th, so a client's flood costs the log nothing.
uint32_t g_claimsRefused = 0;
uint32_t g_endsDropped   = 0;
uint32_t g_rowsDropped   = 0;
std::vector<coop::net::PropPoseSnapshot> g_take;   // kept capacity: the drain allocates nothing in steady state

bool Say(uint32_t n) { return n == 1 || (n % 100) == 0; }

}  // namespace

void OnClaim(coop::net::Session& s, const coop::net::PropSyncClaimPayload& p, uint8_t slot) {
    UE_ASSERT_GAME_THREAD("prop_syncer_host::OnClaim");
    if (Say(++g_claimsRefused))
        UE_LOGI("[PROP-SYNC] HOST refused claim eid=%u from slot %u (reason %u): no judge yet (#%u)",
                p.eid, static_cast<unsigned>(slot), static_cast<unsigned>(p.reason), g_claimsRefused);
    coop::net::PropSyncAnswerPayload a{};
    a.eid     = p.eid;
    a.seq     = p.seq;
    a.result  = static_cast<uint8_t>(coop::net::PropSyncResult::Refused);
    a.refusal = static_cast<uint8_t>(coop::net::PropSyncRefusal::Unknown);
    s.SendReliableToSlot(slot, coop::net::ReliableKind::PropSyncAnswer, &a, sizeof(a));
}

void OnClientEnd(coop::net::Session&, const coop::net::PropDriveEndPayload& p, uint8_t slot) {
    UE_ASSERT_GAME_THREAD("prop_syncer_host::OnClientEnd");
    if (Say(++g_endsDropped))
        UE_LOGI("[PROP-SYNC] HOST dropped an End eid=%u gen=%u from slot %u: no row to end (#%u)",
                p.eid, static_cast<unsigned>(p.gen), static_cast<unsigned>(slot), g_endsDropped);
}

void Admit(coop::net::Session& s) {
    UE_ASSERT_GAME_THREAD("prop_syncer_host::Admit");
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        g_take.clear();
        if (!s.TakeRemotePropDriveBatchFrom(slot, g_take)) continue;
        g_rowsDropped += static_cast<uint32_t>(g_take.size());
        if (Say(g_rowsDropped))
            UE_LOGI("[PROP-SYNC] HOST dropped %zu row(s) from slot %d: no row to admit them against (%u so far)",
                    g_take.size(), slot, g_rowsDropped);
    }
}

}  // namespace coop::prop_syncer_host
