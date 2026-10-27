// coop/dev/grid_drill_upgrade.h -- [dev] the grid drill's upgrade arms, beside grid_drill.cpp. The client walks
// beside the drill's generator (grid_drill_walk.h). An install op with no insert behind it must be refused, the host
// refunding nothing for it; then the client installs the upgrades the host hands it: the first must be taken, the
// second, while the host refuses installs as if another player's had filled the generator first, must roll back
// and come back as a refund beside it. upgradered is the control, a host that judges an install as if its spend
// were recorded, which takes the bare op. Src-local.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::grid_drill {

// Either peer's legs, per frame once the grid resolved. Game thread.
void UpgradeTick(coop::net::Session* session);

// The legs start again. Session end.
void UpgradeOnDisconnect();

}  // namespace coop::dev::grid_drill
