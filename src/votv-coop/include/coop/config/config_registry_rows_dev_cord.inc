// coop/config/config_registry_rows_dev_cord.inc -- the [dev] row of the cord lane: its drill. Included by
// config_registry_rows_dev.inc after the fire rows, before the ATV rows, so each row keeps its place in every
// expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(cord_drill, "cord_drill", "dev", false, "VOTVCOOP_CORD_DRILL",
    "Drill, the cord lane; set on BOTH peers. The host spawns two coffee makers by a cluster of free wall "
    "sockets; each peer's player walks there by the director and plugs, drags and holds cords through the "
    "game's own verbs while the other peer judges what it sees and the host's word. Lines tagged [CORD]; "
    "the client prints 'arms 1-7 PASS' and, rejoined, 'client DONE -- PASS'. Run it on a fresh host world.")
