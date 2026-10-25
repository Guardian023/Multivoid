// coop/config/config_registry_rows_dev_octa.inc -- the [dev] rows of the Geom Octa lane: its drill and its red arm.
// Included by config_registry_rows_dev.inc after the explosion rows, before the PI and NB rows, which stay the last
// ones, so each row keeps its place in every expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_ENUM(octa_red, "octa_red", "dev", "off", "off|no_parking", "VOTVCOOP_OCTA_RED",
    "Developer red arm: never set. no_parking (CLIENT): the Octa's graph watch is left unregistered, so its mirror "
    "runs its own wander and the Octa drill's measure arm must show it (the client logs `[OCTA] mirror ran move`).")
