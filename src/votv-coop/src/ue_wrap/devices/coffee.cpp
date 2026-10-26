// ue_wrap/devices/coffee.cpp -- see ue_wrap/devices/coffee.h.

#include "ue_wrap/devices/coffee.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

namespace ue_wrap::coffee {
namespace {

namespace R = reflection;

// A class as this world holds it, looked up by name again once its slot or serial no longer holds the
// kept one (a Blueprint class can be a new object in a new world).
struct Cls {
    const wchar_t* name;
    ue_wrap::CachedObjRef ref;
};
Cls g_vending{L"prop_coffeevending_C", {}};
Cls g_maker{L"prop_coffeeMaker_C", {}};

void* ClassFor(Cls& c) {
    if (void* k = c.ref.Get()) return k;
    void* k = ue_wrap::object_index::ClassByName(c.name);
    if (k) c.ref.Set(k);
    return k;
}

}  // namespace

Device DeviceOf(void* actor) {
    void* cls = actor ? R::ClassOf(actor) : nullptr;
    if (!cls) return Device::None;
    if (cls == ClassFor(g_vending)) return Device::Vending;
    if (cls == ClassFor(g_maker)) return Device::Maker;
    return Device::None;
}

}  // namespace ue_wrap::coffee
