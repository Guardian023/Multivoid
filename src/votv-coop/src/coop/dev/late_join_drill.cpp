// coop/dev/late_join_drill.cpp -- see coop/dev/late_join_drill.h. The client's idle edge, its two readings and the
// done line; the tallies are bind_wait's own.

#include "coop/dev/late_join_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/bind_wait.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/session/join_progress.h"

#include "ue_wrap/core/log.h"

#include <cstdint>

namespace coop::dev::late_join_drill {
namespace {

namespace BW = coop::element::bind_wait;

constexpr uint64_t kSecondReadingMs = 10000;

struct Kind {
    const char* name;
    coop::net::ReliableKind kind;
};

constexpr Kind kKinds[] = {
    {"ContainerContents", coop::net::ReliableKind::ContainerContents},
    {"FloppyBoxState", coop::net::ReliableKind::FloppyBoxState},
    {"LaptopState", coop::net::ReliableKind::LaptopState},
    {"KerfusState", coop::net::ReliableKind::KerfusState},
    {"DrivePayload", coop::net::ReliableKind::DrivePayload},
    {"RackState", coop::net::ReliableKind::RackState},
    {"DriveSlotState", coop::net::ReliableKind::DriveSlotState},
    {"PropDriveEnd", coop::net::ReliableKind::PropDriveEnd},
};

bool g_leftIdle = false;   // the join phase has been non-idle this session
bool g_firstDone = false;  // the at=idle reading is printed
bool g_done = false;       // the DONE line is printed
uint64_t g_idleAtMs = 0;

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::late_join_drill);
    return s;
}

void Print(const char* at) {
    for (const Kind& k : kKinds) {
        const uint16_t n = static_cast<uint16_t>(k.kind);
        UE_LOGI("[LATE-JOIN] tally at=%s kind=%s(%u) held=%u coalesced=%u replayed=%u dropped=%u givenUp=%u neverBound=%u",
                at, k.name, static_cast<unsigned>(n), BW::HeldOf(n), BW::CoalescedOf(n), BW::ReplayedOf(n),
                BW::DroppedOf(n), BW::GivenUpOf(n), BW::NeverBoundOf(n));
    }
    UE_LOGI("[LATE-JOIN] totals at=%s deferred=%u held=%u replayed=%u givenUp=%u refused=%u neverBound=%u holdsNow=%d",
            at, BW::DeferredCount(), BW::HeldCount(), BW::ReplayedCount(), BW::GivenUpCount(), BW::RefusedCount(),
            BW::DropsNeverBound(), BW::HasHolds() ? 1 : 0);
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!Enabled()) return;
    if (g_done || !session || session->role() != coop::net::Role::Client) return;
    const bool idle = coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
    if (!g_firstDone) {
        if (!idle) {
            g_leftIdle = true;
            return;
        }
        if (!g_leftIdle) return;
        Print("idle");
        g_firstDone = true;
        g_idleAtMs = coop::net::NowMs();
        return;
    }
    if (coop::net::NowMs() - g_idleAtMs < kSecondReadingMs) return;
    Print("idle+10s");
    UE_LOGI("[LATE-JOIN] client DONE");
    g_done = true;
}

void OnDisconnect() {
    if (!Enabled()) return;
    g_leftIdle = false;
    g_firstDone = false;
    g_done = false;
    g_idleAtMs = 0;
}

}  // namespace coop::dev::late_join_drill
