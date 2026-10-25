// coop/creatures/geom_octa_relay.cpp -- see coop/creatures/geom_octa_relay.h.

#include "coop/creatures/geom_octa_relay.h"

#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"

#include <cstdint>

namespace coop::geom_octa_relay {
namespace {

namespace EL = coop::element;
namespace N  = coop::net;

// Why an event is refused. One counter and one line per reason.
enum class Reject : uint8_t {
    None, BadOp, BadTargetKind, BadRefire, VerdictWithTarget, TargetWithoutKind, SlotRange, BadOctaEid, BadTargetEid,
    Count
};
constexpr const char* kReasonName[static_cast<int>(Reject::Count)] = {
    "none", "op outside {0, 1}", "targetKind outside {0, 1, 2}", "refire outside {0, 1} or set on an Aggro",
    "a FireVerdict that names a target", "a target on targetKind 0", "a slot at or past kMaxPeers",
    "an Octa id that is not host-allocated", "an element target that is not host-allocated"};

uint32_t g_rejected[static_cast<int>(Reject::Count)] = {};

// The first reason an honest host never produces, else None. Pure.
Reject Check(const N::GeomOctaEventPayload& p) {
    if (p.op != N::kGeomOctaOpAggro && p.op != N::kGeomOctaOpVerdict) return Reject::BadOp;
    if (p.targetKind > N::kGeomOctaTargetElement) return Reject::BadTargetKind;
    if (p.refire > 1 || (p.op == N::kGeomOctaOpAggro && p.refire != 0)) return Reject::BadRefire;
    if (p.op == N::kGeomOctaOpVerdict && (p.targetKind != N::kGeomOctaTargetNone || p.target != 0))
        return Reject::VerdictWithTarget;
    if (p.targetKind == N::kGeomOctaTargetNone && p.target != 0) return Reject::TargetWithoutKind;
    if (!EL::Registry::IsAllowedHostAllocatedEid(p.octaEid)) return Reject::BadOctaEid;
    if (p.targetKind == N::kGeomOctaTargetSlot && p.target >= static_cast<uint32_t>(N::kMaxPeers))
        return Reject::SlotRange;
    if (p.targetKind == N::kGeomOctaTargetElement && !EL::Registry::IsAllowedHostAllocatedEid(p.target))
        return Reject::BadTargetEid;
    return Reject::None;
}

}  // namespace

void OnEvent(const N::GeomOctaEventPayload& p) {
    const Reject why = Check(p);
    if (why == Reject::None) return;   // the replays take it from here
    const uint32_t n = ++g_rejected[static_cast<int>(why)];
    if (n == 1)
        UE_LOGW("geom_octa_relay: a GeomOctaEvent was refused: %s (counted; said once per reason)",
                kReasonName[static_cast<int>(why)]);
}

}  // namespace coop::geom_octa_relay
