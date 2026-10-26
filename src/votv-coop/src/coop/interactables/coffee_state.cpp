// coop/interactables/coffee_state.cpp -- see coop/interactables/coffee_state.h.

#include "coop/interactables/coffee_state.h"

#include "coop/net/session.h"
#include "coop/props/prop_record_gates.h"
#include "coop/props/prop_save_data.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/coffee.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace coop::coffee_state {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace CF = ue_wrap::coffee;

// The two bodies that change the maker's flags, declared on its class: the brew, and the hit handler that is the
// fill. Another class's function of either name is never asked.
UE_FACT_FN(kRowBrew, "coffee_state", L"prop_coffeeMaker_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowHit, "coffee_state", L"prop_coffeeMaker_C",
           L"BndEvt__StaticMesh_K2Node_ComponentBoundEvent_0_ComponentHitSignature__DelegateSignature", Declared);

constexpr int kTagBrew = 0x43534250;  // 'CSBP'
constexpr int kTagHit  = 0x43534854;  // 'CSHT'

constexpr uint64_t kRefusalSayMs = 10000;

std::atomic<coop::net::Session*> g_session{nullptr};
// Each watch registers once; the set settles once both are live, or once no name is left to resolve.
bool g_brewWatched = false, g_hitWatched = false, g_settled = false, g_gatesRegistered = false;

uint64_t g_cancelled = 0, g_madeMaker = 0, g_madeBag = 0, g_recordsRefused = 0, g_publishFailed = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

coop::net::Session* HostSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->role() == coop::net::Role::Host) ? s : nullptr;
}

// ---- the host's notes ------------------------------------------------------------------------
// What the maker (and a bag that touched it) read before a body ran, kept for the POST of the same call. Each
// body has its own note, and neither nests: the hit handler is one dispatch of the physics scene, and the brew is
// one press. A POST with no note of its own call publishes nothing.
struct Note {
    void*                 object = nullptr;
    CF::MakerFlags        flags;
    ue_wrap::CachedObjRef bag;  // the hit handler's other actor, when it is a coffee bag
    int32_t               amounts = 0;
};
Note g_brewNote, g_hitNote;

// Publishes `actor`'s record. A publish the transport refused is registered for a re-send by prop_save_data; one
// that could not be made at all (no peer ready, the capture failed) is counted and said.
void PublishRecord(coop::net::Session* s, void* actor, const char* what, uint64_t& made) {
    const std::wstring key = ue_wrap::prop::GetKeyString(actor);
    if (key.empty() || key == L"None" || !coop::prop_save_data::Publish(s, actor, key)) {
        ++g_publishFailed;
        UE_LOGW("[COFFEE-STATE] the %s's record on key='%ls' was not published (no peer ready, or the capture "
                "failed)", what, key.c_str());
        return;
    }
    ++made;
}

// ---- the brew --------------------------------------------------------------------------------
sg::Verdict OnBrewPre(const sg::Call& call) {
    coop::net::Session* s = HostSession();
    if (!s || !call.object) return sg::Verdict::Run;
    static R::ParamOffset s_action{L"action"};
    const int32_t off = s_action.Of(call.function);
    g_brewNote.object = nullptr;
    if (off < 0 || *(call.locals + off) != CF::kBrewAction) return sg::Verdict::Run;
    if (!CF::ReadMakerFlags(call.object, g_brewNote.flags)) return sg::Verdict::Run;
    g_brewNote.object = call.object;
    return sg::Verdict::Run;
}

void OnBrewPost(const sg::Call& call) {
    coop::net::Session* s = HostSession();
    if (!s || g_brewNote.object != call.object) return;
    g_brewNote.object = nullptr;
    CF::MakerFlags now;
    // A brew sets and clears the three flags inside the one call, so the record that crosses is the final one.
    if (CF::ReadMakerFlags(call.object, now) && !(now == g_brewNote.flags))
        PublishRecord(s, call.object, "maker", g_madeMaker);
}

// ---- the fill --------------------------------------------------------------------------------
sg::Verdict OnHitPre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !call.object) return sg::Verdict::Run;
    if (s->role() == coop::net::Role::Client) {
        // The fill is the host's: this copy's run would destroy its mirror of the cup and set flags no record
        // carries. The cup's destroy and the maker's record come from the host. Contact rate: counted, said for
        // the first three and every sixtieth.
        const uint64_t n = ++g_cancelled;
        if (n <= 3 || n % 60 == 0)
            UE_LOGI("[COFFEE-STATE] a client's hit on maker %p is cancelled -- the fill is the host's (#%llu)",
                    call.object, static_cast<unsigned long long>(n));
        return sg::Verdict::Cancel;
    }
    if (s->role() != coop::net::Role::Host) return sg::Verdict::Run;
    g_hitNote.object = nullptr;
    if (!CF::ReadMakerFlags(call.object, g_hitNote.flags)) return sg::Verdict::Run;
    g_hitNote.bag.Reset();
    static R::ParamOffset s_other{L"OtherActor"};
    const int32_t off = s_other.Of(call.function);
    void* other = off >= 0 ? *reinterpret_cast<void* const*>(call.locals + off) : nullptr;
    if (CF::IsBag(other) && CF::ReadBagAmounts(other, g_hitNote.amounts)) g_hitNote.bag.Set(other);
    g_hitNote.object = call.object;
    return sg::Verdict::Run;
}

void OnHitPost(const sg::Call& call) {
    coop::net::Session* s = HostSession();
    if (!s || g_hitNote.object != call.object) return;
    g_hitNote.object = nullptr;
    CF::MakerFlags now;
    if (CF::ReadMakerFlags(call.object, now) && !(now == g_hitNote.flags))
        PublishRecord(s, call.object, "maker", g_madeMaker);
    void* bag = g_hitNote.bag.Get();
    g_hitNote.bag.Reset();
    int32_t amounts = 0;
    if (bag && CF::ReadBagAmounts(bag, amounts) && amounts != g_hitNote.amounts)
        PublishRecord(s, bag, "bag", g_madeBag);
}

// ---- the record gate -------------------------------------------------------------------------
// The host takes a client's record for a maker or a bag only when it is the record behind that client's own drop
// (coop/props/prop_drop_intent awaits it); a forged flag or a refilled bag is any other record. A refusal is
// final for that record, so it is counted once.
bool AcceptsClientRecord(void* actor, uint8_t senderSlot) {
    const std::wstring key = ue_wrap::prop::GetKeyString(actor);
    if (!key.empty() && coop::prop_save_data::AwaitsRecordFrom(key, senderSlot)) return true;
    ++g_recordsRefused;
    static uint64_t s_nextSayMs = 0;
    static uint64_t s_since = 0;
    const uint64_t now = NowMs();
    if (now < s_nextSayMs) { ++s_since; return false; }
    UE_LOGW("[COFFEE-STATE] slot %u's record for key='%ls' is refused -- it is no drop's own (%llu more since "
            "the last line)", static_cast<unsigned>(senderSlot), key.c_str(),
            static_cast<unsigned long long>(s_since));
    s_nextSayMs = now + kRefusalSayMs;
    s_since = 0;
    return false;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_brewWatched) g_brewWatched = sg::WatchName(kRowBrew, kTagBrew, &OnBrewPre, &OnBrewPost);
    if (!g_hitWatched) g_hitWatched = sg::WatchName(kRowHit, kTagHit, &OnHitPre, &OnHitPost);
    if (!g_gatesRegistered) {
        g_gatesRegistered = true;
        coop::prop_record_gates::RegisterIntentGate(L"prop_coffeeMaker_C", &AcceptsClientRecord);
        coop::prop_record_gates::RegisterIntentGate(L"prop_coffeeBag_C", &AcceptsClientRecord);
    }
}

void Tick() {
    sg::ResolvePendingNames();
    if (g_settled || !g_brewWatched || !g_hitWatched) return;
    if (sg::NameWatchLive(kRowBrew, kTagBrew) && sg::NameWatchLive(kRowHit, kTagHit)) {
        g_settled = true;
        UE_LOGI("[COFFEE-STATE] the maker's brew and fill gates are live");
    } else if (sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("[COFFEE-STATE] a maker gate is dead (resolved into a full table, or its class declares no such "
                "body) -- the maker's state crosses only where a press was made");
    }
}

void OnDisconnect() {
    if (g_cancelled || g_madeMaker || g_madeBag || g_recordsRefused || g_publishFailed)
        UE_LOGI("[COFFEE-STATE] session end -- client hits cancelled=%llu maker records=%llu bag records=%llu "
                "client records refused=%llu publishes failed=%llu",
                static_cast<unsigned long long>(g_cancelled), static_cast<unsigned long long>(g_madeMaker),
                static_cast<unsigned long long>(g_madeBag), static_cast<unsigned long long>(g_recordsRefused),
                static_cast<unsigned long long>(g_publishFailed));
    g_cancelled = g_madeMaker = g_madeBag = g_recordsRefused = g_publishFailed = 0;
    g_brewNote = Note{};
    g_hitNote = Note{};
}

}  // namespace coop::coffee_state
