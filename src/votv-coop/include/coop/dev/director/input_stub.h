// coop/dev/director/input_stub.h -- a key at the human-input seam: mainPlayer's input-action event stub, resolved once
// by its generated name and called with its zeroed frame. The director's processes and the drills that press a key
// themselves share this one. DEV-ONLY, like the rest of the director; game thread.

#pragma once

namespace coop::director {

// The stub's generated name (a game update renames the K2Node stubs), its function once resolved, and whether the
// resolution was tried.
struct InputStub {
    const wchar_t* name;
    void* fn = nullptr;
    bool  tried = false;
};

// Resolved once (said once when it does not resolve) and called with a zeroed frame (the Key it is given is not
// read). False when it did not resolve or the call failed.
bool PressInputEvent(void* player, InputStub& stub);

}  // namespace coop::director
