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
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
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
constexpr uint64_t kRegenBoundMs   = 3000;   // the raw write to the screen's rebuild, if it comes
constexpr int      kTraceLines     = 400;

enum class Step : uint8_t { Arm, Fill, Publish, WaitRoutes, ViewOpen, ViewRegen, WaitBefore, WaitDone, Done };
Step     g_step = Step::Arm;
uint64_t g_stepMs = 0;
uint64_t g_next = 0;
ue_wrap::CachedObjRef g_pack, g_s;
uint32_t g_eid = static_cast<uint32_t>(coop::element::kInvalidId);
std::vector<ue_wrap::CachedObjRef> g_drivesBefore;   // the drives standing when the fixture was ready
int      g_slotsBefore = 0;
uint64_t g_gen0 = 0;
int      g_drives0 = 0;
int      g_traceLines = 0;
bool     g_r4Over = false;
bool     g_opened = false;   // the open verb was sent for the host's screen

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

bool WasThere(void* obj) {
    for (const ue_wrap::CachedObjRef& b : g_drivesBefore)
        if (b.Is(obj)) return true;
    return false;
}

// The R4 step, from the host's side: the position of each drive born since the fixture was ready (the thrown
// prop's copy), every tick, with the clock the client's line carries. The analysis reads the lag between them.
void TraceR4() {
    if (g_r4Over || g_traceLines >= kTraceLines) return;
    void* cls = ue_wrap::object_index::ClassByName(kDriveClass);
    if (!cls) return;
    ue_wrap::object_index::ForEachInstance(cls, [](void*, void* obj, int32_t) {
        ue_wrap::FVector at{};
        if (!R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__") || WasThere(obj) ||
            !E::TryGetActorLocation(obj, at) || g_traceLines >= kTraceLines)
            return;
        ++g_traceLines;
        UE_LOGI("[CI-CENSUS] r4 host prop=(%.1f,%.1f,%.1f) t=%llu eid=%u", at.X, at.Y, at.Z,
                static_cast<unsigned long long>(cen::UnixMs()), EidOf(obj));
    }, nullptr);
}

void StepArm(coop::net::Session* s) {
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
        // Two loose drives to either side: one the client's vacuum gathers, one it throws into the backpack.
        void* driveCls = R::FindClass(kDriveClass);
        void* a = driveCls ? E::SpawnActor(driveCls, Offset(at, fwd, kSideAheadCm, kSideCm)) : nullptr;
        void* b = driveCls ? E::SpawnActor(driveCls, Offset(at, fwd, kSideAheadCm, -kSideCm)) : nullptr;
        if (!a || !b) { Fail("the loose drives did not spawn"); return; }
        g_s.Set(a);
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
    void* itemCls = R::FindClass(kDriveClass);
    ue_wrap::FVector put{};
    if (!itemCls || !E::TryGetActorLocation(pack, put)) { Fail("the drive's class or the backpack's place is unread"); return; }
    cen::Quiet quiet;   // the fixture's own fill is no route
    int added = 0;
    for (int i = 0; i < kItems; ++i) {
        void* item = E::SpawnActor(itemCls, {put.X, put.Y, put.Z + 60.f + 30.f * i});
        if (item && PutIn(pack, item)) {
            ++added;
            E::DestroyActor(item);   // as every BP caller does after a put that took
        }
    }
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
    g_drivesBefore.clear();
    void* cls = ue_wrap::object_index::ClassByName(kDriveClass);
    if (cls)
        ue_wrap::object_index::ForEachInstance(cls, [](void*, void* obj, int32_t) {
            g_drivesBefore.emplace_back();
            g_drivesBefore.back().Set(obj);
        }, nullptr);
    UE_LOGI("[CI-CENSUS] drill host: a backpack holding %d drives before the client's puppet, eid=%u", kItems, g_eid);
    LogHashes("host", g_eid, "published");
    LogSizes("host", "fixture", pack);
    cen::ReportNow("host fixture");
    Next(cen::IsBeforeOnly() ? Step::WaitBefore : Step::WaitRoutes);
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
        if (!cs::DevRawAppend(g_eid, /*publish=*/true)) { Fail("the raw write to the backpack did not apply"); return; }
        UE_LOGI("[CI-CENSUS] drill host: one raw write applied to eid=%u (%d record(s) now), published", g_eid,
                RecordCount(g_eid));
        Next(Step::ViewRegen);
        return;
    }
    if (Expired(kViewBoundMs)) Fail("the screen never showed the backpack");
}

void StepViewRegen() {
    const bool regen = cen::GenCalls() > g_gen0;
    if (!regen && !Expired(kRegenBoundMs)) return;
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
    // The thrown prop's host copy is traced every tick, whatever the step; the rest is paced.
    if (g_step >= Step::WaitRoutes && g_step <= Step::WaitBefore) {
        if (!g_r4Over && coop::dev::drill_chat::PollDue() && coop::dev::drill_chat::ChatHasLive(kLineR4Done)) g_r4Over = true;
        TraceR4();
    }
    if (g_step == Step::ViewOpen || g_step == Step::ViewRegen) {
        // The screen steps run each tick: the open verb and the write are separate frames.
    } else if (!Due()) {
        return;
    }
    switch (g_step) {
    case Step::Arm:        StepArm(s); return;
    case Step::Fill:       StepFill(); return;
    case Step::Publish:    StepPublish(); return;
    case Step::WaitRoutes:
        if (Said(kLineRoutesDone)) Next(Step::ViewOpen);
        return;
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
    g_drivesBefore.clear();
    g_slotsBefore = 0;
    g_gen0 = 0;
    g_drives0 = 0;
    g_traceLines = 0;
    g_r4Over = false;
    g_opened = false;
}

}  // namespace coop::dev::container_route_drill::detail
