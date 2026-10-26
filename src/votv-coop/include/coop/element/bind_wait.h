// coop/element/bind_wait.h -- the one hold for a message whose prop has no actor here yet.
//
// A bind can be deferred: a save-time pile whose twin has not loaded, a kerfur prop awaiting its local
// twin, a mirror the create path refused. A wire message for that eid (a drive end, a pose) that lands in
// the gap used to be parked by each module in a map of its own, each with its own clock. This module is
// the single owner: the deferring bind MARKS the eid, a receiver HOLDS its message against it, and the
// bind funnel's OnBound moves the holds to a due list that Pump replays in arrival order, from the
// reliable drain. A bind that gives up (or a destroy of the eid, a slot that left, a world change)
// DROPS the holds with a named reason. A message for an eid that was never marked is not held: it is
// counted (NoteNeverBound) and dropped by the caller. Game thread only, no mutex.
//
// Bounds: 64 holds per eid and 1 MiB of held bytes in all; past either the new hold is refused, with one
// warning per eid. A replay that holds again lands in the next Pump, never inside the one running.

#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::element::bind_wait {

// A held message's replay: the bytes handed to Hold, the slot that sent them.
using Replay = void (*)(const uint8_t* bytes, size_t n, int senderSlot);

// The eid's bind was deferred (or refused): later messages for it wait. Idempotent. `why` names the path.
void MarkDeferred(uint32_t eid, const char* why);
bool IsDeferred(uint32_t eid);

// Keep a copy of the message until the eid binds. False when the eid is not deferred (or the red row is
// on): the caller then owns the message. An unmarked eid whose registry row holds a purged actor is marked
// here, "a row awaiting its re-bind": the game's re-created native binds onto it and frees the mark. True when stored, and also when refused by a bound (warned).
// `coalesce`: an earlier hold of the same `kind` for the eid is replaced in place, latest wins.
bool Hold(uint32_t eid, uint16_t kind, int senderSlot, Replay fn, const void* bytes, size_t n, bool coalesce);

// A bind took: the eid is no longer deferred and its holds become due.
void OnBound(uint32_t eid);

// Run every due hold, in arrival order. O(1) when none is due. Called from the reliable drain. A GiveUp of
// an eid, or a slot that left, while the pass runs voids the holds of it the pass has not yet reached.
void Pump();

// The bind will not come: drop the eid's mark and holds. A no-op for an eid neither deferred nor holding.
void GiveUp(uint32_t eid, const char* why);

// A slot left: drop the holds it sent.
void OnSlotLeft(int slot);

// Disconnect or world change: drop every hold and mark.
void Clear(const char* why);

// True while any hold is stored or due: the dev selftest runs only once none is.
bool HasHolds();

// A message named an eid that was never deferred, so nothing waits for it: counted and warned.
void NoteNeverBound(uint16_t kind, uint32_t eid);

// For the drills: the totals of this process.
uint32_t DropsNeverBound();
uint32_t DeferredCount();
uint32_t HeldCount();
uint32_t ReplayedCount();
uint32_t GivenUpCount();

}  // namespace coop::element::bind_wait
