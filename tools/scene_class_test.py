#!/usr/bin/env python3
"""Compile and exercise the production scene_level function in isolation."""

from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SCENE_SOURCE = ROOT / "port/linux/game/scene_class.c"
SCENARIO_DEFINITIONS = ROOT / "source/scenario/scenario_definitions.h"


def extract_function(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise AssertionError(f"Could not find production function: {signature}")
    opening = source.find("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"Unterminated production function: {signature}")


def enum_value(source: str, name: str) -> int:
    value = -1
    for line in source.splitlines():
        item = line.split(",", 1)[0].strip()
        match = re.match(r"(\w+)(?:\s*=\s*(-?\d+))?$", item)
        if not match:
            continue
        value = int(match.group(2)) if match.group(2) is not None else value + 1
        if match.group(1) == name:
            return value
    raise AssertionError(f"Could not find enum value: {name}")


def main() -> None:
    scene_source = SCENE_SOURCE.read_text()
    definitions = SCENARIO_DEFINITIONS.read_text()
    scene_level = extract_function(scene_source, "static void scene_level(")
    scenario_solo = enum_value(definitions, "_scenario_type_solo")
    scenario_multiplayer = enum_value(definitions, "_scenario_type_multiplayer")
    scenario_menu = enum_value(definitions, "_scenario_type_main_menu")

    harness = f"""
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {{ _scenario_type_solo = {scenario_solo},
       _scenario_type_multiplayer = {scenario_multiplayer},
       _scenario_type_main_menu = {scenario_menu} }};
struct scenario {{ int type; }};
struct scenario *global_scenario;
static const char *solo_map;
static const char *multiplayer_map;
char *main_get_map_name(void) {{ return (char *)solo_map; }}
char *main_get_multiplayer_map_name(void) {{ return (char *)multiplayer_map; }}

{scene_level}

static void expect_level(const char *expected)
{{
    char actual[32];
    memset(actual, 0xA5, sizeof(actual));
    scene_level(actual, sizeof(actual));
    if (strcmp(actual, expected) != 0)
    {{
        fprintf(stderr, "expected scene label '%s', got '%s'\\n", expected, actual);
        abort();
    }}
}}

int main(void)
{{
    struct scenario scenario = {{0}};
    char tiny[5];

    scenario.type = _scenario_type_multiplayer;
    global_scenario = &scenario;
    solo_map = "levels\\\\c10\\\\c10";
    multiplayer_map = "levels\\\\test\\\\chillout\\\\chillout";
    expect_level("chillout");

    scenario.type = _scenario_type_main_menu;
    multiplayer_map = "levels\\\\test\\\\chillout\\\\chillout";
    expect_level("ui");

    scenario.type = _scenario_type_solo;
    solo_map = "levels\\\\b30\\\\b30";
    expect_level("b30");
    solo_map = "levels/archive/a50";
    expect_level("a50");

    global_scenario = NULL;
    expect_level("");

    scenario.type = _scenario_type_multiplayer;
    global_scenario = &scenario;
    multiplayer_map = "levels\\\\custom\\\\abcdefghijklmnopqrstuv";
    memset(tiny, 0xA5, sizeof(tiny));
    scene_level(tiny, sizeof(tiny));
    assert(strcmp(tiny, "abcd") == 0);
    assert(tiny[sizeof(tiny) - 1] == '\\0');
    return 0;
}}
"""

    with tempfile.TemporaryDirectory(prefix="scene-class-test-") as temporary:
        c_file = Path(temporary) / "scene_class_test.c"
        executable = Path(temporary) / "scene_class_test"
        c_file.write_text(harness)
        subprocess.run(
            [
                "clang",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(c_file),
                f"--output={executable}",
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True)
    print("scene_level production-function checks passed")


if __name__ == "__main__":
    main()
