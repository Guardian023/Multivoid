// coop/props/prop_syncer_client.cpp -- see coop/props/prop_syncer.h.

#include "coop/props/prop_syncer.h"

#include "coop/net/protocol.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <cstdint>

namespace coop::prop_syncer {

void OnAnswer(const coop::net::PropSyncAnswerPayload& p) {
    UE_ASSERT_GAME_THREAD("prop_syncer::OnAnswer");
    static uint32_t sDropped = 0;
    if (++sDropped == 1 || (sDropped % 100) == 0)
        UE_LOGI("[PROP-SYNC] CLIENT dropped an answer eid=%u result=%u: no claim is open (#%u)", p.eid,
                static_cast<unsigned>(p.result), sDropped);
}

}  // namespace coop::prop_syncer
