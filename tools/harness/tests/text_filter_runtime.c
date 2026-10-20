/* The display wrapper leaves source text unchanged when streamer mode is off. */

#include "harness.h"
#include "under_test.inc"

static int streamer_enabled;
static unsigned long settings_generation = 1;

int config_boolean(const char *name)
{
	CHECK(!strcmp(name, "streamer.enabled"), "unexpected setting %s", name);
	return streamer_enabled;
}

const char *config_string(const char *name)
{
	if (!strcmp(name, "streamer.slurs"))
		return "mild";
	if (!strcmp(name, "streamer.swearing"))
		return "strong";
	if (!strcmp(name, "streamer.insults"))
		return "off";
	CHECK(0, "unexpected setting %s", name);
	return NULL;
}

unsigned long config_changes(void)
{
	return settings_generation;
}

int main(int argc, char **argv)
{
	char source[] = "frak";
	char message[] = "frak";
	const char *shown;

	(void)argc;
	(void)argv;
	CHECK(text_filter_string(source) == source, "disabled mode changed a string pointer");
	text_filter_utf8(message);
	CHECK(!strcmp(message, "frak"), "disabled mode changed a message");

	streamer_enabled = 1;
	settings_generation++;
	text_filter_string("clean");
	CHECK(text_filter_word_add("frak", _text_filter_swearing, _text_filter_strong,
		_text_filter_whole), "could not add test word");
	shown = text_filter_string(source);
	CHECK(!strcmp(shown, "****"), "enabled mode did not filter displayed text");
	CHECK(!strcmp(source, "frak"), "filter changed its source string");
	text_filter_utf8(message);
	CHECK(!strcmp(message, "****"), "enabled mode did not filter a message");

	streamer_enabled = 0;
	settings_generation++;
	CHECK(text_filter_string(source) == source, "disabled mode retained filtering");
	CHECK(!strcmp(source, "frak"), "disabled mode changed source text");
	return 0;
}
