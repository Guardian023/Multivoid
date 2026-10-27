// coop/config/config_registry_rows_dev_container.inc -- the [dev] container rows that sat together in the dev row list,
// moved in their order: the contents selftest, the join drill with its two red arms, the opener probe and the
// view drill. Included by config_registry_rows_dev.inc where they stood, so each row keeps its place in every
// expansion of the list. The entry forms are config_registry_rows.inc's.

CFG_FLAG(container_selftest, "container_selftest", "dev", false, "VOTVCOOP_CONTAINER_SELFTEST",
    "Autonomous selftest: container contents sync. It MUTATES the world -- it extracts an item on"
    " each peer -- so it is armed per run from the environment, like the hand-drop one.")
CFG_FLAG(container_join_drill, "container_join_drill", "dev", false, "VOTVCOOP_CONTAINER_JOIN_DRILL",
    "Drill (BOTH peers): the host extracts an item while a joiner is connected but not world-ready; the joiner "
    "then reports a content hash per world container and the host judges it. Lines tagged [CONTAINER-JOIN] and "
    "[CONTAINER-PARITY]; the host's DONE line ends it. It MUTATES the world.")
CFG_FLAG(container_retry_red, "container_retry_red", "dev", false, "VOTVCOOP_CONTAINER_RETRY_RED",
    "Red arm for the container join drill: a container send refused with no world-ready peer is re-armed "
    "every sweep, as before the fix, and counted.")
CFG_FLAG(container_parity_red, "container_parity_red", "dev", false, "VOTVCOOP_CONTAINER_PARITY_RED",
    "Red arm for the container parity report (the CLIENT): flips one bit of the first row's content hash "
    "before sending, so the host's judge must report a mismatch.")
CFG_FLAG(container_opener_probe, "container_opener_probe", "dev", false, "VOTVCOOP_CONTAINER_OPENER_PROBE",
    "Probe (host): once each client's world is up and it has a body, every drone, dronesack and ATV with "
    "the container it opens, the lookups both ways that must name the pair, and each client's reach to "
    "both, again when a client stands at an opener; a verdict per pass. Read-only.")
CFG_FLAG(container_view_drill, "container_view_drill", "dev", false, "VOTVCOOP_CONTAINER_VIEW_DRILL",
    "Drill (client): walks to the ATV, opens its container through the game's own openPropInv, then walks "
    "back; the view must stay open while the ATV is within reach and close on the tick it leaves it. Run it "
    "with join_at_host=1. Lines tagged [CVIEW-DRILL]; the client's DONE line, carrying the verdict, ends it.")
