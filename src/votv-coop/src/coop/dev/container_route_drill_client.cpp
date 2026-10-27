// coop/dev/container_route_drill_client.cpp -- the CLIENT's half of the route census drill; see
// coop/dev/container_route_drill.h.

#include "container_route_drill_detail.h"
#include "drive_drill_verbs.h"  // Hold, Throw: the player's own pickup and throw (co-located private header)

#include "coop/dev/container_route_census.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_hand.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_act_request.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace coop::dev::container_route_drill::detail {
namespace {

namespace E   = ue_wrap::engine;
namespace R   = ue_wrap::reflection;
namespace CV  = ue_wrap::container_view;
namespace DV  = coop::dev::drive_drill_verbs;
namespace cen = coop::dev::container_route_census;

constexpr uint64_t kFindBoundMs   = 60000;   // world-ready to the fixtures' mirrors
constexpr uint64_t kSettleBoundMs = 30000;   // the mirrors' first sight to rest and the host's drive ended
constexpr uint64_t kViewBoundMs   = 5000;    // the open verb to the screen showing the backpack
constexpr uint64_t kSliceBoundMs  = 6000;    // a route's write to the slice it sends (the sweep is 250 ms)
constexpr uint64_t kOverlapBoundMs = 8000;   // the throw to the prop entering the overlap
constexpr uint64_t kHostBoundMs   = 60000;   // the host's raw write, its round trip and the host's chat
constexpr uint64_t kRegenBoundMs  = 3000;    // the publication's arrival to the screen's rebuild, if it comes
constexpr uint64_t kBackBoundMs   = 8000;    // the refused slice to the host's truth coming back
constexpr int      kRestChecks    = 4;
constexpr int      kEmTakeCalls   = 4;       // the first em_take ever only initialises the screen

enum class Step : uint8_t {
    Arm, Find, Settle, Open, OpenWait, R1, R1Wait, R2, R2Call, R2Wait, R3, R3Use, R3Wait, R5, R5Use, R5Wait, R4, R4HoldWait, R4Wait,
    ViewOpen, ViewOpenWait, ViewWait, BeforeReady, BeforeWait, BeforeTake, BeforeSettle, DoneWait, Done
};
Step     g_step = Step::Arm;
uint64_t g_stepMs = 0;
uint64_t g_next = 0;
ue_wrap::CachedObjRef g_pack, g_drive[2], g_vacuum;
uint32_t g_eid = static_cast<uint32_t>(coop::element::kInvalidId);
ue_wrap::FVector g_last[3] = {};
int      g_rest = 0;
uint64_t g_slice0 = 0;
int      g_inv0 = 0;
int      g_calls = 0;
bool     g_noSpace0 = false;
uint64_t g_ov0 = 0;
int32_t  g_count0 = 0, g_countTaken = 0;
int      g_slots0 = 0;
uint64_t g_gen0 = 0;
uint64_t g_changedMs = 0;
int      g_rb = 0, g_ic = 0;

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
    Abandon("client", why);
    g_step = Step::Done;
}
// A step the game would not let the drill make: said, and the drill goes on to the next route.
void Skip(const char* route, const char* why, Step next) {
    UE_LOGW("[CI-CENSUS] drill client: %s not driven -- %s", route, why);
    Next(next);
}
// Says the census after a route's step and goes on.
void Finish(const char* label, Step next) {
    cen::ReportNow(label);
    Next(next);
}
// The write is sent as a slice within the lane's sweep, or the step says it was not.
bool SliceSent(const char* route) {
    if (cen::SlicesSent() > g_slice0) return true;
    if (!Expired(kSliceBoundMs)) return false;
    UE_LOGW("[CI-CENSUS] drill client: %s sent no slice within %llu ms", route, static_cast<unsigned long long>(kSliceBoundMs));
    return true;
}

// The slot press a player's click makes: the hover set on the screen, then the slot's own button.
bool PressSlot(void* ui, int index) {
    void* slot = SlotAt(ui, index);
    void* hover = ui ? R::FindDispatchFunctionCached(R::ClassOf(ui), L"setHoverContainerSlot") : nullptr;
    if (!slot) return false;
    ue_wrap::ParamFrame f(hover);
    if (hover && f.valid() && f.Set<void*>(L"containerSlot", slot)) ue_wrap::Call(ui, f);
    return CallNoArg(slot, L"pressButton");
}

bool ReadBoolField(void* obj, const wchar_t* name, bool& out) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask) || off < 0) return false;
    out = (*(static_cast<const uint8_t*>(obj) + off) & mask) != 0;
    return true;
}

// hold+run with the held prop: playerUsedOn with the player's input_run raised for the call.
bool UseWithRun(void* player, void* pack, void* hold) {
    int32_t off = -1;
    uint8_t mask = 0;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(pack), L"playerUsedOn");
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !R::FindBoolProperty(R::ClassOf(player), L"input_run", off, mask) || off < 0 ||
        !f.Set<void*>(L"player", player) || !f.Set<void*>(L"holdObject", hold))
        return false;
    uint8_t* flag = static_cast<uint8_t*>(player) + off;
    const uint8_t was = *flag;
    *flag = static_cast<uint8_t>(was | mask);
    const bool ok = ue_wrap::Call(pack, f);
    *flag = static_cast<uint8_t>((*flag & ~mask) | (was & mask));
    return ok;
}

void* HeldActor(void* player) {
    E::MainPlayerGrabState hand{};
    return E::ReadMainPlayerGrabState(player, hand) ? hand.holdingActor : nullptr;
}

struct Gather {
    ue_wrap::FVector from;
    void* pack = nullptr;
    float packCm = kNearCm;
    void* drives[2] = {};
    int nDrives = 0;
};

void GatherFixtures(void* player, Gather& g) {
    if (!E::TryGetActorLocation(player, g.from)) return;
    if (void* cls = ue_wrap::object_index::ClassByName(kPackClass))
        ue_wrap::object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
            auto* x = static_cast<Gather*>(c);
            ue_wrap::FVector at{};
            if (!R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__") || !E::TryGetActorLocation(obj, at)) return;
            const float cm = std::hypot(at.X - x->from.X, at.Y - x->from.Y);
            const uint32_t eid = EidOf(obj);
            if (cm < x->packCm && eid != static_cast<uint32_t>(coop::element::kInvalidId) && RecordCount(eid) == kItems) {
                x->pack = obj;
                x->packCm = cm;
            }
        }, &g);
    if (void* cls = ue_wrap::object_index::ClassByName(kDriveClass))
        ue_wrap::object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
            auto* x = static_cast<Gather*>(c);
            ue_wrap::FVector at{};
            if (x->nDrives >= 2 || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__") ||
                !E::TryGetActorLocation(obj, at) || EidOf(obj) == static_cast<uint32_t>(coop::element::kInvalidId))
                return;
            if (std::hypot(at.X - x->from.X, at.Y - x->from.Y) < kNearCm) x->drives[x->nDrives++] = obj;
        }, &g);
}

void StepFind(void* player) {
    Gather g;
    GatherFixtures(player, g);
    if (g.pack && (cen::IsBeforeOnly() || g.nDrives == 2)) {
        g_pack.Set(g.pack);
        g_eid = EidOf(g.pack);
        if (!cen::IsBeforeOnly()) {
            g_drive[0].Set(g.drives[0]);
            g_drive[1].Set(g.drives[1]);
        }
        g_rest = 0;
        UE_LOGI("[CI-CENSUS] drill client: the backpack's mirror holds the host's %d records, eid=%u", kItems, g_eid);
        LogHashes("client", g_eid, "published");
        LogSizes("client", "fixture", g.pack);
        Next(Step::Settle);
        return;
    }
    if (Expired(kFindBoundMs)) Fail("no backpack with the host's records, and two loose drives, stood near this client within 60 s");
}

void StepSettle() {
    void* actors[3] = {g_pack.Get(), g_drive[0].Get(), g_drive[1].Get()};
    const int n = cen::IsBeforeOnly() ? 1 : 3;
    bool still = true, parked = false;
    for (int i = 0; i < n; ++i) {
        ue_wrap::FVector at{};
        if (!actors[i] || !E::TryGetActorLocation(actors[i], at)) { Fail("a fixture's mirror died"); return; }
        still = still && std::fabs(at.X - g_last[i].X) + std::fabs(at.Y - g_last[i].Y) + std::fabs(at.Z - g_last[i].Z) < 1.f;
        g_last[i] = at;
        parked = parked || coop::prop_drive_stream::IsParked(actors[i]);
    }
    g_rest = still ? g_rest + 1 : 0;
    if (g_rest >= kRestChecks && !parked) { Next(Step::Open); return; }
    if (Expired(kSettleBoundMs)) Fail(parked ? "a mirror stayed parked by the host's drive" : "the mirrors never came to rest");
}

// ---- the routes -----------------------------------------------------------------------------------------------

void StepOpen() {
    void* pack = g_pack.Get();
    if (!pack || !CV::Open(pack)) { Fail("the screen could not be opened onto the backpack"); return; }
    Next(Step::OpenWait);
}

void StepOpenWait() {
    if (CV::Viewed() == g_pack.Get() && SlotCount(Screen()) >= 1) {
        UE_LOGI("[CI-CENSUS] drill client: the screen shows the backpack, %d container slot(s)", SlotCount(Screen()));
        Next(cen::IsBeforeOnly() ? Step::BeforeReady : Step::R1);
        return;
    }
    if (Expired(kViewBoundMs)) Fail("the screen never showed the backpack");
}

void StepR1() {
    g_slice0 = cen::SlicesSent();
    g_inv0 = ue_wrap::inventory::LivePersonalStoreCount();
    if (!PressSlot(Screen(), 0)) { Skip("R1", "the slot press did not dispatch", Step::R2); return; }
    UE_LOGI("[CI-CENSUS] drill client: R1 slot pressed (route entries %llu)", static_cast<unsigned long long>(cen::RouteEntries(0)));
    Next(Step::R1Wait);
}

void StepR1Wait() {
    if (SliceSent("R1")) Finish("R1", Step::R2);
}

void StepR2() {
    void* ui = Screen();
    const int inv = ue_wrap::inventory::LivePersonalStoreCount();
    const int32_t off = ui ? R::FindPropertyOffset(R::ClassOf(ui), L"selected") : -1;
    if (inv <= 0 || off < 0) { Skip("R2", "the player's inventory is empty or the screen has no selection", Step::R3); return; }
    const int32_t sel = inv - 1;   // the item R1 took is the last one
    std::memcpy(static_cast<uint8_t*>(ui) + off, &sel, sizeof(sel));
    CallNoArg(ui, L"updSelect");
    g_inv0 = inv;
    g_calls = 0;
    g_slice0 = cen::SlicesSent();
    Next(Step::R2Call);
}

void StepR2Call() {
    if (ue_wrap::inventory::LivePersonalStoreCount() < g_inv0) { Next(Step::R2Wait); return; }
    if (g_calls >= kEmTakeCalls) { Skip("R2", "four em_take calls put nothing back", Step::R3); return; }
    ++g_calls;
    CallNoArg(Screen(), L"em_take");
}

void StepR2Wait() {
    if (!SliceSent("R2")) return;
    UE_LOGI("[CI-CENSUS] drill client: R2 put the selected item back after %d em_take call(s)", g_calls);
    LogSizes("client", "after R2", g_pack.Get());
    Finish("R2", Step::R3);
}

void StepR3() {
    if (!coop::dev::drill_hand::TakeIntoHandNow(L"drive", kDriveClass)) { Skip("R3", "the drive could not be taken into the hand", Step::R5); return; }
    Next(Step::R3Use);   // the hand's actor is read a tick later
}

void StepR3Use(void* player) {
    void* hand = HeldActor(player);
    g_slice0 = cen::SlicesSent();
    if (!hand || !UseWithRun(player, g_pack.Get(), hand)) { Skip("R3", "no hand actor, or playerUsedOn did not dispatch", Step::R5); return; }
    UE_LOGI("[CI-CENSUS] drill client: R3 hold+run used on the backpack (route entries %llu)",
            static_cast<unsigned long long>(cen::RouteEntries(2)));
    Next(Step::R3Wait);
}

void StepR3Wait() {
    if (!SliceSent("R3")) return;
    LogSizes("client", "after R3", g_pack.Get());
    coop::dev::drill_hand::TakeIntoHandNow(L"None", nullptr);
    Finish("R3", Step::R5);
}

void StepR5() {
    if (!coop::dev::drill_hand::TakeIntoHandNow(L"vacuumC", L"prop_vacuum2_C")) { Skip("R5", "the vacuum could not be taken into the hand", Step::R4); return; }
    Next(Step::R5Use);   // the hand's actor is read a tick later
}

void StepR5Use(void* player) {
    void* vac = HeldActor(player);
    if (!vac || R::ClassNameOf(vac) != L"prop_vacuum2_C") { Skip("R5", "the hand holds no vacuum", Step::R4); return; }
    g_vacuum.Set(vac);
    // The side read: is the vacuum in hand an element, and of what type?
    const uint32_t eid = EidOf(vac);
    if (eid == static_cast<uint32_t>(coop::element::kInvalidId))
        UE_LOGI("[CI-CENSUS] client vacuum in hand: no element id");
    else if (auto* el = coop::element::Registry::Get().Get(eid))
        UE_LOGI("[CI-CENSUS] client vacuum in hand: element id %u, type %d", eid, static_cast<int>(el->GetType()));
    // The loose drive is brought to the vacuum's intake, so the gather has a prop to take.
    void* drive = g_drive[0].Get();
    const int32_t off = R::FindPropertyOffset(R::ClassOf(vac), L"suckin_inv");
    void* suck = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(vac) + off) : nullptr;
    ue_wrap::FVector at{};
    if (!drive || !suck || !E::TryGetComponentLocation(suck, at) || !E::SetActorLocation(drive, at)) {
        Skip("R5", "the vacuum's intake or the loose drive is unread", Step::R4);
        return;
    }
    ReadBoolField(vac, L"noSpace", g_noSpace0);
    UE_LOGI("[CI-CENSUS] drill client: R5 the loose drive moved to the intake (%.0f,%.0f,%.0f), gather called", at.X, at.Y, at.Z);
    if (!CallNoArg(vac, L"gather")) { Skip("R5", "gather did not dispatch", Step::R4); return; }
    Next(Step::R5Wait);
}

void StepR5Wait() {
    void* vac = g_vacuum.Get();
    void* drive = g_drive[0].Get();
    bool noSpace = false;
    ReadBoolField(vac, L"noSpace", noSpace);
    ue_wrap::FVector at{};
    const bool live = drive && R::IsLive(drive);
    if (live) E::TryGetActorLocation(drive, at);
    UE_LOGI("[CI-CENSUS] R5 result: noSpace %d -> %d, the gathered drive %s at (%.0f,%.0f,%.0f), route entries %llu, "
            "addObject cancelled %llu", g_noSpace0 ? 1 : 0, noSpace ? 1 : 0, live ? "still live" : "gone", at.X, at.Y, at.Z,
            static_cast<unsigned long long>(cen::RouteEntries(4)), static_cast<unsigned long long>(cen::VacuumCancelled()));
    LogSizes("client", "the vacuum after R5", vac);
    coop::dev::drill_hand::TakeIntoHandNow(L"None", nullptr);
    Finish("R5", Step::R4);
}

void ThrowTheHeld(void* player) {
    g_ov0 = cen::R4Overlaps();
    if (!DV::Throw(player)) { Skip("R4", "the hand's drive could not be thrown", Step::ViewOpen); return; }
    UE_LOGI("[CI-CENSUS] drill client: R4 the shared drive taken into the hand and thrown at the backpack");
    Next(Step::R4Wait);
}

void StepR4(void* player) {
    void* drive = g_drive[1].Get();
    if (!drive) { Skip("R4", "the loose drive's mirror is gone", Step::ViewOpen); return; }
    if (DV::Hold(player, drive)) { ThrowTheHeld(player); return; }
    // A mirror's hold is asked of the host and ends a round trip after the call.
    if (coop::prop_act_request::dev::PendingId() == 0) { Skip("R4", "the drive could not be taken into the hand", Step::ViewOpen); return; }
    Next(Step::R4HoldWait);
}

void StepR4HoldWait(void* player) {
    if (coop::prop_act_request::dev::PendingId() != 0) return;
    void* hand = HeldActor(player);
    if (!hand || R::ClassNameOf(hand) != kDriveClass) { Skip("R4", "the drive could not be taken into the hand", Step::ViewOpen); return; }
    ThrowTheHeld(player);
}

void StepR4Wait() {
    if (cen::R4Overlaps() <= g_ov0 && !Expired(kOverlapBoundMs)) return;
    UE_LOGI("[CI-CENSUS] drill client: R4 the thrown drive %s the backpack's overlap (%llu entr%s)",
            cen::R4Overlaps() > g_ov0 ? "entered" : "did NOT enter",
            static_cast<unsigned long long>(cen::R4Overlaps() - g_ov0), cen::R4Overlaps() - g_ov0 == 1 ? "y" : "ies");
    coop::dev::drill_chat::Say(kLineR4Done);
    Finish("R4", Step::ViewOpen);
}

// ---- the open screen, then the take the host refuses ----------------------------------------------------------

void StepViewOpen() {
    void* pack = g_pack.Get();
    if (CV::Viewed() != pack && !CV::Open(pack)) { Fail("the screen could not be opened onto the backpack again"); return; }
    Next(Step::ViewOpenWait);
}

void StepViewOpenWait() {
    if (CV::Viewed() != g_pack.Get()) {
        if (Expired(kViewBoundMs)) Fail("the screen never showed the backpack again");
        return;
    }
    g_slots0 = SlotCount(Screen());
    g_gen0 = cen::GenCalls();
    g_count0 = RecordCount(g_eid);
    g_changedMs = 0;
    LogSizes("client", "after the routes", g_pack.Get());
    cen::ReportNow("routes done");
    coop::dev::drill_chat::Say(kLineRoutesDone);
    Next(Step::ViewWait);
}

// The host writes the backpack's contents once and publishes it: this peer's open screen is judged on whether the
// applied publication rebuilt it.
void StepViewWait() {
    if (RecordCount(g_eid) == g_count0) {
        if (Expired(kHostBoundMs)) Skip("the open screen", "the host's publication never arrived", Step::BeforeReady);
        return;
    }
    if (!g_changedMs) g_changedMs = ::GetTickCount64();
    const bool regen = cen::GenCalls() > g_gen0;
    if (!regen && ::GetTickCount64() - g_changedMs < kRegenBoundMs) return;
    UE_LOGI("[CI-CENSUS] client view regen=%d slots %d -> %d", regen ? 1 : 0, g_slots0, SlotCount(Screen()));
    LogHashes("client", g_eid, "after the host's raw write");
    LogSizes("client", "after the host's raw write", g_pack.Get());
    Finish("view", Step::BeforeReady);
}

void StepBeforeReady() {
    void* pack = g_pack.Get();
    if (CV::Viewed() != pack) {
        if (!CV::Open(pack) || Expired(kHostBoundMs)) Fail("the screen could not be opened for the take");
        return;
    }
    coop::dev::drill_chat::Say(kLineBeforeReady);
    Next(Step::BeforeWait);
}

void StepBeforeWait() {
    if (!Said(kLineBeforeWritten)) {
        if (Expired(kHostBoundMs)) Fail("the host never wrote the unpublished change");
        return;
    }
    g_count0 = RecordCount(g_eid);
    g_inv0 = ue_wrap::inventory::LivePersonalStoreCount();
    g_slice0 = cen::SlicesSent();
    if (!PressSlot(Screen(), 0)) { Fail("the take's slot press did not dispatch"); return; }
    UE_LOGI("[CI-CENSUS] drill client: the take pressed on a base the host has moved past (%d record(s) here)", g_count0);
    Next(Step::BeforeTake);
}

void StepBeforeTake() {
    if (!SliceSent("the take")) return;
    g_countTaken = RecordCount(g_eid);
    Next(Step::BeforeSettle);
}

// The host refuses the stale slice and answers with its truth: the record is back when the count rises again.
void StepBeforeSettle() {
    const int32_t now = RecordCount(g_eid);
    if (now <= g_countTaken && !Expired(kBackBoundMs)) return;
    g_rb = now > g_countTaken ? 1 : 0;
    g_ic = ue_wrap::inventory::LivePersonalStoreCount() > g_inv0 ? 1 : 0;
    UE_LOGI("[CI-CENSUS] drill client: the take left %d record(s), the host's truth brought it to %d", g_countTaken, now);
    char say[48];
    std::snprintf(say, sizeof(say), "%srb=%d ic=%d", kPrefixClientDone, g_rb, g_ic);
    coop::dev::drill_chat::Say(say);
    Next(Step::DoneWait);
}

void StepDoneWait() {
    std::string rest;
    int ih = 0;
    if (!coop::dev::drill_chat::PollDue() || !coop::dev::drill_chat::LiveRestOf(kPrefixHostDone, &rest)) {
        if (Expired(kHostBoundMs)) Fail("the host never answered the take");
        return;
    }
    if (!IntAfter(rest, "ih", ih)) return;
    UE_LOGI("[CI-CENSUS] before record-back=%d item-client=%d item-host=%d", g_rb, g_ic, ih);
    LogLargest("client");
    cen::ReportNow("client end");
    UE_LOGI("[CI-CENSUS] drill client DONE");
    Next(Step::Done);
}

}  // namespace

void ClientTick(coop::net::Session*) {
    if (g_step == Step::Done || !Due()) return;
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    switch (g_step) {
    case Step::Arm:
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        Next(Step::Find);
        return;
    case Step::Find:         StepFind(player); return;
    case Step::Settle:       StepSettle(); return;
    case Step::Open:         StepOpen(); return;
    case Step::OpenWait:     StepOpenWait(); return;
    case Step::R1:           StepR1(); return;
    case Step::R1Wait:       StepR1Wait(); return;
    case Step::R2:           StepR2(); return;
    case Step::R2Call:       StepR2Call(); return;
    case Step::R2Wait:       StepR2Wait(); return;
    case Step::R3:           StepR3(); return;
    case Step::R3Use:        StepR3Use(player); return;
    case Step::R3Wait:       StepR3Wait(); return;
    case Step::R5:           StepR5(); return;
    case Step::R5Use:        StepR5Use(player); return;
    case Step::R5Wait:       StepR5Wait(); return;
    case Step::R4:           StepR4(player); return;
    case Step::R4HoldWait:   StepR4HoldWait(player); return;
    case Step::R4Wait:       StepR4Wait(); return;
    case Step::ViewOpen:     StepViewOpen(); return;
    case Step::ViewOpenWait: StepViewOpenWait(); return;
    case Step::ViewWait:     StepViewWait(); return;
    case Step::BeforeReady:  StepBeforeReady(); return;
    case Step::BeforeWait:   StepBeforeWait(); return;
    case Step::BeforeTake:   StepBeforeTake(); return;
    case Step::BeforeSettle: StepBeforeSettle(); return;
    case Step::DoneWait:     StepDoneWait(); return;
    case Step::Done:         return;
    }
}

void ClientReset() {
    g_step = Step::Arm;
    g_stepMs = g_next = 0;
    g_pack.Reset();
    g_drive[0].Reset();
    g_drive[1].Reset();
    g_vacuum.Reset();
    g_eid = static_cast<uint32_t>(coop::element::kInvalidId);
    for (ue_wrap::FVector& v : g_last) v = {};
    g_rest = g_calls = g_inv0 = g_slots0 = g_rb = g_ic = 0;
    g_slice0 = g_ov0 = g_gen0 = g_changedMs = 0;
    g_noSpace0 = false;
    g_count0 = g_countTaken = 0;
}

}  // namespace coop::dev::container_route_drill::detail
