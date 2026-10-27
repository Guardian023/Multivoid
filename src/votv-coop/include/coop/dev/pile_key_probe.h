// coop/dev/pile_key_probe.h -- [dev] [MEASURE sync-order-wp5, temporary] why a joiner's save-loaded pile twin
// misses. The host's key for a pile is its place at the join capture; the joiner finds the pile's native by that
// key. This probe logs, on both peers, what separates the key from the native, and changes nothing (ini
// pile_key_probe=1 / env VOTVCOOP_PILE_KEY_PROBE; a latched read when off).
//
// HOST: at the join capture, each chipPile moving faster than 0.5 cm/s ("not at rest at serialize"), then a count.
// JOINER: the first-seen place of each native chipPile; at the bind's miss, at sweep passes 1, 10, 20, 30 and 39,
// and at the give-up, one PILE-KEY line per pending eid: the nearest unbound native of its form and
// chip type, its distance from the key now (live), at first sight (first-seen), and the drift between; and
// "adopted at pass N" for a twin adopted after a miss was logged.
// Game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstddef>
#include <cstdint>

namespace coop::dev::pile_key_probe {

// A native trash actor as a caller read it: its place at the time of the read.
struct Native {
    void*   actor;
    int32_t idx;
    float   x, y, z;
    uint8_t chipType;
    bool    isClump;
};

// The row, read once.
bool On();

// ---- host ----
void BeginHostCapture();
void NoteHostPile(uint32_t eid, void* actor, const ue_wrap::FVector& at);
void EndHostCapture();

// ---- joiner ----
// The world-ready index build: record each native's first-seen place.
void NoteBindIndex(const Native* natives, size_t count);
// A sweep pass's fresh walk of the unbound natives: record the ones not yet seen, and keep the walk as the
// candidate set for the pass's lines.
void NoteSweepNatives(const Native* natives, size_t count);

// The bind's miss: `unbound` is the bind index's current contents.
void LogBindMiss(uint32_t eid, const ue_wrap::FVector& key, bool wantClump, uint8_t chipType,
                 const Native* unbound, size_t count);
// A pending twin at the sweep pass `pass` (1-based); logs on passes 1, 10, 20, 30 and 39 only.
void LogPass(uint32_t eid, int pass, const ue_wrap::FVector& key, bool wantClump, uint8_t chipType);
// The give-up of an unbound eid.
void LogGaveUp(uint32_t eid, const ue_wrap::FVector& key, bool wantClump, uint8_t chipType);
// A twin adopted by the sweep at `pass`; says so only for an eid whose miss was logged.
void NoteAdopted(uint32_t eid, int pass);

// ---- the turn of a trash element, so a drill's "turn off" names its cause ----
// HOST, the join snapshot's row of `eid` (a trash element): the root's rotation it sends and the child mesh's
// turn (the row's look yaw, degrees). Once per eid.
void LogRowRot(uint32_t eid, const ue_wrap::FRotator& root, float lookYawDeg);
// HOST, the drill's read of `eid`, which failed on turn: the root's rotation now.
void LogHostNowRot(uint32_t eid, float pitch, float yaw, float roll);
// HOST, the land-settle commit of `eid`: the settled pile's root, the rotation the convert carries, and whether
// that rotation came from the root (read) or is the clump's fallback. Nothing writes the root here.
void LogLandRot(uint32_t eid, const ue_wrap::FRotator& settledRoot, const ue_wrap::FRotator& convertRot,
                bool fromRoot);

// The session ended: the first-seen records belong to one world.
void OnDisconnect();

}  // namespace coop::dev::pile_key_probe
