// coop/world/fuse_tower_ops.cpp -- see coop/world/fuse_tower_ops.h.

#include "coop/world/fuse_tower_ops.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_lifecycle.h"  // a refused pull's fuse reaped
#include "coop/props/remote_prop.h"     // the destroys this host applies: a spent fuse
#include "coop/session/net_pump.h"      // IsInAnnouncedWorld
#include "coop/world/fuse_tower_kind.h"
#include "coop/world/fuse_tower_rows.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"             // TryGetActorLocation
#include "ue_wrap/engine/engine_mainplayer.h"  // what the local player holds
#include "ue_wrap/facts/function_row.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace coop::fuse_tower_ops {
namespace {

namespace FK = coop::fuse_tower_kind;
namespace EL = coop::element;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
using coop::net::FuseTowerPayload;

static_assert(coop::net::kMaxPeers <= 4, "FuseTowerPayload's acknowledgements hold four slots");

// A slot's ops wait, in arrival order, while the towers have not resolved, their sender has no body here yet (a
// joiner's first pose follows its world-ready by seconds) or an insert's spend has not arrived (its destroy rides
// the Default lane); each is taken as refused once it has waited this long (coop/world/power_grid's bound).
constexpr uint64_t kWaitMs = 10000;
constexpr size_t   kMaxQueued = 32;
// A lights-out puzzle is pressed a few times a second; a sender's ops run at this rate, the rest waiting their turn.
constexpr coop::net::IntentBudget kOpBudget{8.0f, 8.0f};
// A spend waits for its insert's claim; a player's inserts come seconds apart, so a few a slot cover any that race.
constexpr int kSpendsPerSlot = 4;

std::atomic<coop::net::Session*> g_session{nullptr};

coop::net::Session* Connected() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->connected()) ? s : nullptr;
}

bool IsClient(coop::net::Session* s) { return s && s->role() == coop::net::Role::Client; }

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// `a` is later than `b` in a 16-bit sequence that wraps.
bool SeqAfter(uint16_t a, uint16_t b) { return static_cast<int16_t>(static_cast<uint16_t>(a - b)) > 0; }

bool IsClaim(uint8_t op) { return op == coop::net::kFuseTowerOpPull || op == coop::net::kFuseTowerOpInsert; }

const char* OpName(uint8_t op) {
    switch (op) {
    case coop::net::kFuseTowerOpButton:  return "button press";
    case coop::net::kFuseTowerOpLever:   return "lever";
    case coop::net::kFuseTowerOpRetract: return "panel retract";
    case coop::net::kFuseTowerOpReset:   return "reset lever";
    case coop::net::kFuseTowerOpPull:    return "fuse pull";
    case coop::net::kFuseTowerOpInsert:  return "fuse insert";
    default:                              return "?";
    }
}

// The tower kind of an op, which HostOffer has admitted.
FK::Kind KindOf(const FuseTowerPayload& p) { return static_cast<FK::Kind>(p.kind); }

uint64_t g_rollsRefused = 0;

// ---- the host ------------------------------------------------------------------------------------------------

coop::roster_ledger::PerSlotState<uint16_t> g_ack;
coop::roster_ledger::PerSlotState<uint16_t> g_refused;
struct Waiting { FuseTowerPayload p; uint64_t arrivedMs; };
std::deque<Waiting> g_waiting[coop::net::kMaxPeers];
coop::net::IntentBucket g_rate[coop::net::kMaxPeers];
// A spend at the tower (kind, id); tower -1 is an empty place.
struct Spend { int32_t tower = -1; FK::Kind kind = FK::Kind::Coordinate; uint64_t ms = 0; };
struct Spends { Spend ring[kSpendsPerSlot]; uint8_t next = 0; };
coop::roster_ledger::PerSlotState<Spends> g_spends;
uint64_t g_taken = 0, g_refusedCount = 0, g_refunds = 0, g_queuesFull = 0, g_outOfReach = 0;

// A client's destroy this host applies: a good fuse its sender spent within reach of a tower's fuses is a spend
// there, judged from the host's puppet of the sender with the lane's own reach.
void OnDestroyHeard(int senderSlot, void* actor) {
    auto* s = Connected();
    if (!s || s->role() != coop::net::Role::Host) return;
    if (senderSlot <= 0 || senderSlot >= coop::net::kMaxPeers || !FK::IsFuse(actor)) return;
    const auto sender = static_cast<uint8_t>(senderSlot);
    FK::Tower towers[coop::net::kFuseTowers];
    const int32_t n = FK::ReadAll(towers, coop::net::kFuseTowers);
    const EL::IntentTarget target = EL::IntentTarget::ForClientIntent(*s, sender, kTowerReachUU);
    for (int32_t i = 0; i < n; ++i) {
        ue_wrap::FVector at{};
        if (!FK::PartLocation(towers[i].kind, towers[i].obj, coop::net::kFuseTowerOpPull, 0, at)) continue;
        const EL::IntentSubject reach = target.AuthorizeSegment(at, at);
        if (!reach) continue;
        Spends& sp = g_spends[sender];
        sp.ring[sp.next] = {towers[i].id, towers[i].kind, NowMs()};
        sp.next = static_cast<uint8_t>((sp.next + 1) % kSpendsPerSlot);
        UE_LOGI("fuse_tower: slot %u spent a fuse at tower %d (%.0f uu, allowed %.0f)", sender, towers[i].id,
                reach.distUU, reach.reachUU);
        return;
    }
}

// The newest spend `slot` made at the tower (kind, id), burnt. False when there is none.
bool TakeSpend(uint8_t slot, FK::Kind kind, int32_t id) {
    Spends& sp = g_spends[slot];
    Spend* newest = nullptr;
    for (Spend& e : sp.ring)
        if (e.tower == id && e.kind == kind && (!newest || e.ms > newest->ms)) newest = &e;
    if (!newest) return false;
    *newest = Spend{};
    return true;
}

// A refused insert's fuse, given back where its inserter stands (above the tower's fuses when the body is unread);
// the host's spawn watcher gives it to every peer.
void Refund(uint8_t slot, FK::Kind kind, void* tower) {
    ue_wrap::FVector at{};
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    void* body = rp ? rp->GetActor() : nullptr;
    bool placed = body && ue_wrap::engine::TryGetActorLocation(body, at);
    if (!placed && FK::PartLocation(kind, tower, coop::net::kFuseTowerOpPull, 0, at)) {
        at.Z += 80.f;
        placed = true;
    }
    void* fuse = placed ? FK::SpawnFuse(at) : nullptr;
    if (fuse) ++g_refunds;
    UE_LOGW("fuse_tower: slot %u's refused insert refunded -- %s", slot,
            fuse ? "a fuse spawned where it stands" : "NOT spawned (the fuse is lost)");
}

// The faults in an op that no client sends: each is warned once, however many ops carry it.
enum class Fault : uint8_t { Op, Kind, OpKind, Button, Fuse, Count };
bool g_faultSeen[static_cast<int>(Fault::Count)] = {};

bool FirstOfFault(Fault f) {
    bool& seen = g_faultSeen[static_cast<int>(f)];
    if (seen) return false;
    seen = true;
    return true;
}

enum class Take : uint8_t { Done, Wait };

Take HostTakeOp(coop::net::Session* s, const Waiting& wt, uint8_t sender) {
    const FuseTowerPayload& p = wt.p;
    const bool waited = NowMs() - wt.arrivedMs >= kWaitMs;
    const FK::Kind kind = KindOf(p);
    if (!FK::Resolved(kind) && !waited) return Take::Wait;
    if (!SeqAfter(p.seq, g_ack[sender])) {
        UE_LOGW("fuse_tower: host dropped slot %u's %s seq %u (last taken %u)", sender, OpName(p.op), p.seq,
                g_ack[sender]);
        return Take::Done;
    }
    void* tower = FK::Find(kind, p.tower);
    ue_wrap::FVector at{};
    const bool located = tower && FK::PartLocation(kind, tower, p.op, p.index, at);
    EL::IntentSubject reach{};
    if (located) reach = EL::IntentTarget::ForClientIntent(*s, sender, kTowerReachUU).AuthorizeSegment(at, at);
    if (located && reach.outcome == EL::IntentOutcome::NoBody && !waited) return Take::Wait;
    // Above the two easiest difficulties an insert rests on the fuse its own game spent, which the host saw
    // destroyed beside this tower: taken here, whatever the outcome, so one spend answers one insert. None yet waits
    // for the destroy behind it. On the two easiest the game spends none and the host runs the insert itself.
    const bool easyInsert = p.op == coop::net::kFuseTowerOpInsert && FK::EasyInsert();
    const bool needSpend = located && p.op == coop::net::kFuseTowerOpInsert && !easyInsert;
    bool spent = false;
    if (needSpend) {
        spent = TakeSpend(sender, kind, p.tower);
        if (!spent && !waited) return Take::Wait;
    }
    FK::View st;
    const bool viewed = tower && FK::ReadView(kind, tower, st);
    // A button or a fuse the host's copy of the tower does not have: one warning for the kind of fault, however
    // many ops carry it.
    const bool badButton = viewed && p.op == coop::net::kFuseTowerOpButton && p.index >= st.lightCount;
    const bool badFuse = viewed && IsClaim(p.op) && p.index >= st.fuseCount;
    const char* refusal = !tower ? "no tower of that id"
                        : badButton ? "that button is outside the puzzle"
                        : badFuse ? "that fuse is outside the tower's fuses"
                        : !located ? "that part is not there"
                        : !reach ? EL::OutcomeName(reach.outcome) : !viewed ? "the tower does not read" : nullptr;
    if (!refusal && p.op == coop::net::kFuseTowerOpPull) {
        refusal = FK::CanPull(kind, tower, st, p.index);
    } else if (!refusal && p.op == coop::net::kFuseTowerOpInsert) {
        refusal = (needSpend && !spent) ? "no fuse of the inserter's was spent at it"
                                        : FK::CanInsert(kind, st, p.index);
    }
    g_ack[sender] = p.seq;
    if (refusal) {
        ++g_refusedCount;
        // A client standing out of reach presses often: said at first and then now and then.
        bool quiet = reach.outcome == EL::IntentOutcome::OutOfReach && ++g_outOfReach > 3 && g_outOfReach % 20 != 0;
        if (badButton || badFuse) quiet = !FirstOfFault(badButton ? Fault::Button : Fault::Fuse);
        if (!quiet)
            UE_LOGW("fuse_tower: host refused slot %u's %s of tower %d (seq %u) -- %s (%.0f uu, allowed %.0f)",
                    sender, OpName(p.op), p.tower, p.seq, refusal, reach.distUU, reach.reachUU);
        if (IsClaim(p.op)) g_refused[sender] = p.seq;
        if (spent) Refund(sender, kind, tower);
        coop::fuse_tower_rows::HostSendNow();  // answered before the sender's next op is taken
        return Take::Done;
    }
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(sender);
    void* puppet = rp ? rp->GetActor() : nullptr;
    switch (p.op) {
    case coop::net::kFuseTowerOpButton:
    case coop::net::kFuseTowerOpLever:
    case coop::net::kFuseTowerOpRetract:
    case coop::net::kFuseTowerOpReset:
        // The tower's own use, on its own gates: the panel open, the lever at rest, the tower broken.
        FK::Use(kind, tower, puppet, p.op, p.index);
        break;
    case coop::net::kFuseTowerOpPull:
        FK::TakePull(kind, tower, p.index);
        break;
    case coop::net::kFuseTowerOpInsert:
        if (easyInsert) FK::TakeEasyInsert(kind, tower, p.index);
        else FK::TakeInsert(kind, tower, p.index);
        break;
    default: break;
    }
    ++g_taken;
    UE_LOGI("fuse_tower: host took slot %u's %s of tower %d (index %u, seq %u)", sender, OpName(p.op), p.tower,
            p.index, p.seq);
    coop::fuse_tower_rows::HostSendNow();
    return Take::Done;
}

void HostOffer(coop::net::Session* s, const FuseTowerPayload& p, uint8_t sender) {
    if (sender == 0 || sender >= coop::net::kMaxPeers || p.op == coop::net::kFuseTowerOpRows) return;
    // The receive boundary: an op no client sends is dropped here, with one warning for each kind of fault.
    const char* fault = nullptr;
    Fault which = Fault::Op;
    if (p.op > coop::net::kFuseTowerOpReset) {
        fault = "an op outside 1..6";
    } else if (p.kind >= coop::net::kFuseTowerKinds) {
        which = Fault::Kind;
        fault = "a tower kind that is neither tower";
    } else if ((p.op == coop::net::kFuseTowerOpRetract && p.kind == coop::net::kFuseTowerKindRadio) ||
               (p.op == coop::net::kFuseTowerOpReset && p.kind == coop::net::kFuseTowerKindCoordinate)) {
        which = Fault::OpKind;
        fault = "an op its tower kind does not have (the panel retract is the coordinate towers', the reset lever "
                "the radio tower's)";
    }
    if (fault) {
        if (FirstOfFault(which)) UE_LOGW("fuse_tower: host dropped slot %u's op %u -- %s", sender, p.op, fault);
        return;
    }
    auto& q = g_waiting[sender];
    const Waiting w{p, NowMs()};
    if (q.empty() && g_rate[sender].Take(kOpBudget, NowMs())) {
        if (HostTakeOp(s, w, sender) == Take::Done) return;
        g_rate[sender].Refund(kOpBudget);  // a wait runs nothing
    }
    if (q.size() >= kMaxQueued) {
        // Every waiting op refused with the newest, in order, as a refusal of each would answer it: a claim's own
        // rows name it refused, so a pull's fuse is reaped, and an insert's spend is refunded.
        const size_t n = q.size();
        q.push_back(w);
        for (const Waiting& r : q) {
            if (SeqAfter(r.p.seq, g_ack[sender])) g_ack[sender] = r.p.seq;
            if (!IsClaim(r.p.op)) continue;
            g_refused[sender] = r.p.seq;
            if (r.p.op == coop::net::kFuseTowerOpInsert && TakeSpend(sender, KindOf(r.p), r.p.tower))
                if (void* tower = FK::Find(KindOf(r.p), r.p.tower))
                    Refund(sender, KindOf(r.p), tower);
            coop::fuse_tower_rows::HostSendNow();
        }
        q.clear();
        g_refusedCount += n + 1;
        if (++g_queuesFull <= 3 || g_queuesFull % 100 == 0)
            UE_LOGE("fuse_tower: host refused slot %u's %zu waiting ops and seq %u -- its queue is full", sender, n,
                    p.seq);
        coop::fuse_tower_rows::HostSendNow();
        return;
    }
    q.push_back(w);
}

void HostDrainWaiting(coop::net::Session* s) {
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        auto& q = g_waiting[slot];
        while (!q.empty() && g_rate[slot].Take(kOpBudget, NowMs())) {
            if (HostTakeOp(s, q.front(), slot) == Take::Wait) {
                g_rate[slot].Refund(kOpBudget);
                break;
            }
            q.pop_front();
        }
    }
}

// ---- a client ------------------------------------------------------------------------------------------------

// My claims the host has not answered, oldest first.
struct Claim { uint16_t seq; uint8_t op; FK::Kind kind; int32_t tower; uint8_t slot; };
std::vector<Claim> g_claims;
uint16_t g_seq = 0;
uint16_t g_lastAck = 0;
uint64_t g_sent = 0, g_reaped = 0;
// The use and the insert in flight: the fuse slot the pre saw it would move.
void*   g_pullTower = nullptr;
int32_t g_pullSlot = -1;
void*   g_insertTower = nullptr;
int32_t g_insertSlot = -1;

// A pull or an insert whose fuse this client's own game moved is a claim the rows answer; an insert the gate
// cancelled (the two easiest difficulties) moved nothing here and claims nothing.
void ClientSend(coop::net::Session* s, uint8_t op, FK::Kind kind, int32_t tower, int32_t index, bool claim = true) {
    FuseTowerPayload p{};
    p.op = op;
    p.kind = static_cast<uint8_t>(kind);
    p.tower = tower;
    p.index = static_cast<uint8_t>(std::max(index, 0));
    p.seq = ++g_seq;
    if (claim && IsClaim(op)) g_claims.push_back({p.seq, op, kind, tower, p.index});
    s->SendReliableToSlot(0, coop::net::ReliableKind::FuseTowerState, &p, sizeof(p));
    ++g_sent;
    UE_LOGI("fuse_tower: local %s of tower %d (index %d, seq %u) -- sent to the host", OpName(op), tower, index,
            p.seq);
}

// A refused pull: the fuse its own game put in my hand goes, when it is still there (the floppy crate's reap).
void Reap(const Claim& c) {
    void* player = coop::players::Registry::Get().Local();
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (player && ue_wrap::engine::ReadMainPlayerGrabState(player, gs)) {
        void* held = gs.grabbingActor ? gs.grabbingActor : gs.holdingActor;
        if (held && FK::IsPulledFuse(held)) {
            coop::prop_lifecycle::DestroyLocalProp(held, /*deferred*/true);
            ++g_reaped;
            UE_LOGW("fuse_tower: my pull of tower %d's fuse %u was refused -- the fuse it handed me is reaped",
                    c.tower, c.slot);
            return;
        }
    }
    UE_LOGW("fuse_tower: my pull of tower %d's fuse %u was refused -- its fuse has left my hand, nothing reaped",
            c.tower, c.slot);
}

bool ClientGatesOpen(const sg::Call& call) {
    return !IsClient(Connected()) || RedOpen() || coop::fuse_tower_rows::Applying() || !call.object ||
           !coop::net_pump::IsInAnnouncedWorld(call.object);
}

// ---- the watches ---------------------------------------------------------------------------------------------

// A client's own roll and its lever's judgement never run: the host's world rolls and judges every tower, and
// its rows run each change here. Every route: the fuckuper's timer, an explosion, a tower that loads broken.
sg::Verdict OnRollPre(const sg::Call&) {
    if (!IsClient(Connected()) || RedOpen()) return sg::Verdict::Run;
    if (++g_rollsRefused == 1)
        UE_LOGI("fuse_tower: this client refuses its towers' own rolls and lever judgements -- the host's towers "
                "are the author");
    return sg::Verdict::Cancel;
}

// A client's use: a press of a button, a lever or the retract goes to the host, which runs it; a pull runs here
// and its move is claimed as the use ends.
int32_t g_actionOff = -1;
void* g_actionFn = nullptr;
sg::Verdict OnUsePre(const sg::Call& call) {
    g_pullTower = nullptr;
    if (ClientGatesOpen(call)) return sg::Verdict::Run;
    if (g_actionFn != call.function) {
        g_actionFn = call.function;
        g_actionOff = R::FindParamOffset(call.function, L"action");
    }
    if (g_actionOff < 0 || call.locals[g_actionOff] != 4) return sg::Verdict::Run;
    FK::Kind kind;
    int32_t id;
    uint8_t op = 0;
    int32_t index = -1;
    if (!FK::Identify(call.object, kind, id)) return sg::Verdict::Run;
    switch (FK::ReadUse(kind, call.object, op, index)) {
    case FK::Look::Send:
        ClientSend(Connected(), op, kind, id, index);
        return sg::Verdict::Cancel;
    case FK::Look::Pull:
        g_pullTower = call.object;
        g_pullSlot = index;
        return sg::Verdict::Run;
    default:
        return sg::Verdict::Run;
    }
}

void OnUsePost(const sg::Call& call) {
    if (!call.object || call.object != g_pullTower) return;
    g_pullTower = nullptr;
    FK::Kind kind;
    int32_t id;
    FK::View st;
    auto* s = Connected();
    if (!IsClient(s) || !FK::Identify(call.object, kind, id) || !FK::ReadView(kind, call.object, st) ||
        g_pullSlot >= st.fuseCount || st.fuses[g_pullSlot] != 0)
        return;
    ClientSend(s, coop::net::kFuseTowerOpPull, kind, id, g_pullSlot);
}

// A client's insert above the two easiest difficulties runs here, and its move is claimed as it ends; on those two
// it keeps the fuse and solves, so it is refused here and sent to the host like a press.
int32_t g_holdOff = -1;
void* g_insertFn = nullptr;
sg::Verdict OnInsertPre(const sg::Call& call) {
    g_insertTower = nullptr;
    if (ClientGatesOpen(call)) return sg::Verdict::Run;
    if (g_insertFn != call.function) {
        g_insertFn = call.function;
        g_holdOff = R::FindParamOffset(call.function, L"holdObject");
    }
    if (g_holdOff < 0) return sg::Verdict::Run;
    void* held = *reinterpret_cast<void* const*>(call.locals + g_holdOff);
    FK::Kind kind;
    int32_t id;
    FK::View st;
    int32_t slot = -1;
    if (!FK::IsFuse(held) || !FK::Identify(call.object, kind, id) || !FK::ReadView(kind, call.object, st) ||
        !FK::ReadFuseLook(kind, call.object, slot) || slot < 0 || slot >= st.fuseCount || st.fuses[slot] != 0)
        return sg::Verdict::Run;
    if (FK::EasyInsert()) {
        ClientSend(Connected(), coop::net::kFuseTowerOpInsert, kind, id, slot, /*claim*/false);
        return sg::Verdict::Cancel;
    }
    g_insertTower = call.object;
    g_insertSlot = slot;
    return sg::Verdict::Run;
}

void OnInsertPost(const sg::Call& call) {
    if (!call.object || call.object != g_insertTower) return;
    g_insertTower = nullptr;
    FK::Kind kind;
    int32_t id;
    FK::View st;
    auto* s = Connected();
    if (!IsClient(s) || !FK::Identify(call.object, kind, id) || !FK::ReadView(kind, call.object, st) ||
        g_insertSlot >= st.fuseCount || st.fuses[g_insertSlot] != 1)
        return;
    ClientSend(s, coop::net::kFuseTowerOpInsert, kind, id, g_insertSlot);
}

// Each kind's four bodies (coop/world/fuse_tower_kind), under one tag a kind and a role.
struct WatchDef {
    const ue_wrap::facts::Function* row;
    int tag;
    sg::PreFn pre;
    sg::PostFn post;
};
constexpr int kWatchCount = FK::kKinds * 4;
constexpr int kTags[FK::kKinds][4] = {
    { 0x43545352 /*'CTSR'*/, 0x4354464E /*'CTFN'*/, 0x4354414F /*'CTAO'*/, 0x43545555 /*'CTUU'*/ },
    { 0x52545352 /*'RTSR'*/, 0x5254464E /*'RTFN'*/, 0x5254414F /*'RTAO'*/, 0x52545555 /*'RTUU'*/ },
};
WatchDef g_watches[kWatchCount];
bool g_registered = false;
bool g_settled = false;

void BuildWatches() {
    for (int k = 0; k < FK::kKinds; ++k) {
        const FK::WatchRows& r = FK::WatchesOf(static_cast<FK::Kind>(k));
        g_watches[k * 4 + 0] = { r.roll,   kTags[k][0], &OnRollPre,   nullptr };
        g_watches[k * 4 + 1] = { r.judge,  kTags[k][1], &OnRollPre,   nullptr };
        g_watches[k * 4 + 2] = { r.use,    kTags[k][2], &OnUsePre,    &OnUsePost };
        g_watches[k * 4 + 3] = { r.insert, kTags[k][3], &OnInsertPre, &OnInsertPost };
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_registered) return;
    g_registered = true;
    coop::remote_prop::AddDestroyListener(&OnDestroyHeard);
    BuildWatches();
    for (const WatchDef& w : g_watches)
        if (!sg::WatchName(*w.row, w.tag, w.pre, w.post))
            UE_LOGE("fuse_tower: the script-body gate refused the watch on %ls::%ls -- that act stays each peer's own",
                    w.row->Class(), w.row->Name());
}

void Tick() {
    if (auto* s = Connected(); s && s->role() == coop::net::Role::Host) HostDrainWaiting(s);
    if (g_settled || !g_registered) return;
    sg::ResolvePendingNames();
    int live = 0, settled = 0;
    for (const WatchDef& w : g_watches) {
        if (sg::NameWatchLive(*w.row, w.tag)) ++live;
        if (sg::NameWatchSettled(*w.row, w.tag)) ++settled;
    }
    if (live == kWatchCount) {
        g_settled = true;
        UE_LOGI("fuse_tower: the towers' gates are live (the roll, the lever's judgement, a use, an insert)");
    } else if (settled == kWatchCount) {
        g_settled = true;
        UE_LOGE("fuse_tower: %d of %d tower gates are dead -- those acts stay each peer's own", kWatchCount - live,
                kWatchCount);
    }
}

void OnOp(const FuseTowerPayload& p, uint8_t sender) {
    auto* s = Connected();
    if (!s || s->role() != coop::net::Role::Host) return;
    HostOffer(s, p, sender);
}

void FillAcks(FuseTowerPayload& p) {
    for (int i = 0; i < coop::net::kMaxPeers; ++i) {
        p.ack[i] = g_ack[i];
        p.refused[i] = g_refused[i];
    }
}

void OnAcks(uint16_t ack, uint16_t refused) {
    g_lastAck = ack;
    size_t kept = 0;
    for (const Claim& c : g_claims) {
        if (SeqAfter(c.seq, ack)) {
            g_claims[kept++] = c;
            continue;
        }
        if (c.seq == refused) {
            if (c.op == coop::net::kFuseTowerOpPull) Reap(c);
            else UE_LOGW("fuse_tower: my insert into tower %d's slot %u was refused -- the host gives the fuse back",
                         c.tower, c.slot);
        }
    }
    g_claims.resize(kept);
}

void Overlay(coop::net::FuseTowerRow& row) {
    for (const Claim& c : g_claims) {
        if (static_cast<uint8_t>(c.kind) != row.kind || c.tower != row.id || c.slot >= row.fuseCount) continue;
        if (c.op == coop::net::kFuseTowerOpPull) {
            row.fuses[c.slot] = 0;
        } else {
            row.fuses[c.slot] = 1;
        }
    }
}

void OnPeerLeft(uint8_t slot) {
    if (slot == 0 || slot >= coop::net::kMaxPeers) return;
    g_waiting[slot].clear();
    g_rate[slot].Reset();
}

uint64_t OpsSent() { return g_sent; }
size_t PendingClaims() { return g_claims.size(); }
uint64_t OpsRefused() { return g_refusedCount; }
uint64_t RollsRefused() { return g_rollsRefused; }
uint16_t LastSentSeq() { return g_seq; }
uint16_t LastAck() { return g_lastAck; }
uint64_t Reaped() { return g_reaped; }

bool RedOpen() {
    static const bool red = [] {
        const std::string v = coop::config::ResolveString(::coop::config_registry::rows::tower_drill);
        return v == "red" || v == "joinred";
    }();
    return red;
}

void OnDisconnect() {
    if (g_sent || g_taken || g_refusedCount || g_rollsRefused)
        UE_LOGI("fuse_tower: session end -- ops sent %llu, taken %llu, refused %llu, refunds %llu, reaped %llu, "
                "rolls refused %llu", static_cast<unsigned long long>(g_sent),
                static_cast<unsigned long long>(g_taken), static_cast<unsigned long long>(g_refusedCount),
                static_cast<unsigned long long>(g_refunds), static_cast<unsigned long long>(g_reaped),
                static_cast<unsigned long long>(g_rollsRefused));
    for (auto& q : g_waiting) q.clear();
    for (auto& b : g_rate) b.Reset();
    for (int slot = 0; slot < g_spends.size(); ++slot) g_spends[slot] = Spends{};
    g_claims.clear();
    g_seq = g_lastAck = 0;
    g_pullTower = g_insertTower = nullptr;
    g_sent = g_reaped = g_taken = g_refusedCount = g_refunds = g_queuesFull = g_outOfReach = g_rollsRefused = 0;
}

}  // namespace coop::fuse_tower_ops
