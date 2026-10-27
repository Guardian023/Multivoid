// coop/dev/container_route_drill_client.cpp -- the CLIENT's half of the route census drill; see
// coop/dev/container_route_drill.h.

#include "container_route_drill_detail.h"
#include "drive_drill_verbs.h"  // Hold, Throw: the player's own pickup and throw (co-located private header)

#include "coop/dev/container_route_census.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/director/aim_fan.h"
#include "coop/dev/drill_hand.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_act_request.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/session/join_progress.h"
#include "coop/props/container_contents_sync.h"  // DevRecordHashes
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
#include "ue_wrap/engine/engine_attach.h"   // IsActorRootSimulatingPhysics
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::container_route_drill::detail {
namespace {

namespace E   = ue_wrap::engine;
namespace R   = ue_wrap::reflection;
namespace CV  = ue_wrap::container_view;
namespace DV  = coop::dev::drive_drill_verbs;
namespace cen = coop::dev::container_route_census;
namespace cs  = coop::props::container_contents_sync;

constexpr uint64_t kFindBoundMs   = 60000;   // world-ready to the fixtures' mirrors
constexpr uint64_t kSettleBoundMs = 30000;   // the mirrors' first sight to rest and the host's drive ended
constexpr uint64_t kViewBoundMs   = 5000;    // the open verb to the screen showing the backpack
constexpr uint64_t kSliceBoundMs  = 6000;    // a route's write to the slice it sends (the sweep is 250 ms)
constexpr uint64_t kOverlapBoundMs = 8000;   // the throw to the prop entering the overlap
constexpr uint64_t kHostBoundMs   = 60000;   // the host's raw write, its round trip and the host's chat
constexpr uint64_t kBackBoundMs   = 8000;    // the refused slice to the host's truth coming back
constexpr int      kRestChecks    = 4;
constexpr int      kEmTakeCalls   = 4;       // the first em_take ever only initialises the screen

enum class Step : uint8_t {
    Arm, Find, Settle, Open, OpenWait, R1, R1Wait, R1Done, R2Refill, R2, R2Call, R2Wait, R3, R3Use, R3Wait,
    R5, R5Use, R5Settle, R5Placed, R5Wait,
    R4, R4HoldWait, R4Settle, R4Aim, R4Wait,
    ViewOpen, ViewOpenWait, ViewWait, BeforeReady, BeforeWait, BeforeTake, BeforeSettle, DoneWait, Done
};
Step     g_step = Step::Arm;
uint64_t g_stepMs = 0;
uint64_t g_next = 0;
ue_wrap::CachedObjRef g_pack, g_drive[2], g_vacuum, g_thrown;   // g_drive[0] the drive the host placed at the intake, [1] R4's
uint32_t g_eid = static_cast<uint32_t>(coop::element::kInvalidId);
ue_wrap::FVector g_last[2] = {};
int      g_settled = 0;          // consecutive checks that held the step's condition
float    g_radiusCm = 0.f;       // the vacuum's intake sphere
uint32_t g_placedEid = 0;        // the host's drive at the intake (0 until the host says)
uint32_t g_thrownEid = 0;        // the held drive's element id (0 if it has none)
bool     g_thrownLive = false;   // the client's trace of the held actor still runs
uint32_t g_mirrorEid = 0;        // the host's drive for the throw (0 until the host says)
coop::director::AimFan g_fan;
TraceRate g_trLocal, g_trMirror;
int      g_rest = 0;
uint64_t g_slice0 = 0;
int      g_inv0 = 0;
int      g_calls = 0;
bool     g_noSpace0 = false;
uint64_t g_ov0 = 0;
int32_t  g_count0 = 0, g_countTaken = 0;
int      g_slots0 = 0;
void*    g_slot0 = nullptr;      // the screen's first slot widget at the view-regen baseline
uint64_t g_gen0 = 0;
std::string g_slotText0;         // the screen's first slot at the view-regen baseline
uint64_t g_changedMs = 0;
int      g_rb = 0, g_ic = 0;
bool     g_refilled = false;     // the host said it handed the backpack its fresh records
float    g_intakeCm = 0.f;       // the gathered drive's distance from the vacuum's intake
uint64_t g_r2Add0 = 0, g_r1Add0 = 0;   // the add verbs counted under R2 / R1 before the R2 calls
std::vector<uint64_t> g_hashes;  // the backpack's record hashes at the last baseline (R1, then R2)
uint64_t g_r1Took = 0;           // the record R1's press took, by hash

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
// A bound that passed, or a step the game would not let the drill make: the FAIL line, and the drill ends.
void FailAt(const char* phase) {
    FailPhase("client", phase);
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

// The screen's hovered container slot cleared the way the game's own unhover does (setHoverContainerSlot with no
// object), and read back: em_take presses the hovered slot first, so a stale hover makes the take key an R1.
bool ClearHover(void* ui) {
    void* fn = ui ? R::FindDispatchFunctionCached(R::ClassOf(ui), L"setHoverContainerSlot") : nullptr;
    ue_wrap::ParamFrame f(fn);
    const int32_t off = ui ? R::FindPropertyOffset(R::ClassOf(ui), L"hoverContainerSlot") : -1;
    if (!fn || !f.valid() || off < 0 || !f.Set<void*>(L"containerSlot", nullptr) || !ue_wrap::Call(ui, f)) return false;
    return *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(ui) + off) == nullptr;
}

// The first hash of `a` that `b` does not hold (a record `b` lacks), or 0 when `b` holds them all.
uint64_t FirstMissing(const std::vector<uint64_t>& a, std::vector<uint64_t> b) {
    for (uint64_t h : a) {
        bool found = false;
        for (size_t i = 0; i < b.size() && !found; ++i)
            if (b[i] == h) { b.erase(b.begin() + static_cast<std::ptrdiff_t>(i)); found = true; }
        if (!found) return h;
    }
    return 0;
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
    if (g.pack && (cen::IsBeforeOnly() || g.nDrives == 1)) {
        g_pack.Set(g.pack);
        g_eid = EidOf(g.pack);
        if (!cen::IsBeforeOnly()) g_drive[1].Set(g.drives[0]);   // R4's; the vacuum's is placed once its intake settles
        g_rest = 0;
        UE_LOGI("[CI-CENSUS] drill client: the backpack's mirror holds the host's %d records, eid=%u", kItems, g_eid);
        LogHashes("client", g_eid, "published");
        LogSizes("client", "fixture", g.pack);
        Next(Step::Settle);
        return;
    }
    if (Expired(kFindBoundMs)) Fail("no backpack with the host's records, and one loose drive, stood near this client within 60 s");
}

void StepSettle() {
    void* actors[2] = {g_pack.Get(), g_drive[1].Get()};
    const int n = cen::IsBeforeOnly() ? 1 : 2;
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
    uint64_t whole = 0;
    cs::DevRecordHashes(g_eid, g_hashes, whole);
    if (!PressSlot(Screen(), 0)) { Skip("R1", "the slot press did not dispatch", Step::R1Done); return; }
    UE_LOGI("[CI-CENSUS] drill client: R1 slot pressed (route entries %llu)", static_cast<unsigned long long>(cen::RouteEntries(0)));
    Next(Step::R1Wait);
}

void StepR1Wait() {
    if (SliceSent("R1")) Finish("R1", Step::R1Done);
}

// R1 is over: the record its press took is named by what the backpack lost, and the host is told how many slices this
// peer has sent so it hands the backpack two fresh records only after it has judged them.
void StepR1Done() {
    std::vector<uint64_t> after;
    uint64_t whole = 0;
    cs::DevRecordHashes(g_eid, after, whole);
    g_r1Took = FirstMissing(g_hashes, after);
    g_count0 = RecordCount(g_eid);
    UE_LOGI("[CI-CENSUS] drill client: R1 took record hash=%016llx, the backpack holds %d record(s)",
            static_cast<unsigned long long>(g_r1Took), g_count0);
    char say[64];
    std::snprintf(say, sizeof(say), "%ssent=%llu", kPrefixR1Done, static_cast<unsigned long long>(cen::SlicesSent()));
    coop::dev::drill_chat::Say(say);
    Next(Step::R2Refill);
}

// The host hands the backpack two fresh records once it has judged R1's slice; the put-back goes on the base that
// carries them, never on a stale one.
void StepR2Refill() {
    if (!g_refilled) {
        if (!Said(kLineRefilled)) {
            if (Expired(kHostBoundMs)) Fail("the host never handed the backpack its fresh records");
            return;
        }
        g_refilled = true;
    }
    if (RecordCount(g_eid) < g_count0 + kItems) {
        if (Expired(kHostBoundMs)) Fail("the fresh records never reached this peer's backpack");
        return;
    }
    UE_LOGI("[CI-CENSUS] drill client: the backpack holds %d record(s) with the host's fresh ones", RecordCount(g_eid));
    Next(Step::R2);
}

void StepR2() {
    void* ui = Screen();
    const int inv = ue_wrap::inventory::LivePersonalStoreCount();
    const int32_t off = ui ? R::FindPropertyOffset(R::ClassOf(ui), L"selected") : -1;
    if (inv <= 0 || off < 0) { Skip("R2", "the player's inventory is empty or the screen has no selection", Step::R3); return; }
    if (!ClearHover(ui)) { Skip("R2", "the screen's hovered container slot would not clear", Step::R3); return; }
    const int32_t sel = inv - 1;   // the item R1 took is the last one
    std::memcpy(static_cast<uint8_t*>(ui) + off, &sel, sizeof(sel));
    CallNoArg(ui, L"updSelect");
    g_inv0 = inv;
    g_calls = 0;
    g_slice0 = cen::SlicesSent();
    g_r2Add0 = cen::RouteVerbs(1, false);
    g_r1Add0 = cen::RouteVerbs(0, false);
    uint64_t whole = 0;
    cs::DevRecordHashes(g_eid, g_hashes, whole);
    UE_LOGI("[CI-CENSUS] drill client: R2 the hovered slot cleared, item %d selected of %d, the backpack holds %zu record(s)",
            sel, inv, g_hashes.size());
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
    std::vector<uint64_t> after;
    uint64_t whole = 0;
    cs::DevRecordHashes(g_eid, after, whole);
    const uint64_t put = FirstMissing(after, g_hashes);
    const uint64_t r2Adds = cen::RouteVerbs(1, false) - g_r2Add0;
    UE_LOGI("[CI-CENSUS] drill client: R2 put the selected item back after %d em_take call(s): record hash=%016llx "
            "(R1 took %016llx), the backpack %zu -> %zu record(s), add verbs counted R2 +%llu, R1 +%llu",
            g_calls, static_cast<unsigned long long>(put), static_cast<unsigned long long>(g_r1Took), g_hashes.size(),
            after.size(), static_cast<unsigned long long>(r2Adds),
            static_cast<unsigned long long>(cen::RouteVerbs(0, false) - g_r1Add0));
    if (!r2Adds) UE_LOGW("[CI-CENSUS] drill client: R2 put a record and the census counted no add under R2");
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
    if (!coop::dev::drill_hand::TakeIntoHandNow(L"vacuumC", L"prop_vacuum2_C")) { FailAt("R5 the vacuum could not be taken into the hand"); return; }
    Next(Step::R5Use);   // the hand's actor is read a tick later
}

void* SuckOf(void* vac) {
    const int32_t off = vac ? R::FindPropertyOffset(R::ClassOf(vac), L"suckin_inv") : -1;
    return off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(vac) + off) : nullptr;
}

// The intake sphere's radius in cm, or -1 when it does not read.
float RadiusOf(void* sphere) {
    const int32_t off = sphere && R::IsLive(sphere) ? R::FindPropertyOffset(R::ClassOf(sphere), L"SphereRadius") : -1;
    return off < 0 ? -1.f : *reinterpret_cast<const float*>(static_cast<const uint8_t*>(sphere) + off);
}

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    return std::sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z));
}

void StepR5Use(void* player) {
    void* vac = HeldActor(player);
    if (!vac || R::ClassNameOf(vac) != L"prop_vacuum2_C") { FailAt("R5 the hand holds no vacuum"); return; }
    g_vacuum.Set(vac);
    // The side read: is the vacuum in hand an element, and of what type?
    const uint32_t eid = EidOf(vac);
    if (eid == static_cast<uint32_t>(coop::element::kInvalidId))
        UE_LOGI("[CI-CENSUS] client vacuum in hand: no element id");
    else if (auto* el = coop::element::Registry::Get().Get(eid))
        UE_LOGI("[CI-CENSUS] client vacuum in hand: element id %u, type %d", eid, static_cast<int>(el->GetType()));
    g_settled = 0;
    Next(Step::R5Settle);
}

// The vacuum's intake sphere follows the hand once the hold settles: until it sits within reach of the player, its
// location is not where a gather would look. Settled on two consecutive checks; then the host is told where it is.
void StepR5Settle(void* player) {
    void* vac = g_vacuum.Get();
    void* suck = SuckOf(vac);
    ue_wrap::FVector at{}, me{};
    const bool read = suck && E::TryGetComponentLocation(suck, at) && E::TryGetActorLocation(player, me);
    g_settled = read && Dist(at, me) <= 300.f ? g_settled + 1 : 0;
    if (g_settled < 2) {
        if (Expired(kHandBoundMs)) FailAt("R5 the vacuum's intake never reached the hand");
        return;
    }
    g_radiusCm = RadiusOf(suck);
    if (g_radiusCm <= 0.f) { FailAt("R5 the vacuum's intake radius is unread"); return; }
    UE_LOGI("[CI-CENSUS] drill client: R5 intake settled at (%.0f,%.0f,%.0f), radius %.0f cm", at.X, at.Y, at.Z, g_radiusCm);
    char say[96];
    std::snprintf(say, sizeof(say), "%sx=%ld y=%ld z=%ld", kPrefixR5Intake, std::lround(at.X), std::lround(at.Y), std::lround(at.Z));
    coop::dev::drill_chat::Say(say);
    g_placedEid = 0;
    Next(Step::R5Placed);
}

// The host spawns the drive AT the settled intake and says its id; the gather is called once that drive's mirror lies
// within the intake's radius.
void StepR5Placed() {
    void* vac = g_vacuum.Get();
    void* suck = SuckOf(vac);
    ue_wrap::FVector at{}, d{};
    if (!g_placedEid) {
        std::string rest;
        int eid = 0;
        if (coop::dev::drill_chat::LiveRestOf(kPrefixR5Placed, &rest) && IntAfter(rest, "eid", eid) && eid > 0)
            g_placedEid = static_cast<uint32_t>(eid);
    }
    void* mirror = g_placedEid ? coop::element::LivePropActor(g_placedEid) : nullptr;
    if (mirror && suck && E::TryGetComponentLocation(suck, at) && E::TryGetActorLocation(mirror, d)) {
        const float cm = Dist(d, at);
        if (cm <= g_radiusCm) {
            g_drive[0].Set(mirror);
            g_intakeCm = cm;
            ReadBoolField(vac, L"noSpace", g_noSpace0);
            UE_LOGI("[CI-CENSUS] drill client: R5 the placed drive (eid=%u) is %.0f cm from the intake (%.0f,%.0f,%.0f), "
                    "radius %.0f cm, gather called", g_placedEid, cm, at.X, at.Y, at.Z, g_radiusCm);
            bool called = false;
            {
                cen::GatherScope own;   // the one place drill-vacuumcancel cancels the client's addObject
                called = CallNoArg(vac, L"gather");
            }
            if (!called) { FailAt("R5 gather did not dispatch"); return; }
            Next(Step::R5Wait);
            return;
        }
    }
    if (Expired(kHostBoundMs)) {
        UE_LOGW("[CI-CENSUS] drill client: R5 placed eid=%u, mirror %s", g_placedEid, mirror ? "arrived" : "not seen");
        FailAt("R5 the placed drive never came within the intake's radius");
    }
}

void StepR5Wait() {
    void* vac = g_vacuum.Get();
    void* drive = g_drive[0].Get();
    bool noSpace = false;
    ReadBoolField(vac, L"noSpace", noSpace);
    ue_wrap::FVector at{};
    const bool live = drive && R::IsLive(drive);
    if (live) E::TryGetActorLocation(drive, at);
    UE_LOGI("[CI-CENSUS] R5 result: noSpace %d -> %d, the gathered drive (%.0f cm from the intake, radius %.0f cm) %s at (%.0f,%.0f,%.0f), "
            "route entries %llu, add verbs counted R5 %llu, R4 overlaps %llu, addObject cancelled %llu", g_noSpace0 ? 1 : 0,
            noSpace ? 1 : 0, g_intakeCm, g_radiusCm, live ? "still live" : "gone", at.X, at.Y, at.Z,
            static_cast<unsigned long long>(cen::RouteEntries(4)), static_cast<unsigned long long>(cen::RouteVerbs(4, false)),
            static_cast<unsigned long long>(cen::R4Overlaps()), static_cast<unsigned long long>(cen::VacuumCancelled()));
    LogSizes("client", "the vacuum after R5", vac);
    coop::dev::drill_hand::TakeIntoHandNow(L"None", nullptr);
    Finish("R5", Step::R4);
}

// The aimed throw: the hand's drive is named to the host, then thrown by the game's own throw at the human seam.
void ThrowTheHeld(void* player) {
    void* held = g_thrown.Get();
    g_thrownEid = held && EidOf(held) != static_cast<uint32_t>(coop::element::kInvalidId) ? EidOf(held) : 0;
    const bool sim = held && E::IsActorRootSimulatingPhysics(held);
    char say[48];
    std::snprintf(say, sizeof(say), "%seid=%u", kPrefixR4Start, g_thrownEid);
    coop::dev::drill_chat::Say(say);
    UE_LOGI("[CI-CENSUS] drill client: R4 the held drive eid=%u sim=%d, thrown at the backpack", g_thrownEid, sim ? 1 : 0);
    g_ov0 = cen::R4Overlaps();
    g_thrownLive = held != nullptr;
    g_mirrorEid = 0;
    g_trLocal.Reset();
    g_trMirror.Reset();
    if (!DV::Throw(player)) { FailAt("R4 the hand's drive could not be thrown"); return; }
    Next(Step::R4Wait);
}

void StepR4(void* player) {
    void* drive = g_drive[1].Get();
    if (!drive) { FailAt("R4 the loose drive's mirror is gone"); return; }
    g_settled = 0;
    if (DV::Hold(player, drive)) { Next(Step::R4Settle); return; }
    // A mirror's hold is asked of the host and ends a round trip after the call.
    if (coop::prop_act_request::dev::PendingId() == 0) { FailAt("R4 the drive could not be taken into the hand"); return; }
    Next(Step::R4HoldWait);
}

void StepR4HoldWait() {
    if (coop::prop_act_request::dev::PendingId() == 0) Next(Step::R4Settle);
}

// The hold is settled once the hand holds a drive on two consecutive checks.
void StepR4Settle(void* player) {
    void* hand = HeldActor(player);
    g_settled = hand && R::ClassNameOf(hand) == kDriveClass ? g_settled + 1 : 0;
    if (g_settled >= 2) {
        g_thrown.Set(hand);
        g_fan = coop::director::AimFan();
        Next(Step::R4Aim);
        return;
    }
    if (Expired(kHandBoundMs)) FailAt("R4 the hold never settled");
}

// The camera turned through the fan round the backpack until the player's own interaction trace strikes it (the read
// of director/aimed_grab.cpp's Tick: ReadMainPlayerHitActor against the prop).
void StepR4Aim(void* player) {
    void* pack = g_pack.Get();
    ue_wrap::FVector at{};
    if (!pack || !E::TryGetActorLocation(pack, at)) { FailAt("R4 the backpack's mirror died"); return; }
    switch (g_fan.Tick(player, at, E::ReadMainPlayerHitActor(player) == pack)) {
        case coop::director::AimFan::State::Working: return;
        case coop::director::AimFan::State::Failed:  FailAt("R4 the aim never struck the backpack"); return;
        case coop::director::AimFan::State::Aimed:   ThrowTheHeld(player); return;
    }
}

// From the throw to r4-done, one read of each cached ref per check: the actor the client threw, then the host's copy
// of the drive its drop path made.
void TraceR4() {
    if (g_thrownLive) {
        void* held = g_thrown.Get();
        ue_wrap::FVector at{};
        const bool live = held && E::TryGetActorLocation(held, at);
        if (!live || g_trLocal.Take())   // the line that says it is gone always prints
            UE_LOGI("[CI-CENSUS] r4 client local live=%d sim=%d eid=%u t=%llu pos=(%.1f,%.1f,%.1f)", live ? 1 : 0,
                    live && E::IsActorRootSimulatingPhysics(held) ? 1 : 0, g_thrownEid,
                    static_cast<unsigned long long>(cen::UnixMs()), at.X, at.Y, at.Z);
        g_thrownLive = live;
    }
    if (!g_mirrorEid) {
        std::string rest;
        int eid = 0;
        if (coop::dev::drill_chat::LiveRestOf(kPrefixR4Thrown, &rest) && IntAfter(rest, "eid", eid) && eid > 0)
            g_mirrorEid = static_cast<uint32_t>(eid);
    }
    void* mirror = g_mirrorEid ? coop::element::LivePropActor(g_mirrorEid) : nullptr;
    ue_wrap::FVector at{};
    if (mirror && E::TryGetActorLocation(mirror, at) && g_trMirror.Take())
        UE_LOGI("[CI-CENSUS] r4 client mirror eid=%u sim=%d t=%llu pos=(%.1f,%.1f,%.1f)", g_mirrorEid,
                E::IsActorRootSimulatingPhysics(mirror) ? 1 : 0, static_cast<unsigned long long>(cen::UnixMs()), at.X, at.Y, at.Z);
}

void StepR4Wait() {
    TraceR4();
    if (cen::R4Overlaps() <= g_ov0 && !Expired(kOverlapBoundMs)) return;
    UE_LOGI("[CI-CENSUS] drill client: R4 the thrown drive %s the backpack's overlap on the client (%llu entr%s)",
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
    g_slot0 = SlotAt(Screen(), 0);
    g_slotText0 = FirstSlotText();
    g_count0 = RecordCount(g_eid);
    g_changedMs = 0;
    LogSizes("client", "after the routes", g_pack.Get());
    cen::ReportNow("routes done");
    coop::dev::drill_chat::Say(kLineRoutesDone);
    Next(Step::ViewWait);
}

// The host writes the backpack's contents once (one record added) and publishes it: this peer's open screen is judged
// on whether the applied publication rebuilt it. Nothing else runs in this step; its baseline follows each poll until
// the publication lands, so the last one is taken just before the apply.
void StepViewWait() {
    if (RecordCount(g_eid) != g_count0 + 1) {
        g_gen0 = cen::GenCalls();
        g_slot0 = SlotAt(Screen(), 0);
        g_slotText0 = FirstSlotText();
        if (Expired(kHostBoundMs)) Skip("the open screen", "the host's publication never arrived", Step::BeforeReady);
        return;
    }
    if (!g_changedMs) g_changedMs = ::GetTickCount64();
    const bool regen = ScreenRebuilt(g_gen0, g_slot0);
    if (!regen && ::GetTickCount64() - g_changedMs < kRegenWindowMs) return;
    UE_LOGI("[CI-CENSUS] client view regen=%d slots %d -> %d%s", regen ? 1 : 0, g_slots0, SlotCount(Screen()),
            SlotChange(g_slotText0, FirstSlotText()).c_str());
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
        switch (SeamsGate("client")) {   // the first step waits for every watch to be live
            case SeamGate::Wait: return;
            case SeamGate::Dead: g_step = Step::Done; return;
            case SeamGate::Go:   break;
        }
        Next(Step::Find);
        return;
    case Step::Find:         StepFind(player); return;
    case Step::Settle:       StepSettle(); return;
    case Step::Open:         StepOpen(); return;
    case Step::OpenWait:     StepOpenWait(); return;
    case Step::R1:           StepR1(); return;
    case Step::R1Wait:       StepR1Wait(); return;
    case Step::R1Done:       StepR1Done(); return;
    case Step::R2Refill:     StepR2Refill(); return;
    case Step::R2:           StepR2(); return;
    case Step::R2Call:       StepR2Call(); return;
    case Step::R2Wait:       StepR2Wait(); return;
    case Step::R3:           StepR3(); return;
    case Step::R3Use:        StepR3Use(player); return;
    case Step::R3Wait:       StepR3Wait(); return;
    case Step::R5:           StepR5(); return;
    case Step::R5Use:        StepR5Use(player); return;
    case Step::R5Settle:     StepR5Settle(player); return;
    case Step::R5Placed:     StepR5Placed(); return;
    case Step::R5Wait:       StepR5Wait(); return;
    case Step::R4:           StepR4(player); return;
    case Step::R4HoldWait:   StepR4HoldWait(); return;
    case Step::R4Settle:     StepR4Settle(player); return;
    case Step::R4Aim:        StepR4Aim(player); return;
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
    g_thrown.Reset();
    g_vacuum.Reset();
    g_eid = static_cast<uint32_t>(coop::element::kInvalidId);
    for (ue_wrap::FVector& v : g_last) v = {};
    g_rest = g_calls = g_inv0 = g_slots0 = g_rb = g_ic = 0;
    g_slice0 = g_ov0 = g_gen0 = g_changedMs = 0;
    g_noSpace0 = g_refilled = false;
    g_count0 = g_countTaken = 0;
    g_slot0 = nullptr;
    g_slotText0.clear();
    g_intakeCm = g_radiusCm = 0.f;
    g_settled = 0;
    g_placedEid = g_thrownEid = g_mirrorEid = 0;
    g_thrownLive = false;
    g_fan = coop::director::AimFan();
    g_trLocal.Reset();
    g_trMirror.Reset();
    g_r2Add0 = g_r1Add0 = g_r1Took = 0;
    g_hashes.clear();
}

}  // namespace coop::dev::container_route_drill::detail
