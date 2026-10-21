/*
DELTA_LIST.C

The Delta game list's answer read, and the announcement written
(delta_list.h). The answer is JSON from a server this project does not run,
so it is read as untrusted text: bounded in depth, size and count, and
every string kept as printable ASCII of the size the browser shows.
*/

#include "delta_list.h"

#include <stdio.h>
#include <string.h>

enum
{
	MAXIMUM_DEPTH = 12,
	KEY_SIZE = 24,
};

struct parser
{
	const char *position;
	const char *end;
};

static void skip_space(struct parser *parser)
{
	while (parser->position < parser->end &&
		(*parser->position == ' ' || *parser->position == '\t' || *parser->position == '\n' ||
		*parser->position == '\r'))
	{
		parser->position++;
	}
}

static int hex_value(int character)
{
	if (character >= '0' && character <= '9')
		return character - '0';
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;
	if (character >= 'A' && character <= 'F')
		return character - 'A' + 10;
	return -1;
}

/* a string into text (size bytes with its end; NULL: only skipped), kept as
printable ASCII, a character of any other kind a "?", the rest of its
UTF-8 bytes dropped; 0 if it is not a string */
static int read_string(struct parser *parser, char *text, int size)
{
	int length = 0;

	if (parser->position >= parser->end || *parser->position != '"')
		return 0;
	parser->position++;
	for (;;)
	{
		int character;

		if (parser->position >= parser->end)
			return 0;
		character = (unsigned char)*parser->position++;
		if (character == '"')
			break;
		if (character == '\\')
		{
			if (parser->position >= parser->end)
				return 0;
			character = (unsigned char)*parser->position++;
			switch (character)
			{
			case 'n':
			case 't':
			case 'r':
			case 'b':
			case 'f':
				character = ' ';
				break;
			case 'u':
			{
				int value = 0, digit;

				for (digit = 0; digit < 4; digit++)
				{
					int nibble = parser->position < parser->end ? hex_value((unsigned char)*parser->position) : -1;

					if (nibble < 0)
						return 0;
					value = value * 16 + nibble;
					parser->position++;
				}
				character = value >= 0x20 && value < 0x7F ? value : value < 0x20 ? 0 : '?';
				/* (the low half of a pair of surrogates is no character of its own) */
				if (value >= 0xDC00 && value <= 0xDFFF)
					character = 0;
				break;
			}
			case '"':
			case '\\':
			case '/':
				break;
			default:
				return 0;
			}
		}
		else if (character < 0x20)
		{
			return 0;
		}
		else if (character >= 0x80)
		{
			/* (a continuation byte belongs to the "?" its first made) */
			if (character >= 0x80 && character < 0xC0)
				character = 0;
			else
				character = '?';
		}
		if (text && character && length < size - 1)
			text[length++] = (char)character;
	}
	if (text && size > 0)
		text[length] = 0;
	return 1;
}

static int skip_value(struct parser *parser, int depth);

static int skip_container(struct parser *parser, int depth, char close, int object)
{
	skip_space(parser);
	if (parser->position < parser->end && *parser->position == close)
	{
		parser->position++;
		return 1;
	}
	for (;;)
	{
		skip_space(parser);
		if (object)
		{
			if (!read_string(parser, NULL, 0))
				return 0;
			skip_space(parser);
			if (parser->position >= parser->end || *parser->position++ != ':')
				return 0;
		}
		if (!skip_value(parser, depth + 1))
			return 0;
		skip_space(parser);
		if (parser->position >= parser->end)
			return 0;
		if (*parser->position == ',')
		{
			parser->position++;
			continue;
		}
		if (*parser->position == close)
		{
			parser->position++;
			return 1;
		}
		return 0;
	}
}

static int skip_literal(struct parser *parser)
{
	const char *start = parser->position;

	while (parser->position < parser->end && (*parser->position == '-' || *parser->position == '+' ||
		*parser->position == '.' || (*parser->position >= '0' && *parser->position <= '9') ||
		(*parser->position >= 'a' && *parser->position <= 'z') ||
		(*parser->position >= 'A' && *parser->position <= 'Z')))
	{
		parser->position++;
	}
	return parser->position > start;
}

static int skip_value(struct parser *parser, int depth)
{
	if (depth > MAXIMUM_DEPTH)
		return 0;
	skip_space(parser);
	if (parser->position >= parser->end)
		return 0;
	switch (*parser->position)
	{
	case '"':
		return read_string(parser, NULL, 0);
	case '{':
		parser->position++;
		return skip_container(parser, depth, '}', 1);
	case '[':
		parser->position++;
		return skip_container(parser, depth, ']', 0);
	default:
		return skip_literal(parser);
	}
}

/* a number, true, false or null as an integer (a fraction dropped); 0 if the
value is another kind (it is not consumed) */
static int read_integer(struct parser *parser, long *value)
{
	const char *start;
	int negative = 0;

	skip_space(parser);
	if (parser->position >= parser->end)
		return 0;
	if (parser->end - parser->position >= 4 && !strncmp(parser->position, "true", 4))
	{
		parser->position += 4;
		*value = 1;
		return 1;
	}
	if (parser->end - parser->position >= 5 && !strncmp(parser->position, "false", 5))
	{
		parser->position += 5;
		*value = 0;
		return 1;
	}
	if (parser->end - parser->position >= 4 && !strncmp(parser->position, "null", 4))
	{
		parser->position += 4;
		*value = 0;
		return 1;
	}
	start = parser->position;
	if (*parser->position == '-')
	{
		negative = 1;
		parser->position++;
	}
	*value = 0;
	if (parser->position >= parser->end || *parser->position < '0' || *parser->position > '9')
	{
		parser->position = start;
		return 0;
	}
	while (parser->position < parser->end && *parser->position >= '0' && *parser->position <= '9')
	{
		if (*value < 100000000L)
			*value = *value * 10 + (*parser->position - '0');
		parser->position++;
	}
	/* (a fraction or an exponent: skipped) */
	while (parser->position < parser->end && (*parser->position == '.' || *parser->position == 'e' ||
		*parser->position == 'E' || *parser->position == '+' || *parser->position == '-' ||
		(*parser->position >= '0' && *parser->position <= '9')))
	{
		parser->position++;
	}
	if (negative)
		*value = -*value;
	return 1;
}

/* a map's short name: after its last path separator, before its "@" family */
static void short_map_name(char *map)
{
	char *separator = strrchr(map, '\\');
	char *slash = strrchr(map, '/');
	char *at;

	if (slash && (!separator || slash > separator))
		separator = slash;
	if (separator)
		memmove(map, separator + 1, strlen(separator + 1) + 1);
	at = strchr(map, '@');
	if (at)
		*at = 0;
}

/* spaces taken from both ends */
static void trim(char *text)
{
	size_t length = strlen(text);
	size_t start = strspn(text, " ");

	while (length > start && text[length - 1] == ' ')
		length--;
	memmove(text, text + start, length - start);
	text[length - start] = 0;
}

static int valid_invite(char *invite)
{
	int index;

	if (strlen(invite) != DELTA_INVITE_DIGITS)
		return 0;
	for (index = 0; index < DELTA_INVITE_DIGITS; index++)
	{
		if (hex_value((unsigned char)invite[index]) < 0)
			return 0;
		if (invite[index] >= 'A' && invite[index] <= 'F')
			invite[index] = (char)(invite[index] - 'A' + 'a');
	}
	return 1;
}

/* one game object into game; 1 if it is a game to offer, 0 if not (the
object is read to its end either way), -1 if the text is malformed */
static int read_game(struct parser *parser, struct delta_game *game)
{
	int verified = 1, locked = 0, coop = 0;
	/* (room for one that is too long, which is refused and not cut to fit) */
	char invite[DELTA_INVITE_DIGITS + 16];

	memset(game, 0, sizeof(*game));
	invite[0] = 0;
	skip_space(parser);
	if (parser->position >= parser->end || *parser->position++ != '{')
		return -1;
	skip_space(parser);
	if (parser->position < parser->end && *parser->position == '}')
	{
		parser->position++;
		return 0;
	}
	for (;;)
	{
		char key[KEY_SIZE + 1];
		const char *start;
		long value;
		int known = 1;

		skip_space(parser);
		/* (a key too long to be one of ours: cut, and not one) */
		if (!read_string(parser, key, sizeof(key)))
			return -1;
		skip_space(parser);
		if (parser->position >= parser->end || *parser->position++ != ':')
			return -1;
		skip_space(parser);
		start = parser->position;
		if (!strcmp(key, "invite"))
		{
			known = read_string(parser, invite, sizeof(invite));
			if (!known)
				invite[0] = 0;
		}
		else if (!strcmp(key, "name"))
		{
			known = read_string(parser, game->name, sizeof(game->name));
		}
		else if (!strcmp(key, "map"))
		{
			char map[256];

			known = read_string(parser, map, sizeof(map));
			if (known)
			{
				short_map_name(map);
				strncpy(game->map, map, sizeof(game->map) - 1);
			}
		}
		else if (!strcmp(key, "gametype"))
		{
			known = read_string(parser, game->gametype, sizeof(game->gametype));
		}
		else if (!strcmp(key, "mode"))
		{
			char mode[16];

			known = read_string(parser, mode, sizeof(mode));
			coop = known && !strcmp(mode, "coop");
		}
		else if (!strcmp(key, "engine") || !strcmp(key, "players") || !strcmp(key, "maximum_players") ||
			!strcmp(key, "open") || !strcmp(key, "in_progress") || !strcmp(key, "version") ||
			!strcmp(key, "teams") || !strcmp(key, "verified") || !strcmp(key, "locked"))
		{
			known = read_integer(parser, &value);
			if (known)
			{
				if (value < 0)
					value = 0;
				if (value > 65535)
					value = 65535;
				if (!strcmp(key, "engine"))
					game->engine = (int)value;
				else if (!strcmp(key, "players"))
					game->players = (int)value;
				else if (!strcmp(key, "maximum_players"))
					game->maximum_players = (int)value;
				else if (!strcmp(key, "open"))
					game->open = value != 0;
				else if (!strcmp(key, "in_progress"))
					game->in_progress = value != 0;
				else if (!strcmp(key, "version"))
					game->version = (int)value;
				else if (!strcmp(key, "teams"))
					game->teams = value != 0;
				else if (!strcmp(key, "verified"))
					verified = value != 0;
				else
					locked = value != 0;
			}
		}
		else
		{
			known = 0;
		}
		if (!known)
		{
			/* (a value of another kind than ours: skipped whole) */
			parser->position = start;
			if (!skip_value(parser, 1))
				return -1;
		}
		skip_space(parser);
		if (parser->position >= parser->end)
			return -1;
		if (*parser->position == ',')
		{
			parser->position++;
			continue;
		}
		if (*parser->position++ == '}')
			break;
		return -1;
	}
	trim(game->name);
	trim(game->gametype);
	if (!valid_invite(invite) || locked || coop || !verified || !game->name[0])
		return 0;
	strcpy(game->invite, invite);
	if (game->maximum_players < game->players)
		game->maximum_players = game->players;
	return 1;
}

int delta_parse_games(const char *json, int size, struct delta_game *games, int maximum)
{
	struct parser parser;
	int count = 0, found = 0;

	if (!json || size <= 0 || size > DELTA_MAXIMUM_ANSWER || maximum <= 0)
		return -1;
	parser.position = json;
	parser.end = json + size;
	skip_space(&parser);
	if (parser.position >= parser.end || *parser.position++ != '{')
		return -1;
	skip_space(&parser);
	if (parser.position < parser.end && *parser.position == '}')
		return -1;
	for (;;)
	{
		char key[KEY_SIZE + 1];

		skip_space(&parser);
		if (!read_string(&parser, key, sizeof(key)))
			return -1;
		skip_space(&parser);
		if (parser.position >= parser.end || *parser.position++ != ':')
			return -1;
		skip_space(&parser);
		if (!strcmp(key, "games") && !found)
		{
			found = 1;
			if (parser.position >= parser.end || *parser.position++ != '[')
				return -1;
			skip_space(&parser);
			if (parser.position < parser.end && *parser.position == ']')
			{
				parser.position++;
			}
			else
			{
				for (;;)
				{
					struct delta_game game;
					int result = read_game(&parser, &game);

					if (result < 0)
						return -1;
					if (result > 0 && count < maximum)
						games[count++] = game;
					skip_space(&parser);
					if (parser.position >= parser.end)
						return -1;
					if (*parser.position == ',')
					{
						parser.position++;
						continue;
					}
					if (*parser.position++ == ']')
						break;
					return -1;
				}
			}
		}
		else if (!skip_value(&parser, 1))
		{
			return -1;
		}
		skip_space(&parser);
		if (parser.position >= parser.end)
			return -1;
		if (*parser.position == ',')
		{
			parser.position++;
			continue;
		}
		if (*parser.position++ == '}')
			break;
		return -1;
	}
	skip_space(&parser);
	if (parser.position != parser.end || !found)
		return -1;
	return count;
}

/* ---------- the announcement */

/* text percent-encoded (everything but RFC 3986's unreserved characters)
at form + length; 0 if it does not fit */
static int put_encoded(char *form, int size, int *length, const char *text)
{
	static const char digits[] = "0123456789ABCDEF";

	for (; *text; text++)
	{
		unsigned char character = (unsigned char)*text;

		if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9') || character == '-' || character == '.' || character == '_' ||
			character == '~')
		{
			if (*length + 1 >= size)
				return 0;
			form[(*length)++] = (char)character;
		}
		else
		{
			if (*length + 3 >= size)
				return 0;
			form[(*length)++] = '%';
			form[(*length)++] = digits[character >> 4];
			form[(*length)++] = digits[character & 15];
		}
	}
	return 1;
}

static int put_field(char *form, int size, int *length, const char *name, const char *value)
{
	if (*length)
	{
		if (*length + 1 >= size)
			return 0;
		form[(*length)++] = '&';
	}
	if (!put_encoded(form, size, length, name) || *length + 1 >= size)
		return 0;
	form[(*length)++] = '=';
	return put_encoded(form, size, length, value);
}

static int put_number(char *form, int size, int *length, const char *name, int value)
{
	char text[16];

	snprintf(text, sizeof(text), "%d", value);
	return put_field(form, size, length, name, text);
}

int delta_announce_form(char *form, int size, const struct delta_announce *announce)
{
	int length = 0;

	if (size < 2)
		return 0;
	if (!put_field(form, size, &length, "invite", announce->invite) ||
		!put_field(form, size, &length, "name", announce->name) ||
		!put_field(form, size, &length, "map", announce->map) ||
		!put_number(form, size, &length, "engine", announce->engine) ||
		!put_number(form, size, &length, "players", announce->players) ||
		!put_number(form, size, &length, "maximum_players", announce->maximum_players) ||
		!put_number(form, size, &length, "open", announce->open ? 1 : 0) ||
		!put_number(form, size, &length, "version", announce->version) ||
		!put_number(form, size, &length, "teams", announce->teams ? 1 : 0) ||
		/* (this build plays the legacy protocol alone: no Delta Peer) */
		!put_number(form, size, &length, "delta", 0))
	{
		return 0;
	}
	form[length] = 0;
	return length;
}

int delta_withdraw_form(char *form, int size, const char *invite)
{
	int length = 0;

	if (size < 2 || !put_field(form, size, &length, "invite", invite))
		return 0;
	form[length] = 0;
	return length;
}

int delta_announce_answer(const char *text, int *seconds)
{
	int value = 0;

	if (strncmp(text, "ok", 2) || (text[2] && text[2] != ' ' && text[2] != '\n' && text[2] != '\r'))
		return 0;
	if (text[2] == ' ')
		sscanf(text + 3, "%d", &value);
	*seconds = value > 0 ? value : 0;
	return 1;
}

/* ---------- when announcing goes on */

static int announce_changed(const struct delta_announce *a, const struct delta_announce *b)
{
	return strcmp(a->name, b->name) || strcmp(a->map, b->map) || a->engine != b->engine ||
		a->players != b->players || a->maximum_players != b->maximum_players || a->open != b->open ||
		a->teams != b->teams;
}

static int waiting(const struct delta_announce_state *state, unsigned long now)
{
	return state->wait_until && (long)(now - state->wait_until) < 0;
}

int delta_announce_ready(const struct delta_announce *announce)
{
	return announce->name[0] && announce->map[0] && announce->engine >= 1 && announce->engine <= 6;
}

int delta_announce_step(const struct delta_announce_state *state, const struct delta_announce *wanted,
	unsigned long now)
{
	if (wanted && !delta_announce_ready(wanted))
		wanted = NULL;
	if (state->listed)
	{
		unsigned long since = now - state->sent_time;

		if (!wanted || strcmp(wanted->invite, state->invite))
			return DELTA_STEP_WITHDRAW;
		if (waiting(state, now))
			return DELTA_STEP_NOTHING;
		if (since >= DELTA_ANNOUNCE_INTERVAL ||
			(since >= DELTA_ANNOUNCE_MINIMUM && announce_changed(wanted, &state->sent)))
		{
			return DELTA_STEP_ANNOUNCE;
		}
		return DELTA_STEP_NOTHING;
	}
	if (!wanted || waiting(state, now))
		return DELTA_STEP_NOTHING;
	return DELTA_STEP_ANNOUNCE;
}

void delta_announce_done(struct delta_announce_state *state, const struct delta_announce *announce, int status,
	int ok, unsigned long now)
{
	if (ok)
	{
		state->listed = 1;
		strcpy(state->invite, announce->invite);
		state->sent = *announce;
		state->sent_time = now;
		state->failures = 0;
		state->wait_until = 0;
	}
	else
	{
		unsigned long wait = DELTA_RETRY_WAIT;
		int index;

		state->failures++;
		for (index = 1; index < state->failures && wait < DELTA_RETRY_MAXIMUM; index++)
			wait *= 2;
		if (wait > DELTA_RETRY_MAXIMUM)
			wait = DELTA_RETRY_MAXIMUM;
		if ((status == 403 || status == 429) && wait < DELTA_REFUSED_WAIT)
			wait = DELTA_REFUSED_WAIT;
		state->wait_until = now + wait ? now + wait : 1;
	}
}

void delta_withdraw_done(struct delta_announce_state *state)
{
	state->listed = 0;
	state->invite[0] = 0;
}
