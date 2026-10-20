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
	char non_bmp[] = { 'f', 'r', 'a', (char)0xF0, (char)0x90, (char)0xBA, (char)0x80, 0 };
	char emoji[] = { 'f', 'r', 'a', 'k', ' ', (char)0xF0, (char)0x9F, (char)0x98, (char)0x80, 0 };
	char emoji_expected[] = { '*', '*', '*', '*', ' ', (char)0xF0, (char)0x9F, (char)0x98, (char)0x80, 0 };
	char truncated[] = { 'f', 'r', 'a', (char)0xF0, (char)0x90, (char)0xBA, 0 };
	char malformed[] = { 'f', 'r', 'a', (char)0xF0, (char)0x90, (char)0xBA, 'x', 0 };
	char non_bmp_original[sizeof non_bmp];
	char truncated_original[sizeof truncated];
	char malformed_original[sizeof malformed];
	const char *shown;

	(void)argc;
	(void)argv;
	memcpy(non_bmp_original, non_bmp, sizeof non_bmp);
	memcpy(truncated_original, truncated, sizeof truncated);
	memcpy(malformed_original, malformed, sizeof malformed);
	CHECK(text_filter_string(source) == source, "disabled mode changed a string pointer");
	text_filter_utf8(message);
	CHECK(!strcmp(message, "frak"), "disabled mode changed a message");
	text_filter_utf8(non_bmp);
	CHECK(!memcmp(non_bmp, non_bmp_original, sizeof non_bmp), "disabled mode changed non-BMP bytes");

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
	text_filter_utf8(non_bmp);
	CHECK(!memcmp(non_bmp, non_bmp_original, sizeof non_bmp), "non-BMP scalar caused a false match");
	text_filter_utf8(emoji);
	CHECK(!memcmp(emoji, emoji_expected, sizeof emoji), "emoji changed beside filtered ASCII");
	text_filter_utf8(truncated);
	CHECK(!memcmp(truncated, truncated_original, sizeof truncated), "truncated four-byte prefix caused a false match");
	text_filter_utf8(malformed);
	CHECK(!memcmp(malformed, malformed_original, sizeof malformed), "malformed four-byte prefix caused a false match");

	streamer_enabled = 0;
	settings_generation++;
	CHECK(text_filter_string(source) == source, "disabled mode retained filtering");
	CHECK(!strcmp(source, "frak"), "disabled mode changed source text");
	return 0;
}
