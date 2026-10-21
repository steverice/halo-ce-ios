/* The engine's dialog, profile accessor, and HUD visibility expression. */
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef int boolean;
#define TRUE 1
#define FALSE 0
#define NONE (-1)
#define MAXIMUM_NUMBER_OF_LOCAL_PLAYERS 4
#define match_assert(file, line, condition) assert(condition)
#define match_vassert(file, line, condition, message) assert(condition)
#define _error_silent 0
#define _ui_widget_type_text_box 1
#define csstrncpy strncpy
#define error(...) ((void)0)

static struct {
    struct {
        struct {
            struct { boolean ingame_help_disabled; } controller_settings;
        } profile;
    } local_players[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];
    short single_player_controller[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];
} player_ui_globals;

struct widget_instance {
    struct widget_instance *child, *next;
    int type;
    struct { struct { short string_list_index; } text_box; } parameters;
};
static struct widget_instance text_box, dialog;
static int loads;
static short owner, loaded_owner;
static boolean network_active;
static boolean network_game_is_active(void) { return network_active; }
static int global_scenario_index = 1;
static char const *scenario = "levels\\b40\\b40";
static char const *loaded_name;
static short player_ui_get_single_player_local_player_controller(short index)
{
    assert(index == 0);
    return owner;
}
static char const *tag_get_name(int index) { return scenario; }
static void strlwr(char *text)
{
    for (; *text; ++text) *text = (char)tolower((unsigned char)*text);
}
static struct widget_instance *ui_widget_load_by_name_or_tag(
    char const *name, int tag, void *parent, short controller, int a, int b, int c)
{
    ++loads;
    loaded_name = name;
    loaded_owner = controller;
    return &dialog;
}
#include "under_test.inc"

static struct { boolean show_hud_help_text; } scripted, *hud_scripted_globals = &scripted;
static struct { void *help_message; } messaging, *hud_messaging_globals = &messaging;
static boolean hud_help_visible(short local_player_index)
{
    boolean help_active;
#include "hud_help.inc"
    return help_active;
}
int main(void)
{
    short index;
    dialog.child = &text_box;
    text_box.type = _ui_widget_type_text_box;
    /* A controller other than zero owns the solo dialog. */
    player_ui_globals.single_player_controller[0] = 2;
    player_ui_globals.single_player_controller[1] = 0;
    player_ui_globals.single_player_controller[2] = NONE;
    player_ui_globals.single_player_controller[3] = NONE;
    owner = 2;
    display_scenario_help(7);
    assert(loads == 1 && loaded_owner == 2);
    assert(strstr(loaded_name, "player_help_screen_b40"));
    assert(text_box.parameters.text_box.string_list_index == 7);
    player_ui_globals.local_players[1].profile.controller_settings.ingame_help_disabled = TRUE;
    display_scenario_help(8);
    assert(loads == 2 && text_box.parameters.text_box.string_list_index == 8);
    player_ui_globals.local_players[0].profile.controller_settings.ingame_help_disabled = TRUE;
    display_scenario_help(9);
    assert(loads == 2 && text_box.parameters.text_box.string_list_index == 8);
    /* No owner retains the existing default behavior without indexing NONE. */
    owner = NONE;
    display_scenario_help(10);
    assert(loads == 3 && loaded_owner == NONE);
    assert(!player_ui_ingame_help_disabled(NONE));
    network_active = TRUE;
    scripted.show_hud_help_text = TRUE;
    messaging.help_message = &text_box;
    for (index = 0; index < MAXIMUM_NUMBER_OF_LOCAL_PLAYERS; ++index) {
        player_ui_globals.local_players[index].profile.controller_settings.ingame_help_disabled = FALSE;
        assert(hud_help_visible(index));
        player_ui_globals.local_players[index].profile.controller_settings.ingame_help_disabled = TRUE;
        assert(!hud_help_visible(index));
        assert(scripted.show_hud_help_text && messaging.help_message == &text_box);
    }
    network_active = FALSE;
    player_ui_globals.local_players[0].profile.controller_settings.ingame_help_disabled = FALSE;
    assert(hud_help_visible(2));
    assert(!hud_help_visible(0));
    assert(!player_ui_ingame_help_disabled(3));
    scripted.show_hud_help_text = FALSE;
    assert(!hud_help_visible(2));
    scripted.show_hud_help_text = TRUE;
    messaging.help_message = NULL;
    assert(!hud_help_visible(2));
    puts("scenario dialogs and per-player HUD help respect profiles");
    return 0;
}
