// coop/dev/tower_drill_radio.cpp -- the tower drill's radio arms (radioreach, radio, radiored); see
// coop/dev/tower_drill.h. The arm table and what the arms share are tower_drill.cpp's and tower_drill_detail.h.
//
// The BP fact the solver stands on (bp_cpp radiotower_C, line numbers are its pseudo-C++): a press of cell `i` runs
// switchNeighbors (:1414), which toggles the five cells (row + dr, col + dc) for (dr, dc) in (0,0), (-1,0), (1,0),
// (0,-1), (0,1), with row = i / puzzleSize.Y and col = i % puzzleSize.Y, each wrapped as
// ((row + dr + puzzleSize.X) % puzzleSize.X) * puzzleSize.Y + ((col + dc + puzzleSize.Y) % puzzleSize.Y)
// (:1430-:1470); each toggle is switchSwitch (:1345), so a neighbour the wrap names twice (a 2-wide torus) is
// toggled twice and ends unchanged.

#include "tower_drill_detail.h"

#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "coop/world/fuse_tower_ops.h"
#include "coop/world/fuse_tower_rows.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/radio_tower.h"
#include "ue_wrap/engine/engine.h"  // TryGetActorLocation

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The arms. radioreach: the client walks to the tower with the director and prints the route and how far the walk
// ended from each part a use reaches; a measurement, no verdict. radio: the host scrambles its tower until it reads
// broken; the client reads the break, walks as above and, when every part it uses is within the lane's reach,
// solves the puzzle with presses through the tower's own use and pulls the confirm lever; PASS when its copy and the
// host's last row read whole 20 s at most after the lever. radiored: radio with the client's gates open and no rows
// applied, which ends `radio FAIL: the host's tower stays broken`.

namespace coop::dev::tower_drill {
namespace {

using namespace detail;

namespace RT = ue_wrap::radio_tower;
namespace DR = coop::director;
using coop::net::FuseTowerRow;
using ue_wrap::FVector;

// ---- bounds: failure bounds of the instrument, never a wait for a milestone ----
constexpr int      kMaxScrambles = 20;       // the host's calls to break its tower, one a second
constexpr uint64_t kScrambleGapMs = 1000;
constexpr uint64_t kCrossMs = 30000;         // the host's break reaching this copy; a press answered
constexpr uint64_t kVerdictMs = 20000;       // the lever's repair reaching the host's row and this copy
constexpr int      kWalkDeadlineS = 300;     // the director's own bound on the walk
constexpr float    kRingCm = 150.f;          // the standpoints about the confirm lever
constexpr int      kStandpoints = 8;
constexpr float    kStandReachCm = 60.f;     // a walk ends this near its standpoint, flat

// ---- the puzzle: the torus the radio tower's presses act on ----

// One press of cell `i` on the host's puzzle map, applied to `cells` (a W x H torus): the five toggles of
// switchNeighbors, each wrapped, a doubled toggle cancelling itself.
void Toggle(bool* cells, int width, int height, int i) {
    static const int kOffsets[5][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    const int row = i / height, col = i % height;
    for (const auto& o : kOffsets) {
        const int at = ((row + o[0] + width) % width) * height + ((col + o[1] + height) % height);
        cells[at] = !cells[at];
    }
}

// The presses that light every cell: the system over GF(2), one unknown a press, one equation a cell, solved by
// Gauss-Jordan elimination with the free unknowns left unpressed; the answer is checked by playing it.
bool SolveTorus(const RT::State& st, std::vector<int>& presses) {
    const int n = st.cellCount, width = st.width, height = st.height;
    if (n <= 0 || n > RT::kMaxCells || width <= 0 || height <= 0 || width * height != n) return false;
    uint32_t column[RT::kMaxCells] = {};  // bit c of column[j]: pressing j toggles cell c
    for (int j = 0; j < n; ++j) {
        bool v[RT::kMaxCells] = {};
        Toggle(v, width, height, j);
        for (int c = 0; c < n; ++c)
            if (v[c]) column[j] |= 1u << c;
    }
    uint64_t rows[RT::kMaxCells] = {};  // bit j: the unknown j; bit n: the right side, a cell that must flip
    for (int c = 0; c < n; ++c) {
        for (int j = 0; j < n; ++j)
            if ((column[j] >> c) & 1u) rows[c] |= 1ull << j;
        if (!st.cells[c]) rows[c] |= 1ull << n;
    }
    int pivotColumn[RT::kMaxCells] = {};
    int rank = 0;
    for (int col = 0; col < n; ++col) {
        int p = -1;
        for (int r = rank; r < n && p < 0; ++r)
            if ((rows[r] >> col) & 1ull) p = r;
        if (p < 0) continue;
        std::swap(rows[rank], rows[p]);
        for (int r = 0; r < n; ++r)
            if (r != rank && ((rows[r] >> col) & 1ull)) rows[r] ^= rows[rank];
        pivotColumn[rank++] = col;
    }
    for (int r = rank; r < n; ++r)
        if ((rows[r] >> n) & 1ull) return false;  // an equation with no unknown left and a cell to flip
    presses.clear();
    for (int r = 0; r < rank; ++r)
        if ((rows[r] >> n) & 1ull) presses.push_back(pivotColumn[r]);
    bool played[RT::kMaxCells] = {};
    for (int c = 0; c < n; ++c) played[c] = st.cells[c];
    for (int i : presses) Toggle(played, width, height, i);
    for (int c = 0; c < n; ++c)
        if (!played[c]) return false;
    return true;
}

bool AllLit(const RT::State& st) {
    if (st.cellCount == 0) return false;
    for (int i = 0; i < st.cellCount; ++i)
        if (!st.cells[i]) return false;
    return true;
}

bool CellsEqual(const RT::State& l, const FuseTowerRow& w) {
    if (l.cellCount != w.lightCount) return false;
    for (int i = 0; i < l.cellCount; ++i)
        if (l.cells[i] != (((w.lights >> i) & 1u) != 0)) return false;
    return true;
}

std::string Digits(const RT::State& st, bool fuses) {
    std::string s;
    const int n = fuses ? st.fuseCount : st.cellCount;
    for (int i = 0; i < n; ++i) s += fuses ? static_cast<char>('0' + st.fuses[i] % 10) : (st.cells[i] ? '1' : '0');
    return s;
}

bool IsRed() { return ArmOf() == Arm::RadioRed; }

const char* ArmName() {
    switch (ArmOf()) {
    case Arm::RadioReach: return "radioreach";
    case Arm::RadioRed:   return "radiored";
    default:              return "radio";
    }
}

// ---- the host: the break, on its own roll ---------------------------------------------------------------------

bool     g_hostSaid = false;
bool     g_hostBroken = false;
bool     g_hostBroke = false;      // the tower has read broken once: the drill calls no more rolls
bool     g_hostGaveUp = false;
int      g_hostScrambles = 0;
uint64_t g_hostCallMs = 0;

void HostTick(coop::net::Session& s) {
    void* t = RT::Instance();
    RT::State st;
    if (!t || !RT::Read(t, st) || st.fuseCount == 0 || st.cellCount == 0) return;
    if (!g_hostSaid || st.isBroken != g_hostBroken) {
        g_hostSaid = true;
        g_hostBroken = st.isBroken;
        UE_LOGI("[TOWER-DRILL] host radio broken=%d", st.isBroken ? 1 : 0);
    }
    if (st.isBroken) g_hostBroke = true;
    if (ArmOf() == Arm::RadioReach || g_hostBroke || g_hostGaveUp || !s.IsSlotWorldReady(1)) return;
    if (g_hostScrambles >= kMaxScrambles) {
        g_hostGaveUp = true;
        UE_LOGW("[TOWER-DRILL] host radio could not break the tower in %d scrambles -- FAIL", kMaxScrambles);
        return;
    }
    const uint64_t now = NowMs();
    if (now - g_hostCallMs < kScrambleGapMs) return;
    g_hostCallMs = now;
    RT::Scramble(t);
    ++g_hostScrambles;
}

// ---- the client: walks to the tower, reads the reach, then repairs it ------------------------------------------

enum class Step : uint8_t { Arm, Break, Walk, Press, Lever, Done };
Step     g_step = Step::Arm;
uint64_t g_stepMs = 0;
bool     g_acted = false;
uint16_t g_actSeq = 0;
std::shared_ptr<DR::BackgroundWalk> g_walk;
bool     g_walkAsked = false;
float    g_routeUU = -1.f;
std::vector<int> g_presses;
size_t   g_pressNext = 0;
uint64_t g_leverMs = 0;

void Go(Step s) {
    g_step = s;
    g_stepMs = NowMs();
    g_acted = false;
}

bool Overdue(uint64_t bound) { return NowMs() - g_stepMs > bound; }

void Done(const char* verdict) {
    g_step = Step::Done;
    const auto e = coop::fuse_tower_rows::AppliedEdges(coop::net::kFuseTowerKindRadio);
    UE_LOGI("[TOWER-DRILL] client DONE %s (ops sent %llu, claims unanswered %zu, rolls refused %llu; applied: "
            "breaks %u repairs %u presses %u pulls %u inserts %u levers %u fails %u)", verdict,
            static_cast<unsigned long long>(coop::fuse_tower_ops::OpsSent()), coop::fuse_tower_ops::PendingClaims(),
            static_cast<unsigned long long>(coop::fuse_tower_ops::RollsRefused()), e.breaks, e.repairs, e.presses,
            e.pulls, e.inserts, e.levers, e.fails);
}

void Fail(const char* why) {
    UE_LOGW("[TOWER-DRILL] radio FAIL: %s", why);
    Done("FAIL");
}

// The rows have answered my last op.
bool Answered() {
    return static_cast<int16_t>(static_cast<uint16_t>(coop::fuse_tower_ops::LastAck() - g_actSeq)) >= 0;
}

// A part of the tower and how far the walker stands from it; -1 while the part is not there.
struct PartDist {
    const char* name;
    RT::LookAt  at;
    float       uu = -1.f;
};

void Measure(void* tower, const FVector& me, PartDist& p) {
    FVector where{};
    p.uu = RT::PartLocation(tower, p.at, where) ? Dist(me, where) : -1.f;
}

// What the walk ended on: the line every radio arm prints, and for the radio arms the reach check of the parts
// they use. Returns false when the arm ended here.
bool AfterWalk(void* tower, void* player, bool reached) {
    FVector me{};
    if (!ue_wrap::engine::TryGetActorLocation(player, me)) {
        UE_LOGW("[TOWER-DRILL] %s: the walker's place does not read", ArmName());
        Done("ABANDONED");
        return false;
    }
    PartDist button0{"button0", {RT::Part::Button, 0}};
    PartDist confirm{"confirm lever", {RT::Part::Confirm, -1}};
    PartDist reset{"reset lever", {RT::Part::Reset, -1}};
    PartDist fuse0{"fuse0", {RT::Part::Fuse, 0}};
    for (PartDist* p : {&button0, &confirm, &reset, &fuse0}) Measure(tower, me, *p);
    UE_LOGI("[TOWER-DRILL] %s route=%.0f reached=%d dist button0=%.0f confirm=%.0f reset=%.0f fuse0=%.0f", ArmName(),
            g_routeUU, reached ? 1 : 0, button0.uu, confirm.uu, reset.uu, fuse0.uu);
    if (ArmOf() == Arm::RadioReach) {
        Done("radioreach");
        return false;
    }
    // Every part this client uses must be within the lane's reach, or the fixture, not the lane, is what ended it.
    std::vector<PartDist> used{confirm};
    for (int i : g_presses) {
        PartDist p{"puzzle button", {RT::Part::Button, i}};
        Measure(tower, me, p);
        used.push_back(p);
    }
    for (const PartDist& p : used)
        if (p.uu < 0.f || p.uu > coop::fuse_tower_ops::kTowerReachUU) {
            UE_LOGW("[TOWER-DRILL] radio ABANDONED: %s at %.0f after the walk", p.name, p.uu);
            Done("ABANDONED");
            return false;
        }
    return true;
}

void ClientTick(void* player) {
    void* tower = RT::Instance();
    RT::State l;
    const bool haveL = tower && RT::Read(tower, l);
    FuseTowerRow w{};
    const bool haveW = FindRow(coop::net::kFuseTowerKindRadio, 0, w);
    const bool radio = ArmOf() != Arm::RadioReach;
    switch (g_step) {
    case Step::Arm: {
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        if (g_stepMs == 0) g_stepMs = NowMs();
        if (!haveL || !haveW) {
            if (Overdue(kCrossMs)) {
                UE_LOGW("[TOWER-DRILL] %s ABANDONED: no radio tower here, or the host's rows never came", ArmName());
                Done("ABANDONED");
            }
            return;
        }
        FVector at{};
        ue_wrap::engine::TryGetActorLocation(tower, at);
        UE_LOGI("[TOWER-DRILL] client %s census: the radio tower at (%.0f, %.0f, %.0f), puzzle %dx%d, broken %d, "
                "fuses %s cells %s", ArmName(), at.X, at.Y, at.Z, l.width, l.height, l.isBroken ? 1 : 0,
                Digits(l, true).c_str(), Digits(l, false).c_str());
        Go(radio ? Step::Break : Step::Walk);
        return;
    }
    case Step::Break: {
        // The control applies no rows, so there the host's break is read from the kept row alone.
        const bool crossed = haveW && RowBroken(w) && (IsRed() || (haveL && l.isBroken && CellsEqual(l, w)));
        if (crossed) {
            if (!haveL || !SolveTorus(l, g_presses)) {
                UE_LOGW("[TOWER-DRILL] radio ABANDONED: no set of presses lights this puzzle (the fixture)");
                Done("ABANDONED");
                return;
            }
            UE_LOGI("[TOWER-DRILL] client PASS the host's break reached this copy: cells %s, %zu presses solve it",
                    Digits(l, false).c_str(), g_presses.size());
            Go(Step::Walk);
            return;
        }
        if (Overdue(kCrossMs)) {
            UE_LOGW("[TOWER-DRILL] FAIL: the host's break never reached this copy");
            Done("FAIL");
        }
        return;
    }
    case Step::Walk: {
        if (!tower) return;
        if (!g_walkAsked) {
            // The walk is the director's, to a standpoint about the confirm lever that a route reaches (a part on a
            // platform ends no route of its own); a walk that gives up is a number, never a retry.
            FVector me{}, lever{};
            if (!ue_wrap::engine::TryGetActorLocation(player, me) ||
                !RT::PartLocation(tower, RT::LookAt{RT::Part::Confirm, -1}, lever)) {
                UE_LOGW("[TOWER-DRILL] %s ABANDONED: the walker's or the confirm lever's place does not read",
                        ArmName());
                Done("ABANDONED");
                return;
            }
            g_walkAsked = true;
            const std::vector<FVector> ring = DR::ReachableStandpoints(player, lever, kRingCm, kStandpoints, kStandReachCm);
            if (ring.empty()) {
                if (!AfterWalk(tower, player, false)) return;
                Go(Step::Press);
                return;
            }
            float len = -1.f;
            if (!DR::RouteInLegs(player, me, ring[0], kStandReachCm, &len)) len = -1.f;
            g_routeUU = len;
            UE_LOGI("[TOWER-DRILL] %s walks to a standpoint %.0f uu from the confirm lever at (%.0f, %.0f, %.0f), "
                    "route %.0f uu", ArmName(), kRingCm, ring[0].X, ring[0].Y, ring[0].Z, len);
            g_walk = DR::StartBackgroundWalk(ring[0], kStandReachCm, kWalkDeadlineS);
            return;
        }
        if (!g_walk) return;
        const int state = g_walk->state.load();
        if (state == 0) return;
        g_walk.reset();
        if (!AfterWalk(tower, player, state == 1)) return;
        Go(Step::Press);
        return;
    }
    case Step::Press: {
        if (!haveL) return;
        if (g_pressNext < g_presses.size()) {
            if (!g_acted) {
                const uint64_t before = coop::fuse_tower_ops::OpsSent();
                RT::Use(tower, player, RT::LookAt{RT::Part::Button, g_presses[g_pressNext]});
                if (!IsRed() && coop::fuse_tower_ops::OpsSent() == before) {
                    Fail("a puzzle press was not sent to the host");
                    return;
                }
                g_actSeq = coop::fuse_tower_ops::LastSentSeq();
                g_acted = true;
                g_stepMs = NowMs();
                return;
            }
            if (IsRed() || Answered()) {
                ++g_pressNext;
                g_acted = false;
                g_stepMs = NowMs();
            } else if (Overdue(kCrossMs)) {
                Fail("the host never answered a press");
            }
            return;
        }
        // Every press answered: the host's rows must have lit this copy's puzzle before the lever is pulled.
        if (!IsRed() && !AllLit(l)) {
            if (Overdue(kCrossMs)) Fail("the host's presses never lit this copy's puzzle");
            return;
        }
        UE_LOGI("[TOWER-DRILL] client pressed %zu puzzle buttons, this copy's cells read %s", g_presses.size(),
                Digits(l, false).c_str());
        Go(Step::Lever);
        return;
    }
    case Step::Lever: {
        if (!haveL) return;
        if (!g_acted) {
            const uint64_t before = coop::fuse_tower_ops::OpsSent();
            RT::Use(tower, player, RT::LookAt{RT::Part::Confirm, -1});
            if (!IsRed() && coop::fuse_tower_ops::OpsSent() == before) {
                Fail("the confirm lever's press was not sent to the host");
                return;
            }
            g_actSeq = coop::fuse_tower_ops::LastSentSeq();
            g_acted = true;
            g_leverMs = NowMs();
            UE_LOGI("[TOWER-DRILL] client pulled the confirm lever (seq %u)", g_actSeq);
            return;
        }
        const bool copyWhole = !l.isBroken;
        const bool hostWhole = haveW && !RowBroken(w);
        if (copyWhole && hostWhole) {
            UE_LOGI("[TOWER-DRILL] radio PASS (presses=%zu, route=%.0f)", g_presses.size(), g_routeUU);
            Done("PASS");
            return;
        }
        if (NowMs() - g_leverMs > kVerdictMs) {
            Fail(!hostWhole ? "the host's tower stays broken" : "this copy still reads broken");
        }
        return;
    }
    case Step::Done:
        return;
    }
}

}  // namespace

namespace detail {

void RadioHostTick(coop::net::Session& s) { HostTick(s); }

void RadioClientTick() {
    if (g_step == Step::Done) return;
    if (void* player = coop::players::Registry::Get().Local()) ClientTick(player);
}

void RadioReset() {
    g_hostSaid = g_hostBroken = g_hostBroke = g_hostGaveUp = false;
    g_hostScrambles = 0;
    g_hostCallMs = 0;
    if (g_walk) DR::CancelBackgroundWalk(*g_walk);
    g_walk.reset();
    g_walkAsked = false;
    g_routeUU = -1.f;
    g_presses.clear();
    g_pressNext = 0;
    g_step = Step::Arm;
    g_stepMs = 0;
    g_acted = false;
    g_actSeq = 0;
    g_leverMs = 0;
}

}  // namespace detail
}  // namespace coop::dev::tower_drill
