// ue_wrap/actors/geom_octa.h -- engine access for the Geom Octahedron (geomOcta_C) and its laser (geomOctaLazer_C):
// reflected reads of the members the coop layer judges them by. Engine-wrapper layer (principle 7): no gameplay,
// network or coop state. Game thread. Implementation: src/ue_wrap/actors/geom_octa.cpp.

#pragma once

namespace ue_wrap::geom_octa {

// The Octa's `targetActor` (the actor it is locked onto, set by its aggro and cleared when it closes), when that
// object is live; null for no target, a dead target, or an actor that is no Octa. Game thread.
void* TargetActor(void* octa);

// The Octa's `isAnim` (true from its aggro until its close finishes); false when the member does not resolve. Game
// thread.
bool IsAnim(void* octa);

// The Octa's `minSphere` component (the sphere whose begin-overlap picks its target), when live; null otherwise. Game
// thread.
void* MinSphere(void* octa);

// The laser's owner (AActor::Owner, which the Octa's fire() sets to itself when it spawns the laser), when live; null
// otherwise. Game thread.
void* LaserOwner(void* laser);

}  // namespace ue_wrap::geom_octa
