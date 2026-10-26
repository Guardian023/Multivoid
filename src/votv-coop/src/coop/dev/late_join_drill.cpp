// coop/dev/late_join_drill.cpp -- see coop/dev/late_join_drill.h. The client's idle edge, its two readings and the
// done line, and its look for the world the host staged (the tallies are bind_wait's own); the host's staging is
// late_join_drill_host.cpp.

#include "coop/dev/late_join_drill.h"

#include "late_join_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/bind_wait.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/session/join_progress.h"

#include "ue_wrap/actors/kerfus.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/drive_chain.h"
#include "ue_wrap/devices/floppybox.h"
#include "ue_wrap/devices/portable_pc.h"

#include <cstdint>
#include <vector>

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
bool g_seen = false;       // the staged-world line is printed
uint64_t g_idleAtMs = 0;
uint64_t g_scanAtMs = 0;

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

// Once a second until printed: one live prop element of each staged class in this client's registry, the PC's lid
// open and the drive the one the host named. The five are the host's staged subjects, which seeds and spawn births
// bring; the rig ends a life on this line.
void LookForStagedWorld() {
    namespace PPC = ue_wrap::portable_pc;
    namespace DC = ue_wrap::drive_chain;
    namespace R = ue_wrap::reflection;
    const uint64_t now = coop::net::NowMs();
    if (g_seen || now - g_scanAtMs < 1000) return;
    g_scanAtMs = now;
    std::vector<coop::element::Registry::ActorIdPair> pairs;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    bool pc = false, box = false, kerfus = false, rack = false, drive = false;
    for (const auto& pr : pairs) {
        if (!pr.actor || !R::IsLiveByIndex(pr.actor, pr.internalIdx)) continue;
        void* cls = R::ClassOf(pr.actor);
        bool opened = false;
        if (PPC::IsPortablePc(pr.actor) && PPC::ReadOpened(pr.actor, opened) && opened) pc = true;
        if (ue_wrap::floppybox::IsFloppyBoxClass(cls)) box = true;
        if (ue_wrap::kerfus::IsKerfus(pr.actor)) kerfus = true;
        if (DC::IsRackClass(cls)) rack = true;
        ue_wrap::signal_dynamic::Row row;
        if (DC::IsDriveClass(cls) && DC::ReadDriveRow(pr.actor, row) && row.name == detail::kDriveName) drive = true;
    }
    if (!(pc && box && kerfus && rack && drive)) return;
    UE_LOGI("[LATE-JOIN] client sees the staged world (pc box kerfus rack drive)");
    g_seen = true;
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!Enabled() || !session) return;
    if (session->role() == coop::net::Role::Host) {
        if (session->connected()) detail::HostTick(*session);
        return;
    }
    if (session->role() != coop::net::Role::Client) return;
    LookForStagedWorld();
    if (g_done) return;
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
    g_seen = false;
    g_idleAtMs = 0;
    g_scanAtMs = 0;
}

}  // namespace coop::dev::late_join_drill
