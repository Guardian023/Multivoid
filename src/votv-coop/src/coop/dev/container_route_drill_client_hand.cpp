// coop/dev/container_route_drill_client_hand.cpp -- the CLIENT's hand routes of the route census drill: R5 (the vacuum
// gathers a drive the host places at its intake) and R4 (the held drive thrown at the backpack); see
// coop/dev/container_route_drill.h. The step machine they run on is container_route_drill_client_state.h.

#include "container_route_drill_client_state.h"
#include "container_route_drill_detail.h"
#include "drive_drill_verbs.h"  // Throw, Hold: the player's own pickup and throw (co-located private header)

#include "coop/dev/container_route_census.h"
#include "coop/dev/director/aim_fan.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_hand.h"
#include "coop/element/registry.h"
#include "coop/props/prop_act_request.h"
#include "coop/props/prop_drop_intent.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"   // IsActorRootSimulatingPhysics
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::container_route_drill::detail {
namespace {

namespace E   = ue_wrap::engine;
namespace R   = ue_wrap::reflection;
namespace DV  = coop::dev::drive_drill_verbs;
namespace CV  = ue_wrap::container_view;
namespace cen = coop::dev::container_route_census;

constexpr uint64_t kOverlapBoundMs = 8000;   // the throw to the prop entering the overlap

ue_wrap::CachedObjRef g_vacuum, g_thrown, g_world;   // the vacuum; the drive in the hand; the world actor the throw spawned
int      g_settled = 0;          // consecutive checks that held the step's condition
float    g_radiusCm = 0.f;       // the vacuum's intake sphere
uint32_t g_placedEid = 0;        // the host's drive at the intake (0 until the host says)
uint32_t g_thrownEid = 0;        // the held drive's element id (0 if it has none)
bool     g_worldLive = false;    // the client's trace of the throw's world actor still runs
bool     g_handSaid = false;     // the hand actor's one trace line is out
uint32_t g_mirrorEid = 0;        // the host's drive for the throw (0 until the host says)
coop::director::AimFan g_fan;
TraceRate g_trLocal, g_trMirror;
bool     g_noSpace0 = false;
uint64_t g_ov0 = 0;
float    g_intakeCm = 0.f;       // the gathered drive's distance from the vacuum's intake

uint32_t WireEid(void* actor) {   // the element id the host reads: 0 when the actor has none
    const uint32_t eid = EidOf(actor);
    return eid == static_cast<uint32_t>(coop::element::kInvalidId) ? 0 : eid;
}

// The container screen closed the way its close key closes it (the game's own exit), and awaited: a route that aims
// and acts through the player's own trace runs with no interface up.
void CloseScreen(const char* phase, Step next) {
    if (!CV::Viewed()) { Next(next); return; }
    CV::Close();
    if (Expired(kHandBoundMs)) FailAt(phase);
}

bool ReadBoolField(void* obj, const wchar_t* name, bool& out) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask) || off < 0) return false;
    out = (*(static_cast<const uint8_t*>(obj) + off) & mask) != 0;
    return true;
}

}  // namespace

void StepR5Close() { CloseScreen("R5 the screen would not close", Step::R5); }

void StepR5() {
    if (!coop::dev::drill_hand::TakeIntoHandNow(L"vacuumC", L"prop_vacuum2_C")) { FailAt("R5 the vacuum could not be taken into the hand"); return; }
    Next(Step::R5Use);   // the hand's actor is read a tick later
}

namespace {
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
}  // namespace

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

// The host spawns the drive AT the settled intake and says its id; the aim begins once that drive's mirror is here.
void StepR5Placed() {
    if (!g_placedEid) {
        std::string rest;
        int eid = 0;
        if (coop::dev::drill_chat::LiveRestOf(kPrefixR5Placed, &rest) && IntAfter(rest, "eid", eid) && eid > 0)
            g_placedEid = static_cast<uint32_t>(eid);
    }
    if (void* mirror = g_placedEid ? coop::element::LivePropActor(g_placedEid) : nullptr) {
        g_drive[0].Set(mirror);
        g_fan = coop::director::AimFan();
        Next(Step::R5Aim);
        return;
    }
    if (Expired(kHostBoundMs)) {
        UE_LOGW("[CI-CENSUS] drill client: R5 placed eid=%u, mirror not seen", g_placedEid);
        FailAt("R5 the placed drive's mirror never arrived");
    }
}

// Aims like a player: the camera turned through the fan round the placed drive until the intake (which follows the
// hand) lies within its sphere's radius of it; then the gather.
void StepR5Aim(void* player) {
    void* vac = g_vacuum.Get();
    void* drive = g_drive[0].Get();
    if (!vac || !drive) { FailAt("R5 the vacuum or the placed drive is gone"); return; }
    ue_wrap::FVector at{}, d{};
    if (!E::TryGetComponentLocation(SuckOf(vac), at) || !E::TryGetActorLocation(drive, d)) {
        if (Expired(kHostBoundMs)) FailAt("R5 the intake never came within reach of the placed drive");
        return;
    }
    const float cm = Dist(d, at);
    switch (g_fan.Tick(player, d, cm <= g_radiusCm)) {
        case coop::director::AimFan::State::Working:
            if (Expired(kHostBoundMs)) FailAt("R5 the intake never came within reach of the placed drive");
            return;
        case coop::director::AimFan::State::Failed:
            FailAt("R5 the intake never came within reach of the placed drive");
            return;
        case coop::director::AimFan::State::Aimed: break;
    }
    g_intakeCm = cm;
    ReadBoolField(vac, L"noSpace", g_noSpace0);
    UE_LOGI("[CI-CENSUS] drill client: R5 the placed drive (eid=%u) is %.0f cm from the intake (%.0f,%.0f,%.0f), "
            "radius %.0f cm after %d aim pose(s), gather called", g_placedEid, cm, at.X, at.Y, at.Z, g_radiusCm, g_fan.Poses());
    bool called = false;
    {
        cen::GatherScope own;   // the one place drill-vacuumcancel cancels the client's addObject
        called = CallNoArg(vac, L"gather");
    }
    if (!called) { FailAt("R5 gather did not dispatch"); return; }
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
    UE_LOGI("[CI-CENSUS] R5 result: noSpace %d -> %d, the gathered drive (%.0f cm from the intake, radius %.0f cm) %s at (%.0f,%.0f,%.0f), "
            "route entries %llu, add verbs counted R5 %llu, R4 overlaps %llu, addObject cancelled %llu", g_noSpace0 ? 1 : 0,
            noSpace ? 1 : 0, g_intakeCm, g_radiusCm, live ? "still live" : "gone", at.X, at.Y, at.Z,
            static_cast<unsigned long long>(cen::RouteEntries(4)), static_cast<unsigned long long>(cen::RouteVerbs(4, false)),
            static_cast<unsigned long long>(cen::R4Overlaps()), static_cast<unsigned long long>(cen::VacuumCancelled()));
    LogSizes("client", "the vacuum after R5", vac);
    coop::dev::drill_hand::TakeIntoHandNow(L"None", nullptr);
    Finish("R5", Step::R4Close);
}

namespace {
// The aimed throw: the hand's drive is named to the host, then thrown by the game's own throw at the human seam. The
// world actor the throw spawns is the one the client's place hook caught.
void ThrowTheHeld(void* player) {
    void* held = g_thrown.Get();
    char simText[16] = "live=0";   // a hand actor that is gone has no sim to read
    if (held) std::snprintf(simText, sizeof(simText), "sim=%d", E::IsActorRootSimulatingPhysics(held) ? 1 : 0);
    char say[48];
    std::snprintf(say, sizeof(say), "%seid=%u", kPrefixR4Start, g_thrownEid);
    coop::dev::drill_chat::Say(say);
    UE_LOGI("[CI-CENSUS] drill client: R4 the held drive eid=%u %s, thrown at the backpack", g_thrownEid, simText);
    g_ov0 = cen::R4Overlaps();
    g_handSaid = false;
    g_mirrorEid = 0;
    g_trLocal.Reset();
    g_trMirror.Reset();
    void* const before = coop::prop_drop_intent::dev::LastClientPlaceSpawn();
    if (!DV::Throw(player)) { FailAt("R4 the hand's drive could not be thrown"); return; }
    void* const placed = coop::prop_drop_intent::dev::LastClientPlaceSpawn();
    g_worldLive = placed && placed != before;
    if (g_worldLive) g_world.Set(placed);
    UE_LOGI("[CI-CENSUS] r4 client local caught=%d", g_worldLive ? 1 : 0);
    Next(Step::R4Wait);
}
}  // namespace

void StepR4Close() { CloseScreen("R4 the screen would not close", Step::R4); }

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
    if (coop::prop_act_request::dev::PendingId() == 0) { Next(Step::R4Settle); return; }
    if (Expired(kHandBoundMs)) FailAt("R4 the hold's grant never came");
}

// The hold is settled once the hand holds a drive on two consecutive checks; the host is then told, and takes the
// drives it holds before the throw can make another.
void StepR4Settle(void* player) {
    void* hand = HeldActor(player);
    g_settled = hand && R::ClassNameOf(hand) == kDriveClass ? g_settled + 1 : 0;
    if (g_settled >= 2) {
        g_thrown.Set(hand);
        g_thrownEid = WireEid(hand);
        char say[48];
        std::snprintf(say, sizeof(say), "%seid=%u", kPrefixR4Hold, g_thrownEid);
        coop::dev::drill_chat::Say(say);
        Next(Step::R4Armed);
        return;
    }
    if (Expired(kHandBoundMs)) FailAt("R4 the hold never settled");
}

// The aim and the throw wait for the host to say it holds its set of drives.
void StepR4Armed() {
    if (coop::dev::drill_chat::ChatHasLive(kLineR4Armed)) {
        g_fan = coop::director::AimFan();
        Next(Step::R4Aim);
        return;
    }
    if (Expired(kHostBoundMs)) FailAt("R4 the host never armed the throw");
}

// The camera turned through the fan round the backpack until the player's own interaction trace strikes it (the read
// of director/aimed_grab.cpp's Tick: ReadMainPlayerHitActor against the prop).
void StepR4Aim(void* player) {
    void* pack = g_pack.Get();
    ue_wrap::FVector at{};
    if (!pack || !E::TryGetActorLocation(pack, at)) { FailAt("R4 the backpack's mirror died"); return; }
    switch (g_fan.Tick(player, at, E::ReadMainPlayerHitActor(player) == pack)) {
        case coop::director::AimFan::State::Working:
            if (Expired(kHostBoundMs)) FailAt("R4 the aim never struck the backpack");
            return;
        case coop::director::AimFan::State::Failed:  FailAt("R4 the aim never struck the backpack"); return;
        case coop::director::AimFan::State::Aimed:   ThrowTheHeld(player); return;
    }
}

namespace {
// From the throw to r4-done, one read of each cached ref per check: the hand actor once, the world actor the throw
// spawned until it dies, then the host's copy of the drive its drop path made.
void TraceR4() {
    const unsigned long long now = static_cast<unsigned long long>(cen::UnixMs());
    if (!g_handSaid) {
        g_handSaid = true;
        UE_LOGI("[CI-CENSUS] r4 client hand live=%d eid=%u t=%llu", g_thrown.Get() ? 1 : 0, g_thrownEid, now);
    }
    if (g_worldLive) {
        void* world = g_world.Get();
        ue_wrap::FVector at{};
        const bool live = world && E::TryGetActorLocation(world, at);
        if (!live || g_trLocal.Take())   // the line that says it is gone always prints
            UE_LOGI("[CI-CENSUS] r4 client local live=%d sim=%d eid=%u t=%llu pos=(%.1f,%.1f,%.1f)", live ? 1 : 0,
                    live && E::IsActorRootSimulatingPhysics(world) ? 1 : 0, live ? WireEid(world) : 0u, now, at.X, at.Y, at.Z);
        g_worldLive = live;
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
                E::IsActorRootSimulatingPhysics(mirror) ? 1 : 0, now, at.X, at.Y, at.Z);
}
}  // namespace

void StepR4Wait() {
    TraceR4();
    if (cen::R4Overlaps() <= g_ov0 && !Expired(kOverlapBoundMs)) return;
    UE_LOGI("[CI-CENSUS] drill client: R4 the thrown drive %s the backpack's overlap on the client (%llu entr%s)",
            cen::R4Overlaps() > g_ov0 ? "entered" : "did NOT enter",
            static_cast<unsigned long long>(cen::R4Overlaps() - g_ov0), cen::R4Overlaps() - g_ov0 == 1 ? "y" : "ies");
    coop::dev::drill_chat::Say(kLineR4Done);
    Finish("R4", Step::ViewOpen);
}

void HandReset() {
    g_thrown.Reset();
    g_world.Reset();
    g_vacuum.Reset();
    g_noSpace0 = false;
    g_ov0 = 0;
    g_intakeCm = g_radiusCm = 0.f;
    g_settled = 0;
    g_placedEid = g_thrownEid = g_mirrorEid = 0;
    g_worldLive = g_handSaid = false;
    g_fan = coop::director::AimFan();
    g_trLocal.Reset();
    g_trMirror.Reset();
}

}  // namespace coop::dev::container_route_drill::detail
