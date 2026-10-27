// coop/dev/container_route_drill_host.cpp -- the HOST's half of the route census drill; see
// coop/dev/container_route_drill.h.

#include "container_route_drill_detail.h"

#include "coop/dev/container_route_census.h"
#include "coop/dev/drill_chat.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/container_contents_sync.h"
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"   // IsActorRootSimulatingPhysics
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::container_route_drill::detail {
namespace {

namespace E   = ue_wrap::engine;
namespace R   = ue_wrap::reflection;
namespace ci  = ue_wrap::container_inventory;
namespace CV  = ue_wrap::container_view;
namespace cen = coop::dev::container_route_census;
namespace cs  = coop::props::container_contents_sync;

constexpr uint64_t kFillBoundMs    = 5000;   // the spawn to the backpack's inventory binding its slot
constexpr uint64_t kPublishBoundMs = 10000;  // the fill to the first fan-out
constexpr uint64_t kViewBoundMs    = 5000;   // the open verb to the screen showing the backpack
constexpr uint64_t kJudgeBoundMs   = 15000;  // the client's R1 slice sent to the host judging it
constexpr uint64_t kPlaceBoundMs   = 5000;   // the R5 drive's spawn to its element id
constexpr uint64_t kThrownBoundMs  = 30000;  // r4-start to the host holding a drive made by the throw

enum class Step : uint8_t { Arm, Fill, Publish, WaitR1, WaitJudge, WaitRoutes, ViewOpen, ViewRegen, WaitBefore, WaitDone, Done };
Step     g_step = Step::Arm;
uint64_t g_stepMs = 0;
uint64_t g_next = 0;
ue_wrap::CachedObjRef g_pack, g_s, g_r4Prop;
uint32_t g_eid = static_cast<uint32_t>(coop::element::kInvalidId);
int      g_slotsBefore = 0;
uint64_t g_gen0 = 0;
void*    g_slot0 = nullptr;   // the screen's first slot widget before the raw write
int      g_drives0 = 0;
bool     g_opened = false;   // the open verb was sent for the host's screen
int      g_r1Sent = 0;       // the slices the client had sent when it finished R1
uint64_t g_placeMs = 0;      // R5: the drive at the intake spawned
bool     g_r5Placed = false, g_r5Said = false;
bool     g_r4Started = false, g_r4Over = false, g_r4Gone = false;
uint64_t g_r4StartMs = 0;
uint64_t g_ov0 = 0;          // the census's R4 overlaps at r4-start
coop::element::ElementId g_hi = 0;   // the registry's highest live eid at r4-start
uint32_t g_r4Eid = 0;        // the drive the throw made here (0 until found)
std::vector<coop::element::Registry::ActorIdPair> g_snap;
TraceRate g_trHost;

void Next(Step s) { g_step = s; g_stepMs = ::GetTickCount64(); }
bool Expired(uint64_t bound) { return ::GetTickCount64() - g_stepMs > bound; }
bool Due() {
    const uint64_t now = ::GetTickCount64();
    if (now < g_next) return false;
    g_next = now + kCheckEveryMs;
    return true;
}
bool Said(const char* line) {
    return coop::dev::drill_chat::PollDue() && coop::dev::drill_chat::ChatHasLive(line);
}

void Fail(const char* why) {
    Abandon("host", why);
    g_step = Step::Done;
}

// A bound that passed: the FAIL line, and the drill ends.
void FailAt(const char* phase) {
    FailPhase("host", phase);
    g_step = Step::Done;
}

ue_wrap::FVector Offset(const ue_wrap::FVector& at, const ue_wrap::FVector& fwd, float ahead, float side) {
    return {at.X + fwd.X * ahead + fwd.Y * side, at.Y + fwd.Y * ahead - fwd.X * side, at.Z + kDropCm};
}

// The backpack's own put-in, as the player's hand puts an item in. True when it took the item.
bool PutIn(void* pack, void* item) {
    void* inv = ci::InventoryOf(pack);
    void* fn = inv ? R::FindDispatchFunctionCached(R::ClassOf(inv), L"addObject") : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !f.Set<void*>(L"actor", item) || !f.Set<int32_t>(L"insertIndex1", -1) ||
        !ue_wrap::Call(inv, f))
        return false;
    return f.Get<bool>(L"return");
}

// The fixture's own fill, or the refill: two drives put into the backpack the way the player's hand puts one in,
// each destroyed after the put that took, none counted as a route. Returns how many the backpack took.
int AddDrives(void* pack) {
    void* itemCls = R::FindClass(kDriveClass);
    ue_wrap::FVector put{};
    if (!itemCls || !E::TryGetActorLocation(pack, put)) return -1;
    cen::Quiet quiet;
    int added = 0;
    for (int i = 0; i < kItems; ++i) {
        void* item = E::SpawnActor(itemCls, {put.X, put.Y, put.Z + 60.f + 30.f * i});
        if (item && PutIn(pack, item)) {
            ++added;
            E::DestroyActor(item);   // as every BP caller does after a put that took
        }
    }
    return added;
}

void StepArm(coop::net::Session* s) {
    switch (SeamsGate("host")) {   // the first step waits for every watch to be live
        case SeamGate::Wait: return;
        case SeamGate::Dead: g_step = Step::Done; return;
        case SeamGate::Go:   break;
    }
    if (!s->IsSlotWorldReady(1)) return;
    void* puppet = coop::puppet_drive::Puppet(1).GetActor();   // spawned once the peer's pose arrives
    ue_wrap::FVector at{};
    if (!puppet || !R::IsLive(puppet) || !E::TryGetActorLocation(puppet, at)) return;
    void* packCls = ue_wrap::asset_load::LoadObjectByPath(kPackPath);
    if (!packCls) { Fail("the backpack's class does not load"); return; }
    const ue_wrap::FVector fwd = E::GetActorForwardVector(puppet);
    void* pack = E::SpawnActor(packCls, Offset(at, fwd, kAheadCm, 0.f));
    if (!pack || !ci::IsContainer(pack)) { Fail("the backpack did not spawn as a container"); return; }
    g_pack.Set(pack);
    if (!cen::IsBeforeOnly()) {
        // One loose drive to the side, which the client holds and throws into the backpack (R4); the drive its vacuum
        // gathers (R5) is spawned later, at the intake the client reads. The client's mirror arrives where the host put it.
        void* driveCls = R::FindClass(kDriveClass);
        void* b = driveCls ? E::SpawnActor(driveCls, Offset(at, fwd, kSideAheadCm, -kSideCm)) : nullptr;
        if (!b) { Fail("the loose drive did not spawn"); return; }
    }
    Next(Step::Fill);
}

void StepFill() {
    void* pack = g_pack.Get();
    void* inv = pack ? ci::InventoryOf(pack) : nullptr;
    if (!pack) { Fail("the backpack died"); return; }
    if (!inv || !ci::ContentsSlot(inv)) {   // its inventory's own init binds the save slot's contents first
        if (Expired(kFillBoundMs)) Fail("the backpack's inventory never bound its contents slot");
        return;
    }
    const int added = AddDrives(pack);   // the fixture's own fill is no route
    if (added < 0) { Fail("the drive's class or the backpack's place is unread"); return; }
    if (added != kItems) { Fail("the backpack did not take the two drives"); return; }
    Next(Step::Publish);
}

void StepPublish() {
    void* pack = g_pack.Get();
    if (!pack) { Fail("the backpack died"); return; }
    g_eid = EidOf(pack);
    const bool bound = g_eid != static_cast<uint32_t>(coop::element::kInvalidId);
    if (!bound || !cs::SentBaselineKnown(g_eid)) {
        if (Expired(kPublishBoundMs)) Fail(bound ? "the backpack's contents were never published" : "the backpack has no element id");
        return;
    }
    UE_LOGI("[CI-CENSUS] drill host: a backpack holding %d drives before the client's puppet, eid=%u", kItems, g_eid);
    LogHashes("host", g_eid, "published");
    LogSizes("host", "fixture", pack);
    cen::ReportNow("host fixture");
    Next(cen::IsBeforeOnly() ? Step::WaitBefore : Step::WaitR1);
}

// The client's R1 take is done: once the host has judged the slices it sent, the backpack is handed two fresh records
// (the fixture the R2 put-back and the later reads stand on), and the client is told.
void StepWaitR1() {
    std::string rest;
    int sent = 0;
    if (!coop::dev::drill_chat::PollDue() || !coop::dev::drill_chat::LiveRestOf(kPrefixR1Done, &rest)) return;
    if (!IntAfter(rest, "sent", sent)) return;
    g_r1Sent = sent;
    Next(Step::WaitJudge);
}

void StepWaitJudge() {
    const uint64_t judged = cen::HostSlices(1, true) + cen::HostSlices(1, false);
    if (judged < static_cast<uint64_t>(g_r1Sent)) {
        if (Expired(kJudgeBoundMs)) Fail("the client's R1 slice was never judged");
        return;
    }
    void* pack = g_pack.Get();
    if (!pack) { Fail("the backpack died"); return; }
    if (AddDrives(pack) != kItems) { Fail("the backpack did not take the two fresh drives"); return; }
    UE_LOGI("[CI-CENSUS] drill host: two fresh records handed to the backpack (%d record(s) now), after %llu judged slice(s)",
            RecordCount(g_eid), static_cast<unsigned long long>(judged));
    coop::dev::drill_chat::Say(kLineRefilled);
    Next(Step::WaitRoutes);
}

// R5: the drive the client's vacuum gathers is spawned AT the intake the client read once the intake settled in its
// hand, and its element id is said once it has one.
void PlaceR5(int x, int y, int z) {
    void* driveCls = R::FindClass(kDriveClass);
    void* a = driveCls ? E::SpawnActor(driveCls, {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)}) : nullptr;
    if (!a) { FailAt("R5 the drive at the intake did not spawn"); return; }
    g_s.Set(a);
    g_placeMs = ::GetTickCount64();
    UE_LOGI("[CI-CENSUS] drill host: R5 a drive spawned at the client's intake (%d,%d,%d)", x, y, z);
}

void SayR5Placed() {
    const uint32_t eid = EidOf(g_s.Get());
    if (eid == static_cast<uint32_t>(coop::element::kInvalidId)) {
        if (::GetTickCount64() - g_placeMs > kPlaceBoundMs) FailAt("R5 the drive at the intake got no element id");
        return;
    }
    char say[48];
    std::snprintf(say, sizeof(say), "%seid=%u", kPrefixR5Placed, eid);
    coop::dev::drill_chat::Say(say);
    g_r5Said = true;
}

// The highest element id live in the registry now.
coop::element::ElementId HighestLiveEid() {
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, g_snap);
    coop::element::ElementId hi = 0;
    for (const auto& p : g_snap) hi = p.id > hi ? p.id : hi;
    return hi;
}

// The drive the host's drop path made for the client's throw: the first prop_drive_C element above the id recorded at
// r4-start. True once found and named to the client.
bool FindThrownDrive() {
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, g_snap);
    coop::element::ElementId best = coop::element::kInvalidId;
    for (const auto& p : g_snap)
        if (p.id > g_hi && p.id < best && R::ClassNameOf(coop::element::LivePropActor(p.id)) == kDriveClass) best = p.id;   // null-safe
    if (best == coop::element::kInvalidId) return false;
    g_r4Prop.Set(coop::element::LivePropActor(best));
    g_r4Eid = best;
    char say[48];
    std::snprintf(say, sizeof(say), "%seid=%u", kPrefixR4Thrown, best);
    coop::dev::drill_chat::Say(say);
    UE_LOGI("[CI-CENSUS] drill host: the throw made a drive here, eid=%u (registry highest at r4-start %u)", best, g_hi);
    return true;
}

// From the found drive to the client's done line, one read per check, with the clock the client's lines carry.
void TraceR4() {
    void* prop = g_r4Prop.Get();
    ue_wrap::FVector at{};
    const bool live = prop && E::TryGetActorLocation(prop, at);
    if (!live && !g_r4Gone) {
        g_r4Gone = true;
        UE_LOGI("[CI-CENSUS] r4 host eid=%u gone t=%llu", g_r4Eid, static_cast<unsigned long long>(cen::UnixMs()));
    }
    if (live && g_trHost.Take())
        UE_LOGI("[CI-CENSUS] r4 host eid=%u sim=%d t=%llu pos=(%.1f,%.1f,%.1f)", g_r4Eid, E::IsActorRootSimulatingPhysics(prop) ? 1 : 0,
                static_cast<unsigned long long>(cen::UnixMs()), at.X, at.Y, at.Z);
}

// The client drives R5 and R4 here; each of its start lines is answered, and the host's side of R4 is traced.
void StepWaitRoutes() {
    std::string rest;
    int x = 0, y = 0, z = 0, held = 0;
    if (!g_r5Placed && coop::dev::drill_chat::LiveRestOf(kPrefixR5Intake, &rest) && IntAfter(rest, "x", x) &&
        IntAfter(rest, "y", y) && IntAfter(rest, "z", z)) {
        g_r5Placed = true;
        PlaceR5(x, y, z);
        if (g_step == Step::Done) return;
    }
    if (g_r5Placed && !g_r5Said) SayR5Placed();
    if (g_step == Step::Done) return;
    if (!g_r4Started && coop::dev::drill_chat::LiveRestOf(kPrefixR4Start, &rest) && IntAfter(rest, "eid", held)) {
        g_r4Started = true;
        g_r4StartMs = ::GetTickCount64();
        g_ov0 = cen::R4Overlaps();
        g_hi = HighestLiveEid();
        UE_LOGI("[CI-CENSUS] drill host: r4-start, the client holds eid=%d, registry highest live eid %u", held, g_hi);
    }
    if (g_r4Started && !g_r4Eid && !FindThrownDrive() && ::GetTickCount64() - g_r4StartMs > kThrownBoundMs) {
        FailAt("R4 the host made no drive for the throw");
        return;
    }
    if (g_r4Started && !g_r4Over) {
        if (coop::dev::drill_chat::ChatHasLive(kLineR4Done)) {
            g_r4Over = true;
            UE_LOGI("[CI-CENSUS] drill host: R4 overlap n=%llu", static_cast<unsigned long long>(cen::R4Overlaps() - g_ov0));
        } else if (g_r4Eid) {
            TraceR4();
        }
    }
    if ((!g_r4Started || g_r4Eid) && coop::dev::drill_chat::ChatHasLive(kLineRoutesDone)) Next(Step::ViewOpen);
}

// Read 7: the host's own screen onto the backpack, one raw write the way an accepted client slice is applied, and
// whether the open screen rebuilt (its gen_player body ran) or still shows the slots it had.
void StepViewOpen() {
    void* pack = g_pack.Get();
    if (!pack) { Fail("the backpack died"); return; }
    if (!g_opened) {
        g_opened = true;
        if (!CV::Open(pack)) Fail("the host's screen could not be opened onto the backpack");
        return;
    }
    if (CV::Viewed() == pack) {
        g_slotsBefore = SlotCount(Screen());
        g_gen0 = cen::GenCalls();
        g_slot0 = SlotAt(Screen(), 0);
        if (!cs::DevRawAppend(g_eid, /*publish=*/true)) { Fail("the raw write to the backpack did not apply"); return; }
        UE_LOGI("[CI-CENSUS] drill host: one raw write applied to eid=%u (%d record(s) now), published", g_eid,
                RecordCount(g_eid));
        Next(Step::ViewRegen);
        return;
    }
    if (Expired(kViewBoundMs)) Fail("the screen never showed the backpack");
}

void StepViewRegen() {
    const bool regen = ScreenRebuilt(g_gen0, g_slot0);
    if (!regen && !Expired(kRegenWindowMs)) return;
    UE_LOGI("[CI-CENSUS] host view regen=%d slots %d -> %d", regen ? 1 : 0, g_slotsBefore, SlotCount(Screen()));
    CV::Close();
    LogHashes("host", g_eid, "after the raw write");
    LogSizes("host", "after the raw write", g_pack.Get());
    cen::ReportNow("host read 7");
    Next(Step::WaitBefore);
}

void StepWaitBefore() {
    if (!Said(kLineBeforeReady)) return;
    if (!cs::DevRawAppend(g_eid, /*publish=*/false)) { Fail("the unpublished raw write did not apply"); return; }
    g_drives0 = LiveDrives();
    UE_LOGI("[CI-CENSUS] drill host: a raw write NOT published to eid=%u (%d record(s) here), the client's base is stale",
            g_eid, RecordCount(g_eid));
    coop::dev::drill_chat::Say(kLineBeforeWritten);
    Next(Step::WaitDone);
}

void StepWaitDone() {
    std::string rest;
    int rb = 0, ic = 0;
    if (!coop::dev::drill_chat::PollDue() || !coop::dev::drill_chat::LiveRestOf(kPrefixClientDone, &rest)) return;
    if (!IntAfter(rest, "rb", rb) || !IntAfter(rest, "ic", ic)) return;
    const int ih = LiveDrives() > g_drives0 ? 1 : 0;
    char say[48];
    std::snprintf(say, sizeof(say), "%sih=%d", kPrefixHostDone, ih);
    coop::dev::drill_chat::Say(say);
    // The host's own container never lost the record, so its answer to "is it back" is yes.
    UE_LOGI("[CI-CENSUS] before record-back=1 item-client=%d item-host=%d (the host judged slot 1: accepted=%llu refused=%llu)",
            ic, ih, static_cast<unsigned long long>(cen::HostSlices(1, true)),
            static_cast<unsigned long long>(cen::HostSlices(1, false)));
    if (void* s = g_s.Get())
        UE_LOGI("[CI-CENSUS] drill host: the gathered drive is %s on the host", R::IsLive(s) ? "still live" : "gone");
    LogLargest("host");
    cen::ReportNow("host end");
    UE_LOGI("[CI-CENSUS] drill host DONE");
    Next(Step::Done);
}

}  // namespace

void HostTick(coop::net::Session* s) {
    if (g_step == Step::Done) return;
    if (g_step == Step::ViewOpen || g_step == Step::ViewRegen) {
        // The screen steps run each tick: the open verb and the write are separate frames.
    } else if (!Due()) {
        return;
    }
    switch (g_step) {
    case Step::Arm:        StepArm(s); return;
    case Step::Fill:       StepFill(); return;
    case Step::Publish:    StepPublish(); return;
    case Step::WaitR1:     StepWaitR1(); return;
    case Step::WaitJudge:  StepWaitJudge(); return;
    case Step::WaitRoutes: StepWaitRoutes(); return;
    case Step::ViewOpen:   StepViewOpen(); return;
    case Step::ViewRegen:  StepViewRegen(); return;
    case Step::WaitBefore: StepWaitBefore(); return;
    case Step::WaitDone:   StepWaitDone(); return;
    case Step::Done:       return;
    }
}

void HostReset() {
    g_step = Step::Arm;
    g_stepMs = g_next = 0;
    g_pack.Reset();
    g_s.Reset();
    g_eid = static_cast<uint32_t>(coop::element::kInvalidId);
    g_slotsBefore = 0;
    g_gen0 = 0;
    g_slot0 = nullptr;
    g_r1Sent = 0;
    g_r4Prop.Reset();
    g_drives0 = 0;
    g_placeMs = g_r4StartMs = g_ov0 = 0;
    g_r5Placed = g_r5Said = g_r4Started = g_r4Over = g_r4Gone = false;
    g_hi = 0;
    g_r4Eid = 0;
    g_trHost.Reset();
    g_opened = false;
}

}  // namespace coop::dev::container_route_drill::detail
