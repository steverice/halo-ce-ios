"""Follow each UIKit button through SDL/XInput to the game's default action."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def touch_actions():
    overlay = (ROOT / "port/ios/host/host_touch.m").read_text()
    bridge = (ROOT / "port/linux/src/xinput_sdl.c").read_text()
    defaults = (ROOT / "source/input/input_abstraction.c").read_text()
    # Interpret the actual registration and translation tables rather than
    # keeping a second copy of the overlay's SDL assignments in this test.
    analog = dict((sdl, xbox.lower()) for xbox, sdl in re.findall(
        r"merge_button\(pad, XINPUT_GAMEPAD_(\w+), SDL_GetGamepadButton\(gamepad, (\w+)\)\)", bridge))
    digital = dict((sdl, xbox.lower()) for sdl, xbox in re.findall(
        r"\{ (SDL_GAMEPAD_BUTTON_\w+), XINPUT_GAMEPAD_(\w+) \}", bridge))
    bindings = dict((button, action) for action, button in re.findall(
        r"preferences->game_control_to_xbox_buttons\[_game_control_(\w+)\] = _gamepad_(\w+);", defaults))
    actions = {}
    for title, button, axis in re.findall(
        r'\[self addButton:@"([^"]+)" label:@"[^"]+" button:(\w+) axis:([\w-]+)\];', overlay
    ):
        if axis != "-1":
            xbox = "analog_button_" + axis.removeprefix("SDL_GAMEPAD_AXIS_").lower()
        elif button in analog:
            xbox = "analog_button_" + analog[button]
        else:
            # The Xbox names its stick clicks "thumb" rather than "stick".
            xbox = "binary_button_" + digital[button]
        actions[title] = bindings.get(xbox, xbox)
    return actions


EXPECTED_ACTIONS = dict([
    ("A", "jump"), ("B", "melee"), ("X", "action"), ("Y", "switch_weapons"),
    ("FIRE", "primary_trigger"), ("GRENADE", "grenade"),
    ("↑", "binary_button_dpad_up"), ("↓", "binary_button_dpad_down"),
    ("←", "binary_button_dpad_left"), ("→", "binary_button_dpad_right"),
    ("PAUSE", "start"), ("CROUCH", "crouch"), ("ZOOM", "zoom"),
    ("LIGHT", "flashlight"), ("SWAP G", "switch_grenades"),
])
def test_touch_buttons_reach_their_named_actions():
    actual = touch_actions()
    mismatches = {title: (actual.get(title), action) for title, action in EXPECTED_ACTIONS.items()
                  if actual.get(title) != action}
    assert not mismatches, mismatches


if __name__ == "__main__":
    test_touch_buttons_reach_their_named_actions()
