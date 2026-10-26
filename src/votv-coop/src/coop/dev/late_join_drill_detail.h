// coop/dev/late_join_drill_detail.h -- what the late-join drill's two files share: the host's staging entry point and
// the name of the staged drive's row, which the client's look for the staged world reads too. Impl-private: nothing
// outside coop/dev includes it. Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::late_join_drill::detail {

// The row name the host writes into the staged drive. A save's own drives carry other names, so a drive with this
// one on a client is the staged drive and no other.
inline constexpr const wchar_t* kDriveName = L"LATE-JOIN-DRIVE";

// HOST: one tick of the staging leg (late_join_drill_host.cpp).
void HostTick(coop::net::Session& session);

}  // namespace coop::dev::late_join_drill::detail
