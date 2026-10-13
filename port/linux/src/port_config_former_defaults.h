/*
PORT_CONFIG_FORMER_DEFAULTS.H

Defaults that main once shipped for settings a device profile may set
(port_profile_keys.h), as config.toml held them. A config.toml from before
config.origins has every setting at whatever default its build wrote: a value
there equal to the current default or to one of these was written by the
game, not chosen by the player, and follows the device's profile
(port_config.c, config_origin_decide).

Add a line whenever a profile setting's default changes. Only profile
settings: for a setting that is the player's, and above all a boolean whose
default flipped, "equal to a former default" would discard real choices.

	FORMER_DEFAULT(name, value as the file held it)
*/

FORMER_DEFAULT("display.frame_pacing", "\"tick\"")
FORMER_DEFAULT("display.foveation", "false")
