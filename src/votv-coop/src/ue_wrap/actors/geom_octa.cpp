// ue_wrap/actors/geom_octa.cpp -- see ue_wrap/actors/geom_octa.h.

#include "ue_wrap/actors/geom_octa.h"

#include "ue_wrap/core/reflection.h"

#include <cstddef>
#include <cstdint>

namespace ue_wrap::geom_octa {
namespace {

namespace R = reflection;

R::InstanceOffset g_target{L"targetActor"};
R::InstanceOffset g_owner{L"Owner"};

// The live object in the pointer slot `off` bytes into `base`, or null.
void* LivePointerAt(void* base, int32_t off) {
    if (!base || off < 0) return nullptr;
    void* p = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(base) + static_cast<size_t>(off));
    return (p && R::IsLive(p)) ? p : nullptr;
}

}  // namespace

void* TargetActor(void* octa) { return octa ? LivePointerAt(octa, g_target.Of(octa)) : nullptr; }

void* LaserOwner(void* laser) { return laser ? LivePointerAt(laser, g_owner.Of(laser)) : nullptr; }

}  // namespace ue_wrap::geom_octa
