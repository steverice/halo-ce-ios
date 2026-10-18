#!/usr/bin/env bash
# Native, asset-free checks of the actual animation helpers and load callbacks.
set -eu
probe_root=$(cd "${BASH_SOURCE[0]%/*}/.." && pwd)
probe_dir=$(mktemp -d -t enhanced-animation-test)
trap 'rm -r -f "$probe_dir"' EXIT

awk '/^static short \*node_link\(/ || /^static short \*node_chain_walk\(/ || /^static boolean node_tree_check\(/ {copy=1} copy {print} copy && /^}/ {copy=0}' \
    "$probe_root/port/linux/game/tag_schema_models.c" > "$probe_dir/under_test.inc"
awk '/^static boolean unit_node_is_at_or_below\(/ {copy=1} copy {print} copy && /^}/ {copy=0}' \
    "$probe_root/source/units/units.c" >> "$probe_dir/under_test.inc"
awk '/^#define GRENADE_THROW_FULL_MOVE_SPEED / {copy=1} /^static struct animation \*unit_weapon_class_base_animation\(/ {copy=0} copy {print}' \
    "$probe_root/source/units/units.c" >> "$probe_dir/under_test.inc"
awk '/^void units_initialize_for_new_map\(/ {copy=1} copy {print} copy && /^}/ {copy=0}' \
    "$probe_root/source/units/units.c" >> "$probe_dir/under_test.inc"
awk '/^static game_state_before_load_proc before_load_procs\[\]/ {copy=1} copy {print} copy && /^};/ {copy=0}' \
    "$probe_root/source/saved games/game_state.c" >> "$probe_dir/under_test.inc"
awk '/^static void game_state_call_before_load_procs\(/ {copy=1} copy {print} copy && /^}/ {copy=0}' \
    "$probe_root/source/saved games/game_state.c" >> "$probe_dir/under_test.inc"

probe_cc=${CC:-clang}
"$probe_cc" -std=c11 -Wall -Wextra -Werror -Wno-unused-function -I "$probe_dir" \
    "$probe_root/tools/enhanced_animation_probe.c" -o "$probe_dir/probe"
probe_failed=0
for probe_case in hierarchy root bounds cycle salt same-tick map-reset load-reset rewind first-tick ik-salt ik-map-reset ik-load-reset ik-rewind ik-grace leg-ease; do
    if "$probe_dir/probe" "$probe_case"; then
        printf 'PASS %s\n' "$probe_case"
    else
        printf 'FAIL %s\n' "$probe_case"
        probe_failed=1
    fi
done
exit "$probe_failed"
