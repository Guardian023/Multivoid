// coop/dev/cord_drill_hand.cpp -- see coop/dev/cord_drill_detail.h: the acting player's hand. The mechanics are
// pr50cordm's measured recipe (drill10: the walk 160 s, the press, the head 7.0 uu inside 10, the release plugged).

#include "cord_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/dev/director/input_stub.h"

#include "ue_wrap/actors/prop_use.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>

namespace coop::dev::cord_drill::detail {
namespace {

namespace D = coop::director;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;

// The use key's release: the game's own release of a held cord (cord used, pressed false -> tryToConnect).
D::InputStub g_useRelease{L"InpActEvt_use_K2Node_InputActionEvent_42"};

// The release overlaps a 5 uu sphere at the CORD's Sphere with the socket's trigger Sphere, and the dragged head rides
// the trace's hit minus 5 on z: aimed at a socket is those two spheres within 5 + the socket's radius.
constexpr float kOverlapCm = 5.f;
// Released in the air: the head farther than this from every socket of the cast.
constexpr float kAirClearCm = 100.f;
// The camera goes up 60 degrees for it, then the head follows for a few ticks before the release.
constexpr float kAirForwardCm = 200.f;
constexpr float kAirUpCm = 346.f;
constexpr int   kAirSettleTicks = 15;

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float SphereRadius(void* sphere) {
    if (!sphere || !R::IsLive(sphere)) return -1.f;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(sphere), L"SphereRadius");
    return off < 0 ? -1.f : *reinterpret_cast<const float*>(static_cast<const uint8_t*>(sphere) + off);
}

// The cord head's Sphere, the end a socket's overlap tests; false when it does not read.
bool HeadSphereAt(void* cord, ue_wrap::FVector& out) {
    void* s = FieldOf(cord, L"Sphere");
    return s && E::TryGetComponentLocation(s, out);
}

bool ClearOfSockets(const ue_wrap::FVector& head) {
    for (int k = 0; k < kSocks; ++k) {
        ue_wrap::FVector at{};
        void* s = FieldOf(Sock(k), L"Sphere");
        if (s && E::TryGetComponentLocation(s, at) && Dist(head, at) <= kAirClearCm) return false;
    }
    return true;
}

}  // namespace

void Hand::Start(const Plan& plan) {
    Cancel();
    plan_ = plan;
    fan_ = D::AimFan();
    airTicks_ = 0;
    stuckWhat_ = "";
    walk_.reset();
    if (plan.walk) st_ = St::Walk;
    else if (plan.mode == Mode::WalkOnly) st_ = St::Done;
    else st_ = St::Press;
}

void Hand::Next(St s) {
    st_ = s;
    fan_ = D::AimFan();
    airTicks_ = 0;
}

Hand::St Hand::Stick(const char* at, const char* what) {
    UE_LOGW("[CORD] %s hand stuck at %s: %s", plan_.who, at, what);
    stuckWhat_ = what;
    Cancel();
    st_ = St::Stuck;
    return st_;
}

void Hand::Cancel() {
    if (walk_ && walk_->state.load() == 0) D::CancelBackgroundWalk(*walk_);
    walk_.reset();
}

void Hand::Resume(int sock) {
    if (st_ != St::Held) return;
    plan_.sock = sock;
    Next(St::Aim);
}

Hand::St Hand::Tick(void* player) {
    switch (st_) {
    case St::Walk: {
        if (!walk_) {
            walk_ = D::StartBackgroundWalk(plan_.stand, kStandReachCm, kWalkDeadlineS);
            UE_LOGI("[CORD] %s walk started to (%.0f,%.0f,%.0f)", plan_.who, plan_.stand.X, plan_.stand.Y, plan_.stand.Z);
            return st_;
        }
        const int w = walk_->state.load();
        if (w == 0) return st_;
        walk_.reset();
        if (w == 2) return Stick("walk", "the director's walk failed or ran out its deadline");
        UE_LOGI("[CORD] %s arrived", plan_.who);
        Next(plan_.mode == Mode::WalkOnly ? St::Done : St::Press);
        return st_;
    }
    case St::Press: {
        // The press is the cord's own actionOptionIndex with this player, the function the E press ends in (it ignores
        // `action`, cord.cpp:98-112): on the floor the head lies behind the maker from every pose of the aim fan.
        void* head = FieldOf(plan_.cord, L"head");
        ue_wrap::FVector at{};
        if (!head || !E::TryGetComponentLocation(head, at)) return Stick("press", "the cord's head is unread");
        const bool ok = ue_wrap::prop_use::CallActionOptionIndex(plan_.cord, player, head, at, 0, head);
        UE_LOGI("[CORD] %s pressed ok=%d", plan_.who, ok ? 1 : 0);
        if (!ok) return Stick("press", "the cord's actionOptionIndex did not run");
        Next(plan_.mode == Mode::Grab ? St::Held : St::Aim);
        return st_;
    }
    case St::Aim:
        return TickAim(player);
    case St::Release: {
        const bool ok = D::PressInputEvent(player, g_useRelease);
        UE_LOGI("[CORD] %s released ok=%d", plan_.who, ok ? 1 : 0);
        if (!ok) return Stick("release", "the use key's release stub did not dispatch");
        Next(St::Done);
        return st_;
    }
    case St::Idle:
    case St::Held:
    case St::Done:
    case St::Stuck:
        return st_;
    }
    return st_;
}

Hand::St Hand::TickAim(void* player) {
    ue_wrap::FVector at{}, head{};
    const bool haveHead = HeadSphereAt(plan_.cord, head);
    bool aimed = false;
    if (plan_.sock >= 0) {
        void* sphere = FieldOf(Sock(plan_.sock), L"Sphere");
        if (!sphere || !E::TryGetComponentLocation(sphere, at)) return Stick("aim", "the socket's Sphere is unread");
        aimed = haveHead && Dist(head, at) <= kOverlapCm + SphereRadius(sphere);
    } else {
        const ue_wrap::FVector cam = E::GetCameraLocation();
        const ue_wrap::FVector f = E::GetActorForwardVector(player);
        at = {cam.X + f.X * kAirForwardCm, cam.Y + f.Y * kAirForwardCm, cam.Z + kAirUpCm};
        if (fan_.Poses() == 0) {   // the camera goes up first
            fan_.Tick(player, at, false);
            return st_;
        }
        aimed = haveHead && ++airTicks_ >= kAirSettleTicks && ClearOfSockets(head);
    }
    const auto st = fan_.Tick(player, at, aimed);
    if (st == D::AimFan::State::Failed) return Stick("aim", "no pose of the fan put the cord's head where it belongs");
    if (st == D::AimFan::State::Aimed) {
        UE_LOGI("[CORD] %s aimed %s", plan_.who, plan_.sock >= 0 ? "at the socket" : "into the air");
        Next(St::Release);
    }
    return st_;
}

}  // namespace coop::dev::cord_drill::detail
