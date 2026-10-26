// ue_wrap/devices/coffee.h -- reflected access to VOTV's two coffee devices, the vending machine
// (prop_coffeevending_C) and the coffee maker (prop_coffeeMaker_C). Which class an actor is, and nothing
// the lanes decide; no network or gameplay logic (principle 7). Game thread.
#pragma once

#include <cstdint>

namespace ue_wrap::coffee {

enum class Device : uint8_t { None, Vending, Maker };

// The action both devices brew or dispense on: their actionOptionIndex(player, action) takes the points
// (the vending machine) or starts the brew (the maker) at this value, and every other value is a look-at
// option the player's aim reads.
inline constexpr uint8_t kBrewAction = 4;

// The device an actor is: its class IS prop_coffeevending_C or prop_coffeeMaker_C, exactly. A subclass,
// another prop, null and a class not loaded yet answer None.
Device DeviceOf(void* actor);

}  // namespace ue_wrap::coffee
