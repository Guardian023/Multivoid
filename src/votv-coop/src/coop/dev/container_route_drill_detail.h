// coop/dev/container_route_drill_detail.h -- what the census drill's three files share. A private header of
// coop/dev/container_route_drill*.cpp (src tree, not include/).

#pragma once

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::dev::container_route_drill::detail {

inline constexpr const wchar_t* kPackPath   = L"/Game/objects/prop_backpack.prop_backpack_C";
inline constexpr const wchar_t* kPackClass  = L"prop_backpack_C";
inline constexpr const wchar_t* kDriveClass = L"prop_drive_C";
inline constexpr int      kItems        = 2;      // the records the fixture holds
inline constexpr float    kAheadCm      = 100.f;  // the fixture before the puppet
inline constexpr float    kSideAheadCm  = 60.f;   // the loose drives, to either side of it
inline constexpr float    kSideCm       = 80.f;
inline constexpr float    kDropCm       = 30.f;
inline constexpr float    kNearCm       = 400.f;  // CLIENT: the fixtures' mirrors are this near its player
inline constexpr uint64_t kCheckEveryMs = 250;

// The lines the peers say to each other (drill_chat).
inline constexpr const char* kLineRoutesDone    = "ci-census client routes-done";
inline constexpr const char* kLineR4Done        = "ci-census client r4-done";
inline constexpr const char* kLineBeforeReady   = "ci-census client before-ready";
inline constexpr const char* kLineBeforeWritten = "ci-census host before-written";
inline constexpr const char* kPrefixClientDone  = "ci-census client done ";   // + "rb=<0|1> ic=<0|1>"
inline constexpr const char* kPrefixHostDone    = "ci-census host done ";     // + "ih=<0|1>"

// Each side's steps (container_route_drill_host.cpp, container_route_drill_client.cpp).
void HostTick(coop::net::Session* s);
void ClientTick(coop::net::Session* s);
void HostReset();
void ClientReset();

// The element id of an actor, or kInvalidId.
uint32_t EidOf(void* actor);

// The record count of the world container `eid`, or -1 when it does not read.
int32_t RecordCount(uint32_t eid);

// Says the lane's hash of each record of the container, in order, then the whole slice (role "host" / "client").
void LogHashes(const char* role, uint32_t eid, const char* what);

// Says the encoded size of each record the container actor holds, and keeps the largest.
void LogSizes(const char* role, const char* what, void* container);
void LogLargest(const char* role);

// Prints a line of the drill (every one carries the tag the rig reads).
void Abandon(const char* role, const char* why);

// The screen the gamemode holds for a container view (ui_playerInventory_C), its container slot widgets, and a
// no-argument call of a function on an object. Null / 0 / false when they do not resolve.
void* Screen();
int SlotCount(void* screen);
void* SlotAt(void* screen, int i);
bool CallNoArg(void* obj, const wchar_t* fn);

// The integer after "key=" in a chat line's text, or false when the key is absent.
bool IntAfter(const std::string& text, const char* key, int& out);

// Live, non-default instances of prop_drive_C on this peer.
int LiveDrives();

}  // namespace coop::dev::container_route_drill::detail
