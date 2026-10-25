// coop/dev/prop_syncer_drill_host.cpp -- the host of the prop syncer drill, see coop/dev/prop_syncer_drill.h.
//   "PSYNC go" once both clients said they are ready (their own streams run); on "rested" waits for its own End to
//   apply, says "ended", and judges the stream it took and where its copy rests.

#include "prop_syncer_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/dev/drill_chat.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/props/prop_syncer_host.h"

#include <cstdint>
#include <string>

namespace coop::dev::prop_syncer_drill::detail {
namespace {

namespace DC  = coop::dev::drill_chat;
namespace PDS = coop::prop_drive_stream;
namespace PSH = coop::prop_syncer_host;

struct HostState {
    // The ready lines are latched the first time they are seen: a chat row is live for a short while, and the two
    // clients can join further apart than that.
    bool     done = false, clientReady = false, observerReady = false, goSaid = false, restedSeen = false, endedSaid = false;
    uint64_t at = 0, goAt = 0, restedAt = 0;
    Rested   r;
} g_h;

void HostJudge(Arm a) {
    HostState& h = g_h;
    const PDS::dev::DriveView view = PDS::dev::ViewOf(h.r.eid);
    if (a == Arm::Forge) {
        const uint32_t admitted = PSH::dev::NonSyncerAdmitted(), counted = PSH::dev::NotSyncerCount();
        if (admitted > 0) return Fail("host", h.done, Fmt("admitted %u rows from a non-syncer", admitted));
        if (counted < 1) return Fail("host", h.done, "no forged row was counted NotSyncer");
        const Verdict v = JudgeCopy(h.r, view, false);
        if (!v.why.empty()) return Fail("host", h.done, v.why);
        return Pass("host", h.done, Fmt("eid=%u not_syncer=%u admitted_from_non_syncer=0 off=%.1f cm", h.r.eid, counted, v.offCm));
    }
    // The stream is judged, not the End: rows applied to this copy (each fed by an admitted row) before the End, and
    // how far the last of them took the copy.
    const PSH::dev::Admitted adm = PSH::dev::AdmittedOf(h.r.eid);
    if (view.rows < kMinRows)
        return Fail("host", h.done, Fmt("applied %u rows of the granted generation before the End (want >= %u; admitted %u)",
                                        view.rows, kMinRows, adm.rows));
    const Verdict v = JudgeCopy(h.r, view, true);
    if (!v.why.empty()) return Fail("host", h.done, v.why);
    Pass("host", h.done, Fmt("eid=%u gen=%u rows=%u admitted=%u travelled=%.1f of %.1f cm off=%.1f cm", h.r.eid,
                             static_cast<unsigned>(view.gen), view.rows, adm.rows, v.travelled, h.r.cm, v.offCm));
}

}  // namespace

void HostTick(Arm a) {
    HostState& h = g_h;
    if (h.done) return;
    const uint64_t now = Now();
    if (h.at == 0) h.at = now;
    if (!h.goSaid) {
        if (DC::PollDue()) {
            h.clientReady = h.clientReady || DC::ChatHasLive(kClientReadyLine);
            h.observerReady = h.observerReady || DC::ChatHasLive(kObserverReadyLine);
        }
        if (h.clientReady && h.observerReady) {
            DC::Say(kGoLine);
            h.goSaid = true;
            h.goAt = now;
        } else if (now - h.at > kReadyBoundMs) {
            Fail("host", h.done, "the clients never said they were ready");
        }
        return;
    }
    if (!h.restedSeen) {
        if (DC::PollDue() && ReadRested(&h.r)) { h.restedSeen = true; h.restedAt = now; }
        else if (now - h.goAt > kSayBoundMs) Fail("host", h.done, "the client never said it rested");
        return;
    }
    if (!PDS::dev::ViewOf(h.r.eid).endApplied) {
        if (now - h.restedAt > kEndBoundMs) Fail("host", h.done, "no End applied for the client's prop");
        return;
    }
    if (!h.endedSaid) {
        DC::Say(std::string(kEndedPrefix) + std::to_string(h.r.eid));
        h.endedSaid = true;
    }
    HostJudge(a);
}

void HostReset() { g_h = HostState{}; }

}  // namespace coop::dev::prop_syncer_drill::detail
