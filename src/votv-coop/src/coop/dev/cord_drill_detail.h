// coop/dev/cord_drill_detail.h -- what the cord drill's files share: its bounds, the cast (makers and sockets as this
// peer holds them), the acting player's hand, and the chat edges. Impl-private: nothing outside coop/dev includes it.
// Game thread.

#pragma once

#include "coop/dev/director/aim_fan.h"
#include "coop/dev/director/director.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/types.h"

#include <cstdint>
#include <memory>
#include <string>

namespace coop::net { class Session; }

namespace coop::dev::cord_drill::detail {

// ---- bounds: failure bounds of the instrument, never a wait for a milestone ----

// An arm not closed this long after its opening edge: the peer that waited prints FAIL.
inline constexpr uint64_t kArmFailMs = 60000;
// The walk to the first stand and the cast's arrival, which the director's own deadline bounds (+ a margin).
inline constexpr int      kWalkDeadlineS = 300;
inline constexpr uint64_t kStageFailMs = (static_cast<uint64_t>(kWalkDeadlineS) + 30) * 1000;

// ---- the geometry of the recipe (pr50cordm drill10) ----

inline constexpr float kStandReachCm = 60.f;   // the director's reach to a stand point
inline constexpr float kStandOutCm = 150.f;    // the stand point: this far out along the socket's plug arrow
inline constexpr float kSideCm = 80.f;         // a maker's spot: this far to the side of the stand point
inline constexpr float kClusterCm = 200.f;     // sockets 2 and 3 stand within this of socket 1's stand point
inline constexpr float kMakerRiseCm = 700.f;   // arm 6: the scripted move of maker A

inline constexpr int kMakers = 2;   // A, B
inline constexpr int kSocks = 3;    // 1, 2, 3
inline constexpr int kArms = 6;

// What both peers' copies must show when arm n has closed: the socket each maker is in (0 none), and what the
// `sees` line says. The joiner's expectation is arm 4's end: A in 1, B unplugged.
struct Expect {
    int a, b;
    const char* sees;
};
inline constexpr Expect kExpect[kArms + 1] = {
    {0, 0, ""},        {1, 0, "A in 1"},       {1, 2, "B in 2"},      {1, 0, "B unplugged"},
    {1, 0, "A in 1"},  {1, 2, "B in 2"},       {0, 2, "A unplugged"},
};
inline constexpr Expect kJoinerExpect = {1, 0, "A in 1, B unplugged"};

// ---- the cast (cord_drill_cast.cpp) ----

struct Cast {
    ue_wrap::CachedObjRef maker[kMakers];
    ue_wrap::CachedObjRef sock[kSocks];
    ue_wrap::FVector makerAt[kMakers]{};   // the host's spawn spots
    std::wstring makerKey[kMakers], sockKey[kSocks];
};
Cast& TheCast();
void ResetCast();

void* Maker(int m);          // live corded prop or null
void* CordOfMaker(int m);    // live cord or null
void* Sock(int k);           // live socket or null
// The socket maker m is in as 1..3, 0 unplugged, 9 a socket outside the cast, -1 when the maker is not here.
int LinkOf(int m);
bool Matches(const Expect& e);
// "A in 1, B unplugged" -- this peer's copies, for a line.
std::string LinkText();

// A component of an actor by its property name, null when it is not there (reflection; the recipe's reads).
void* FieldOf(void* obj, const wchar_t* name);
// A free socket's world location and its plug arrow's forward; false when either does not read.
bool SocketGeometry(void* socket, ue_wrap::FVector& at, ue_wrap::FVector& fwd);
// The stand point of a socket: 150 uu out along its plug arrow, at the socket's height.
bool StandOf(void* socket, ue_wrap::FVector& stand);

// ---- the acting player's hand (cord_drill_hand.cpp) ----

// One piece of cord work, a step per tick: walk to a stand by the director; press through the cord's own
// actionOptionIndex (the function the E press ends in); hold, or aim at a socket's Sphere (sock >= 0) or into the air
// (sock < 0) and release with the use key's stub.
class Hand {
public:
    enum class Mode : uint8_t { WalkOnly, Plug, Grab };
    enum class St : uint8_t { Idle, Walk, Press, Held, Aim, Release, Done, Stuck };
    struct Plan {
        Mode mode = Mode::WalkOnly;
        const char* who = "";            // the peer's name, for the lines
        void* cord = nullptr;            // Plug, Grab
        ue_wrap::FVector stand{};        // walked to when `walk`
        bool walk = false;
        int sock = -1;                   // Plug: the socket 0..2
    };
    void Start(const Plan& plan);
    St Tick(void* player);
    // From Held: aim at socket `sock` (0..2) or into the air (-1), and release.
    void Resume(int sock);
    // The walk, when one is in flight, ends.
    void Cancel();
    const char* stuckWhat() const { return stuckWhat_; }

private:
    void Next(St s);
    St Stick(const char* at, const char* what);
    St TickAim(void* player);
    Plan plan_{};
    St st_ = St::Idle;
    std::shared_ptr<coop::director::BackgroundWalk> walk_;
    coop::director::AimFan fan_;
    int airTicks_ = 0;
    const char* stuckWhat_ = "";
};

// ---- the chat edges (cord_drill.cpp) ----

// A line both peers watch for, latched on sight (a live row retires after a few more lines). Heard is true once the
// row was seen live; Rest is the text after a prefix, from the newest live row that carried it.
bool Heard(const std::string& line);
bool Rest(const std::string& prefix, std::string* rest);
void Say(const std::string& line);
// The lines' names, so both roles say and watch the same words.
std::string ArmLine(int n);            // host: arm n opens
std::string HostSeenLine(int n);       // host: its copies show arm n's links
std::string ClientSeenLine(int n);     // client: the same
// A narrow key for a line and back (keys are ASCII).
std::string Narrow(const std::wstring& w);
std::wstring Wide(const std::string& s);
// The text after `tag` up to the next space, from a line's rest.
std::string Token(const std::string& rest, const char* tag);

// A failure line, once; `arm` names the phase. Prints this peer's links.
void FailLine(const char* peer, const char* arm, const char* why);

uint64_t NowMs();

// ---- the roles (cord_drill_host.cpp, cord_drill_client.cpp) ----

void HostTick(coop::net::Session* s);
// The session's end on the host. True when the host is past its six arms and the client has left: the cast and the
// phase stay for the rejoin (the host's session lives on); otherwise the drill starts over.
bool HostOnDisconnect();
void ClientTick(coop::net::Session* s);
void ClientReset();

}  // namespace coop::dev::cord_drill::detail
