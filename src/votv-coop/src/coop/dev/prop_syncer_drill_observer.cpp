// coop/dev/prop_syncer_drill_observer.cpp -- the observing client (slot 2) of the prop syncer drill, see
// coop/dev/prop_syncer_drill.h.
//   kick:  on the client's rested line and the host's ended line, waits for its own End and judges what the relay
//          brought: rows of the granted generation, the copy's travel and its rest.
//   forge: from the client's kicked line, publishes rows for its eid at this player's place on the generation the
//          relayed stream carries, every tick it may, until the host's ended line.

#include "prop_syncer_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/dev/drill_chat.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/props/remote_prop.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <cstdint>
#include <string>

namespace coop::dev::prop_syncer_drill::detail {
namespace {

namespace DC  = coop::dev::drill_chat;
namespace PDS = coop::prop_drive_stream;
namespace PR  = ue_wrap::prop;
namespace E   = ue_wrap::engine;

struct ObserverState {
    bool               done = false, started = false;
    uint64_t           at = 0, endedAt = 0, kickedAt = 0;
    uint32_t           forged = 0, eid = 0;
    Rested             r;
    coop::net::WireKey key{};
} g_o;

void Kick(uint64_t now) {
    ObserverState& o = g_o;
    if (o.endedAt == 0) {
        uint32_t endedEid = 0;
        if (DC::PollDue() && ReadRested(&o.r) && ReadEid(kEndedPrefix, &endedEid) && endedEid == o.r.eid) o.endedAt = now;
        else if (now - o.at > kSayBoundMs + kEndBoundMs) Fail("observer", o.done, "the client's rested or the host's ended line never came");
        return;
    }
    const PDS::dev::DriveView view = PDS::dev::ViewOf(o.r.eid);
    if (!view.endApplied) {
        if (now - o.endedAt > kEndBoundMs) Fail("observer", o.done, "no End applied after the host's end");
        return;
    }
    if (view.rows == 0) return Fail("observer", o.done, "no relayed row before the host's end");
    if (view.rows < kMinRows)
        return Fail("observer", o.done, Fmt("only %u relayed rows before the host's end (want >= %u)", view.rows, kMinRows));
    const Verdict v = JudgeCopy(o.r, view, true);
    if (!v.why.empty()) return Fail("observer", o.done, v.why);
    Pass("observer", o.done, Fmt("eid=%u gen=%u rows=%u travelled=%.1f of %.1f cm off=%.1f cm", o.r.eid,
                                 static_cast<unsigned>(view.gen), view.rows, v.travelled, o.r.cm, v.offCm));
}

void Forge(coop::net::Session* s, uint64_t now) {
    ObserverState& o = g_o;
    if (o.eid == 0) {
        if (!DC::PollDue() || !ReadEid(kKickedPrefix, &o.eid)) {
            if (now - o.at > kSayBoundMs) Fail("observer", o.done, "the client never said it kicked");
            return;
        }
        o.kickedAt = now;
    }
    uint32_t endedEid = 0;
    if (ReadEid(kEndedPrefix, &endedEid) && endedEid == o.eid) return Pass("observer", o.done, Fmt("eid=%u forged=%u rows", o.eid, o.forged));
    const PDS::dev::DriveView view = PDS::dev::ViewOf(o.eid);
    if (view.rows == 0 || view.endApplied) {
        if (view.rows == 0 && now - o.kickedAt > kGenBoundMs) Fail("observer", o.done, "no relayed row to read the generation from");
        return;
    }
    void* prop = coop::remote_prop::ResolveLiveActorByEid(o.eid);
    void* player = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    ue_wrap::FRotator rot{};
    if (!prop || !player || !E::TryGetActorLocation(player, at) || !E::TryGetActorRotation(player, rot)) return;
    if (o.key.len == 0) {
        const std::wstring w = PR::GetInteractableKeyString(prop);
        for (size_t i = 0; i < w.size() && i < sizeof(o.key.data); ++i) o.key.data[o.key.len++] = static_cast<char>(w[i]);
    }
    if (s->PropDrivePoseTurn(o.eid) == coop::net::PoseTurn::Wait) return;
    coop::net::PropPoseSnapshot pp{};
    pp.key = o.key;
    pp.elementId = o.eid;
    pp.ctx = view.gen;
    pp.x = at.X; pp.y = at.Y; pp.z = at.Z;
    pp.pitch = ue_wrap::NormalizeAxis(rot.Pitch);
    pp.yaw = ue_wrap::NormalizeAxis(rot.Yaw);
    pp.roll = ue_wrap::NormalizeAxis(rot.Roll);
    s->PublishPropDrivePose(pp);
    if (++o.forged == 1)
        UE_LOGI("[PROP-SYNC] observer forging rows for eid=%u at gen=%u from its own place", o.eid, static_cast<unsigned>(view.gen));
}

}  // namespace

void ObserverTick(coop::net::Session* s, Arm a) {
    if (g_o.done) return;
    if (!g_o.started) { g_o.started = true; g_o.at = Now(); }
    if (a == Arm::Forge) Forge(s, Now());
    else Kick(Now());
}

void ObserverReset() { g_o = ObserverState{}; }

}  // namespace coop::dev::prop_syncer_drill::detail
