// coop/world/fuse_tower_kind.cpp -- see coop/world/fuse_tower_kind.h.

#include "coop/world/fuse_tower_kind.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/coord_tower.h"
#include "ue_wrap/world/game_rules.h"  // the difficulty an insert reads

#include <algorithm>
#include <cstdint>

namespace coop::fuse_tower_kind {
namespace {

namespace CT = ue_wrap::coord_tower;
using coop::net::FuseTowerRow;
using Edges = coop::fuse_tower_rows::Edges;

static_assert(CT::kMaxFuses <= 8 && CT::kMaxLights <= 16, "a row holds eight fuses and sixteen lights");

// The insert's own branch on the difficulty (coordRadarDish playerUsedOn @4823): on the two easiest, the
// puzzle is solved with it.
bool InsertSolves() {
    ue_wrap::game_rules::Snapshot rules;
    if (!ue_wrap::game_rules::ReadLocal(rules)) return false;
    for (const auto& f : rules.fields)
        if (f.key == "difficulty") return f.ival <= 1;
    return false;
}

CT::LookAt CoordPart(uint8_t op, int32_t index) {
    switch (op) {
    case coop::net::kFuseTowerOpButton:  return {CT::Part::Button, index};
    case coop::net::kFuseTowerOpLever:   return {CT::Part::Lever, -1};
    case coop::net::kFuseTowerOpRetract: return {CT::Part::Retract, -1};
    case coop::net::kFuseTowerOpPull:
    case coop::net::kFuseTowerOpInsert:  return {CT::Part::Fuse, index};
    default:                              return {CT::Part::None, -1};
    }
}

bool CoordReadRow(void* tower, FuseTowerRow& w) {
    CT::State st;
    if (!CT::Read(tower, st)) return false;
    w = FuseTowerRow{};
    w.id = st.id;
    w.flags = static_cast<uint8_t>((st.isBroken ? coop::net::kFuseTowerBroken : 0) |
                                   (st.opened ? coop::net::kFuseTowerOpened : 0) |
                                   (st.isAnim ? coop::net::kFuseTowerAnim : 0) |
                                   (st.leverMoving ? coop::net::kFuseTowerLeverMoving : 0) |
                                   (st.leverUp ? coop::net::kFuseTowerLeverUp : 0));
    w.fuseCount = st.fuseCount;
    for (int i = 0; i < st.fuseCount; ++i) w.fuses[i] = st.fuses[i];
    w.lightCount = st.lightCount;
    for (int i = 0; i < st.lightCount; ++i)
        if (st.lights[i]) w.lights = static_cast<uint16_t>(w.lights | (1u << i));
    return true;
}

void CoordApply(void* tower, const FuseTowerRow& c, bool live, Edges& edges, bool& sizeWarned) {
    CT::State l;
    if (!CT::Read(tower, l)) return;
    const bool broken = (c.flags & coop::net::kFuseTowerBroken) != 0;
    bool lights[CT::kMaxLights] = {};
    bool lightsDiffer = false;
    if (c.lightCount == l.lightCount) {
        for (int i = 0; i < c.lightCount; ++i) {
            lights[i] = ((c.lights >> i) & 1u) != 0;
            lightsDiffer = lightsDiffer || lights[i] != l.lights[i];
        }
    } else if (!sizeWarned) {
        // Every peer plays under the host's rules (ue_wrap/world/game_rules.h), which size the puzzle.
        sizeWarned = true;
        UE_LOGW("fuse_tower: tower %d's puzzle has %u lights here and %u on the host -- its lights are not applied",
                c.id, l.lightCount, c.lightCount);
    }
    if (lightsDiffer) CT::WriteLights(tower, lights, c.lightCount);
    if (l.isBroken && !broken) {
        CT::SolvePuzzle(tower);  // the repair: every light lit, painted, its success sound, isBroken off, painted
        ++edges.repairs;
        UE_LOGI("fuse_tower: tower %d repaired as the host's", c.id);
    } else if (!l.isBroken && broken) {
        CT::WriteBroken(tower, true);
        CT::UpdPuzzle(tower);
        CT::UpdBroken(tower);
        ++edges.breaks;
        UE_LOGI("fuse_tower: tower %d broke as the host's", c.id);
    } else if (lightsDiffer) {
        CT::UpdPuzzle(tower);
        if (live) CT::Play(tower, CT::Sound::Click);  // a press, the host's route's own click
        ++edges.presses;
    }
    bool fusesDiffer = false, pulled = false, inserted = false;
    if (c.fuseCount == l.fuseCount) {
        for (int i = 0; i < c.fuseCount; ++i) {
            if (c.fuses[i] == l.fuses[i]) continue;
            pulled = pulled || (l.fuses[i] == 2 && c.fuses[i] == 0);
            inserted = inserted || (l.fuses[i] == 0 && c.fuses[i] == 1);
            CT::WriteFuse(tower, i, c.fuses[i]);
            fusesDiffer = true;
        }
    }
    if (fusesDiffer) {
        CT::UpdFuses(tower);
        if (live && pulled) CT::Play(tower, CT::Sound::FusePulled);
        if (live && inserted) CT::Play(tower, CT::Sound::FuseInserted);
        edges.pulls += pulled ? 1 : 0;
        edges.inserts += inserted ? 1 : 0;
    }
    const bool up = (c.flags & coop::net::kFuseTowerLeverUp) != 0;
    if (up != l.leverUp) {
        // A lever that goes down on a tower broken before and after is the judgement's fail branch: its other
        // caller, a scramble, breaks a whole tower (Scramble Radar Dish's end, and loadData, before any join).
        if (!up && l.isBroken && broken) {
            if (live) CT::Play(tower, CT::Sound::Fail);
            ++edges.fails;
        }
        CT::MoveLever(tower, up);
        ++edges.levers;
    }
    // The lever's end, which judges the puzzle, is refused here, and it is what clears the flag: the host's.
    CT::WriteLeverMoving(tower, (c.flags & coop::net::kFuseTowerLeverMoving) != 0);
}

}  // namespace

bool Resolved(Kind) { return CT::EnsureResolved(); }

int32_t ReadAll(Tower* out, int32_t cap) {
    void* towers[coop::net::kFuseTowers];
    const int32_t n = CT::ReadAll(towers, std::min<int32_t>(cap, coop::net::kFuseTowers));
    if (n < 0) return -1;
    for (int32_t i = 0; i < n; ++i) out[i] = {Kind::Coordinate, CT::IdOf(towers[i]), towers[i]};
    return n;
}

void* Find(Kind, int32_t id) {
    void* towers[coop::net::kFuseTowers];
    const int32_t n = CT::ReadAll(towers, coop::net::kFuseTowers);
    for (int32_t i = 0; i < n; ++i)
        if (CT::IdOf(towers[i]) == id) return towers[i];
    return nullptr;
}

bool Identify(void* obj, Kind& kind, int32_t& id) {
    const int32_t i = CT::IdOf(obj);
    if (i < 0) return false;
    kind = Kind::Coordinate;
    id = i;
    return true;
}

bool ReadView(Kind, void* tower, View& out) {
    CT::State st;
    if (!CT::Read(tower, st)) return false;
    out = View{};
    out.broken = st.isBroken;
    out.fuseCount = st.fuseCount;
    for (int i = 0; i < st.fuseCount; ++i) out.fuses[i] = st.fuses[i];
    return true;
}

bool ReadRow(const Tower& tower, FuseTowerRow& row) { return CoordReadRow(tower.obj, row); }

void ApplyRow(Kind, void* tower, const FuseTowerRow& row, bool live, Edges& edges, bool& sizeWarned) {
    CoordApply(tower, row, live, edges, sizeWarned);
}

bool ConvergePanel(Kind, void* tower, void* player, const FuseTowerRow& row, bool& opens, bool& ran) {
    CT::State l;
    if (!CT::Read(tower, l) || l.isAnim) return false;
    const bool open = (row.flags & coop::net::kFuseTowerOpened) != 0;
    const bool target = (row.flags & coop::net::kFuseTowerAnim) ? !open : open;
    if (l.opened == target) return false;
    opens = target;
    ran = CT::Use(tower, player, CT::LookAt{CT::Part::Retract, -1});
    return true;
}

bool PartLocation(Kind, void* tower, uint8_t op, int32_t index, ue_wrap::FVector& out) {
    return CT::PartLocation(tower, CoordPart(op, index), out);
}

bool Use(Kind, void* tower, void* player, uint8_t op, int32_t index) {
    return CT::Use(tower, player, CoordPart(op, index));
}

Look ReadUse(Kind, void* tower, uint8_t& op, int32_t& index) {
    CT::LookAt at;
    if (!CT::ReadLookAt(tower, at)) return Look::None;
    switch (at.part) {
    case CT::Part::Button:
        op = coop::net::kFuseTowerOpButton;
        index = at.index;
        return Look::Send;
    case CT::Part::Lever:
        op = coop::net::kFuseTowerOpLever;
        index = -1;
        return Look::Send;
    case CT::Part::Retract:
        op = coop::net::kFuseTowerOpRetract;
        index = -1;
        return Look::Send;
    case CT::Part::Fuse: {
        CT::State st;
        if (!CT::Read(tower, st) || at.index < 0 || at.index >= st.fuseCount || st.fuses[at.index] != 2)
            return Look::None;
        op = coop::net::kFuseTowerOpPull;
        index = at.index;
        return Look::Pull;
    }
    default:
        return Look::None;
    }
}

bool ReadFuseLook(Kind, void* tower, int32_t& slot) { return CT::ReadFuseLook(tower, slot); }

const char* CanPull(Kind, void* tower, const View& view, int32_t slot) {
    // The pull's own gates (coordRadarDish actionOptionIndex @3586-@3782), on the host's copy.
    CT::State st;
    if (!CT::Read(tower, st)) return "the tower does not read";
    if (!st.opened) return "its panel is closed";
    if (st.isAnim) return "its panel is moving";
    if (st.leverMoving) return "its lever is moving";
    if (slot < 0 || slot >= view.fuseCount || view.fuses[slot] != 2) return "that fuse is not blown on the host";
    return nullptr;
}

const char* CanInsert(Kind, const View& view, int32_t slot) {
    if (slot < 0 || slot >= view.fuseCount || view.fuses[slot] != 0) return "that fuse slot is not empty on the host";
    return nullptr;
}

void TakePull(Kind, void* tower, int32_t slot) {
    // The pull's writes on the host's copy; its fuse is already in its puller's hand.
    CT::WriteFuse(tower, slot, 0);
    CT::UpdFuses(tower);
    CT::Play(tower, CT::Sound::FusePulled);
}

void TakeInsert(Kind, void* tower, int32_t slot) {
    // The insert's writes (playerUsedOn @4823..@5317), on the host's copy; its fuse is already spent.
    if (InsertSolves()) CT::SolvePuzzle(tower);
    CT::WriteFuse(tower, slot, 1);
    CT::Play(tower, CT::Sound::FuseInserted);
    CT::UpdFuses(tower);
}

bool IsFuse(void* actor) { return CT::IsFuse(actor); }
void* SpawnFuse(const ue_wrap::FVector& at) { return CT::SpawnFuse(at); }
bool IsPulledFuse(void* actor) { return CT::IsPulledFuse(actor); }

}  // namespace coop::fuse_tower_kind
