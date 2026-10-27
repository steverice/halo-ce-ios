/* The production browser's row binding, with name/layout boundaries replaced. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include "halo_network_content.h"
#define ROW_TEXT_LENGTH 96
#define FALSE 0
#define TRUE 1
typedef int boolean;
#define _multiplayer_mode_server_browser 0
#define _multiplayer_mode_lan 1
#define NONE -1
#define PIN(value, low, high) ((value) < (low) ? (low) : (value) > (high) ? (high) : (value))
#define NUMBEROF(a) (sizeof(a) / sizeof((a)[0]))
#define ustrncpy wcsncpy
struct advertised_game { wchar_t game_name[16]; int engine_type, player_count, maximum_player_count; };
struct widget_instance { wchar_t text[96]; char const *name; struct widget_instance *parent; };
static struct { struct advertised_game *games[1]; int mode, game_count; } multiplayer;
static struct { int count, first; struct { int joinable; } games[3]; } lobby_browser;
static struct widget_instance type, ping, dedicated_icon, other;
static int dedicated, dedicated_visible;
static unsigned kind, version = 38;
static int advertised_dedicated(struct advertised_game *game) { (void)game; return dedicated; }
static wchar_t const *engine_names[] = { L"", L"CTF", L"SLAYER", L"ODDBALL", L"KING", L"RACE" };
static struct widget_instance *named(struct widget_instance *row, char const *name, int index)
{
	(void)row; (void)index;
	return !strcmp(name, "server_item_type") ? &type : !strcmp(name, "server_item_ping") ? &ping :
		!strcmp(name, "server_item_dedicated") ? &dedicated_icon : &other;
}
static void text_set(struct widget_instance *widget, wchar_t const *text) { wcsncpy(widget->text, text, 95); }
static void visible_set(struct widget_instance *widget, int visible) { if (widget == &dedicated_icon) dedicated_visible = visible; }
static void game_map_name(struct advertised_game *game, wchar_t *text) { (void)game; wcscpy(text, L"Blood Gulch"); }
static int advertised_in_progress(struct advertised_game *game) { (void)game; return 0; }
static unsigned advertised_kind(struct advertised_game *game) { (void)game; return kind; }
static unsigned network_game_client_advertised_game_version(void *client, struct advertised_game *game)
{ (void)client; (void)game; return version; }
static void *global_network_game_client_get(void) { return NULL; }
static int network_game_client_advertised_game_compatible(void *client, struct advertised_game *game, int tell)
{ (void)client; (void)game; (void)tell; return !kind && version == 38; }
/* Halo's formatter uses %s for wide strings and %hs for narrow strings. */
static void usnprintf(wchar_t *text, size_t count, wchar_t const *format, ...)
{
	wchar_t native[192]; size_t i, n = 0; va_list args;
	for (i = 0; format[i]; i++)
	{
		if (format[i] == '%' && format[i + 1] == 's')
		{ native[n++] = '%'; native[n++] = 'l'; native[n++] = 's'; i++; }
		else if (format[i] == '%' && format[i + 1] == 'h' && format[i + 2] == 's')
		{ native[n++] = '%'; native[n++] = 's'; i += 2; }
		else native[n++] = format[i];
	}
	native[n] = 0; va_start(args, format); vswprintf(text, count, native, args); va_end(args);
}
/* (no device limit here: the BIG mark and its ticker text are menu_functions.c's) */
static long player_capacity(void) { return 0; }
static int player_capacity_exceeded(long players) { (void)players; return 0; }
static int player_capacity_marked(long players, long maximum_players) { (void)players; (void)maximum_players; return 0; }

/* DECLARATIONS */
static void render_row(void)
{
	struct widget_instance row_data = { 0 }, *row = &row_data;
	struct advertised_game *game;
	wchar_t text[ROW_TEXT_LENGTH];
	int index = 0;
	/* ROW */
}
static void render_tint(struct widget_instance *widget, float *red, float *green, float *blue)
{
	struct { float red, green, blue; } color = { 0.1f, 0.3f, 0.8f };
	/* TINT */
	*red = color.red; *green = color.green; *blue = color.blue;
}
int main(void)
{
	struct advertised_game game = { L"Test game", 2, 2, 128 };
	struct widget_instance list = { .name = "join_game_items_list" };
	struct widget_instance row = { .name = "server_item_1", .parent = &list };
	struct widget_instance cell = { .name = "server_item_type", .parent = &row };
	struct widget_instance outside = { .name = "profile_name" };
	float red, green, blue;
	multiplayer.games[0] = &game;
	multiplayer.mode = _multiplayer_mode_lan;
	multiplayer.game_count = 1;
	render_row(); assert(!wcscmp(type.text, L"SLAYER"));
	assert(!dedicated_visible);
	dedicated = 1; render_row(); assert(dedicated_visible);
	dedicated = 0; render_row(); assert(!dedicated_visible);
	kind = 1; render_row(); assert(!wcscmp(type.text, L"SWAT"));
	kind = 2; render_row(); assert(!wcscmp(type.text, L"INFECTION"));
	kind = 3; render_row(); assert(!wcscmp(type.text, L"GUN GAME"));
	kind = 128; game.engine_type = 1; render_row(); assert(!wcscmp(type.text, L"HC CTF"));
	kind = 131; render_row(); assert(!wcscmp(type.text, L"HC GUN GAME"));
	kind = 4; render_row(); assert(!wcscmp(type.text, L"UNSUPPORTED"));
	kind = 0; version = 37; render_row(); assert(!wcscmp(ping.text, L"V37"));
	kind = 1; version = 38;
	assert(ui_widget_port_browser_row_unavailable(&cell));
	assert(!ui_widget_port_browser_row_unavailable(&outside));
	render_tint(&cell, &red, &green, &blue);
	assert(red == 0.5f && green == 0.5f && blue == 0.5f);
	kind = 0;
	assert(!ui_widget_port_browser_row_unavailable(&cell));
	render_tint(&cell, &red, &green, &blue);
	assert(red == 0.1f && green == 0.3f && blue == 0.8f);
	version = 37;
	assert(ui_widget_port_browser_row_unavailable(&cell));
	multiplayer.mode = _multiplayer_mode_server_browser;
	lobby_browser.count = 2;
	lobby_browser.first = 1;
	lobby_browser.games[1].joinable = 0;
	assert(ui_widget_port_browser_row_unavailable(&cell));
	lobby_browser.games[1].joinable = 1;
	assert(!ui_widget_port_browser_row_unavailable(&cell));
	puts("PASS: browser mode names, version labels, gray unavailable rows");
	return 0;
}
