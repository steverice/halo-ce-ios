/* settings the host reads itself from config.toml (host_config.c) */
#pragma once
#include <stddef.h>

/* section.key as a number, or fallback when it isn't in the file */
double host_config_real(const char *name, double fallback);
/* the device profile's value for a setting config.toml lacks (host_device.m installs it): asked before a
   fallback is used. config.toml itself already holds the profile's values once the game has run
   (port/linux/src/port_config.c writes a setting nobody chose at its device value) */
void host_config_set_profile(int (*value)(const char *name, char *text, size_t size));
/* section.key as a string, unquoted, or fallback */
void host_config_string(const char *name, const char *fallback, char *out, size_t size);
/* section.key set to true or false in config.toml, every other line kept (the section added when missing);
   1 on success */
int host_config_write_boolean(const char *name, int value);
