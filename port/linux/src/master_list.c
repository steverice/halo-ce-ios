/* MASTER_LIST.C

The official master's signature authenticates the exact response bytes. Only
then is JSON read; each host's listing has an independent signature. The JSON
reader follows delta_list.c's bounded cursor approach, with strict JSON numbers
and types because protocol and timestamps are security boundaries here.
*/
#include "master_list.h"
#include "monocypher-ed25519.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

struct master_json
{
	const char *position, *end;
};

static const char master_key_id[] = "fe290a97f7ab592b";
static const unsigned char master_public_key[32] = {
	0x89, 0xf4, 0xed, 0xa7, 0x12, 0x29, 0xf1, 0x1f,
	0x35, 0x38, 0x67, 0x9f, 0x49, 0xef, 0x7f, 0xb0,
	0xc2, 0xb4, 0x07, 0xb3, 0x80, 0x09, 0x3f, 0x7f,
	0x4f, 0xe0, 0x21, 0x05, 0xb9, 0x22, 0x25, 0xaa,
};

static int master_error(char *error, int size, const char *reason)
{
	if (error && size > 0)
		snprintf(error, (size_t)size, "%s", reason);
	return -1;
}

static void json_space(struct master_json *json)
{
	while (json->position < json->end && strchr(" \t\r\n", *json->position) && *json->position)
		json->position++;
}

static int json_take(struct master_json *json, char character)
{
	json_space(json);
	if (json->position == json->end || *json->position != character)
		return 0;
	json->position++;
	return 1;
}

static int hex_digit(unsigned char character)
{
	if (character >= '0' && character <= '9') return character - '0';
	if (character >= 'a' && character <= 'f') return character - 'a' + 10;
	if (character >= 'A' && character <= 'F') return character - 'A' + 10;
	return -1;
}

/* Decodes to the browser's printable ASCII convention. length counts every
printable character even when the destination is full, so identifiers cannot
be accepted after truncation. Controls are removed, not replaced by spaces. */
static int json_text(struct master_json *json, char *text, int size, int clean)
{
	int length = 0;
	if (!json_take(json, '"')) return -1;
	while (json->position < json->end)
	{
		unsigned int character = (unsigned char)*json->position++;
		if (character == '"')
		{
			if (text && size > 0) text[length < size ? length : size - 1] = 0;
			return length;
		}
		if (character < 32) return -1;
		if (character == '\\')
		{
			if (json->position == json->end) return -1;
			character = (unsigned char)*json->position++;
			if (character == 'u')
			{
				int index;
				character = 0;
				for (index = 0; index < 4; index++)
				{
					int digit = json->position < json->end ? hex_digit((unsigned char)*json->position++) : -1;
					if (digit < 0) return -1;
					character = character * 16 + (unsigned int)digit;
				}
			}
			else if (character && strchr("ntbrf", (int)character)) character = 0;
			else if (character != '"' && character != '\\' && character != '/') return -1;
		}
		if (character < 32 || (character >= 127 && character <= 159))
		{
			if (clean) continue;
			character = 1;
		}
		if (clean && character >= 0xDC00 && character <= 0xDFFF) continue;
		if (clean && character >= 128 && character < 192) continue;
		if (character > 126) character = '?';
		if (text && length < size - 1) text[length] = (char)character;
		length++;
	}
	return -1;
}

static int json_string(struct master_json *json, char *text, int size)
{
	return json_text(json, text, size, 0);
}

static int json_digit(char character)
{
	return character >= '0' && character <= '9';
}

static int json_number(struct master_json *json)
{
	const char *start = json->position;
	if (json->position < json->end && *json->position == '-') json->position++;
	if (json->position == json->end || !json_digit(*json->position)) return 0;
	if (*json->position++ != '0')
		while (json->position < json->end && json_digit(*json->position)) json->position++;
	if (json->position < json->end && *json->position == '.')
	{
		json->position++;
		if (json->position == json->end || !json_digit(*json->position)) return 0;
		while (json->position < json->end && json_digit(*json->position)) json->position++;
	}
	if (json->position < json->end && (*json->position == 'e' || *json->position == 'E'))
	{
		json->position++;
		if (json->position < json->end && (*json->position == '+' || *json->position == '-')) json->position++;
		if (json->position == json->end || !json_digit(*json->position)) return 0;
		while (json->position < json->end && json_digit(*json->position)) json->position++;
	}
	return json->position > start;
}

static int json_value(struct master_json *json, int depth)
{
	char close;
	int object;
	json_space(json);
	if (depth > 16 || json->position == json->end) return 0;
	if (*json->position == '"') return json_string(json, NULL, 0) >= 0;
	if (*json->position != '[' && *json->position != '{')
	{
		static const char *const literals[] = { "true", "false", "null" };
		int index;
		for (index = 0; index < 3; index++)
		{
			size_t size = strlen(literals[index]);
			if ((size_t)(json->end - json->position) >= size && !memcmp(json->position, literals[index], size))
			{
				json->position += size;
				return 1;
			}
		}
		return json_number(json);
	}
	object = *json->position++ == '{';
	close = object ? '}' : ']';
	if (json_take(json, close)) return 1;
	for (;;)
	{
		if (object && (json_string(json, NULL, 0) < 0 || !json_take(json, ':'))) return 0;
		if (!json_value(json, depth + 1)) return 0;
		if (json_take(json, close)) return 1;
		if (!json_take(json, ',')) return 0;
	}
}

/* Finds one direct member in an already validated object. Duplicate members
are refused rather than letting consumers disagree on which one was signed. */
static int json_member(struct master_json object, const char *name, struct master_json *value)
{
	int found = 0;
	if (!json_take(&object, '{')) return 0;
	if (json_take(&object, '}')) return 0;
	for (;;)
	{
		char key[64];
		int length = json_string(&object, key, sizeof(key));
		const char *start;
		if (length < 0 || !json_take(&object, ':')) return 0;
		json_space(&object);
		start = object.position;
		if (!json_value(&object, 1)) return 0;
		if (length == (int)strlen(name) && !strcmp(key, name))
		{
			if (found) return 0;
			value->position = start;
			value->end = object.position;
			found = 1;
		}
		if (json_take(&object, '}')) return found;
		if (!json_take(&object, ',')) return 0;
	}
}

static int json_unsigned(struct master_json json, unsigned long long *value)
{
	unsigned long long number = 0;
	if (json.position == json.end) return 0;
	while (json.position < json.end)
	{
		unsigned int digit = (unsigned char)*json.position++ - '0';
		if (digit > 9 || number > (ULLONG_MAX - digit) / 10) return 0;
		number = number * 10 + digit;
	}
	*value = number;
	return 1;
}

static int base64_digit(unsigned char character)
{
	if (character >= 'A' && character <= 'Z') return character - 'A';
	if (character >= 'a' && character <= 'z') return character - 'a' + 26;
	if (character >= '0' && character <= '9') return character - '0' + 52;
	if (character == '+') return 62;
	if (character == '/') return 63;
	return -1;
}

/* Standard padded alphabet only, with canonical zero padding bits. */
static int decode_base64(const char *text, unsigned char *bytes, int maximum)
{
	size_t length = strlen(text), offset;
	int count = 0;
	if (!length || length % 4) return -1;
	for (offset = 0; offset < length; offset += 4)
	{
		int a = base64_digit((unsigned char)text[offset]);
		int b = base64_digit((unsigned char)text[offset + 1]);
		int c = text[offset + 2] == '=' ? 0 : base64_digit((unsigned char)text[offset + 2]);
		int d = text[offset + 3] == '=' ? 0 : base64_digit((unsigned char)text[offset + 3]);
		int padding = text[offset + 3] == '=' ? (text[offset + 2] == '=' ? 2 : 1) : 0;
		int size = 3 - padding;
		if (a < 0 || b < 0 || c < 0 || d < 0 || count + size > maximum ||
			(text[offset + 2] == '=' && text[offset + 3] != '=') ||
			(padding && offset + 4 != length) || (padding == 2 && (b & 15)) ||
			(padding == 1 && (c & 3))) return -1;
		bytes[count++] = (unsigned char)((a << 2) | (b >> 4));
		if (size > 1) bytes[count++] = (unsigned char)((b << 4) | (c >> 2));
		if (size > 2) bytes[count++] = (unsigned char)((c << 6) | d);
	}
	return count;
}

static int master_listing_signed(const unsigned char *bytes, int size)
{
	static const char label[] = "hceu-lobby-1";
	unsigned char message[sizeof(label) - 1 + MASTER_MAXIMUM_LISTING];
	if (size < 140 || size > MASTER_MAXIMUM_LISTING || bytes[0] != 'H' || bytes[1] != 'L' || bytes[2] != 1)
		return 0;
	memcpy(message, label, sizeof(label) - 1);
	memcpy(message + sizeof(label) - 1, bytes, (size_t)(size - 64));
	return crypto_ed25519_check(bytes + size - 64, bytes + 14, message, sizeof(label) - 1 + (size_t)(size - 64)) == 0;
}

static int master_read_game(struct master_json object, struct master_game *game)
{
	struct master_json value;
	char encoded[4 * ((MASTER_MAXIMUM_LISTING + 2) / 3) + 1];
	int index, length;
	memset(game, 0, sizeof(*game));
	if (!json_member(object, "id", &value) || json_string(&value, game->id, sizeof(game->id)) != 16) return 0;
	for (index = 0; index < 16; index++)
	{
		int digit = hex_digit((unsigned char)game->id[index]);
		if (digit < 0) return 0;
		game->id[index] = "0123456789abcdef"[digit];
	}
	/* (the operator's region is unsigned display text: missing or null is none) */
	if (json_member(object, "region", &value) && !(value.end - value.position == 4 && !memcmp(value.position, "null", 4)) &&
		json_text(&value, game->region, sizeof(game->region), 1) < 0) return 0;
	if (!json_member(object, "updated", &value) || !json_unsigned(value, &game->updated) || !game->updated) return 0;
	if (!json_member(object, "listing", &value)) return 0;
	length = json_string(&value, encoded, sizeof(encoded));
	if (length <= 0 || length >= (int)sizeof(encoded)) return 0;
	game->listing_size = decode_base64(encoded, game->listing, sizeof(game->listing));
	return master_listing_signed(game->listing, game->listing_size);
}

/* Private seam: callers must authenticate the exact body first. Kept static so
no network caller can accidentally bypass the official pin. */
static int master_parse_authenticated(const char *body, int size, long long now,
	struct master_game *games, int maximum, char *error, int error_size)
{
	struct master_json root, check, master, value, servers;
	unsigned long long protocol, timestamp;
	char key[32];
	int count = 0;
	if (error && error_size > 0) error[0] = 0;
	if (!body || size <= 0 || size > MASTER_MAXIMUM_ANSWER || !games || maximum <= 0)
		return master_error(error, error_size, "invalid master response size or destination");
	root.position = body;
	root.end = body + size;
	check = root;
	if (!json_value(&check, 0)) return master_error(error, error_size, "malformed master JSON");
	json_space(&check);
	if (check.position != check.end) return master_error(error, error_size, "trailing master JSON data");
	if (!json_member(root, "protocol", &value) || !json_unsigned(value, &protocol) || protocol != 1)
		return master_error(error, error_size, "unsupported master protocol");
	if (!json_member(root, "master", &master) || !json_member(master, "key_id", &value) ||
		json_string(&value, key, sizeof(key)) != 16 || strcmp(key, master_key_id))
		return master_error(error, error_size, "master body key id mismatch");
	if (!json_member(master, "time", &value) || !json_unsigned(value, &timestamp) || timestamp > LLONG_MAX)
		return master_error(error, error_size, "invalid master time");
	if (now < 0 || (timestamp > (unsigned long long)now ? timestamp - (unsigned long long)now : (unsigned long long)now - timestamp) > 300)
		return master_error(error, error_size, "master clock mismatch: response time differs by more than 300 seconds");
	if (!json_member(root, "servers", &servers) || !json_take(&servers, '['))
		return master_error(error, error_size, "master servers must be an array");
	if (maximum > MASTER_MAXIMUM_GAMES) maximum = MASTER_MAXIMUM_GAMES;
	if (json_take(&servers, ']')) return 0;
	for (;;)
	{
		struct master_json entry = servers;
		struct master_game game;
		int index;
		if (!json_value(&servers, 1)) return master_error(error, error_size, "malformed master entry");
		entry.end = servers.position;
		if (master_read_game(entry, &game))
		{
			for (index = 0; index < count; index++)
				if (!strcmp(games[index].id, game.id)) break;
			if (index < count)
			{
				if (game.updated > games[index].updated) games[index] = game;
			}
			else if (count < maximum) games[count++] = game;
		}
		if (json_take(&servers, ']')) return count;
		if (!json_take(&servers, ',')) return master_error(error, error_size, "malformed master array");
	}
}

int master_parse_list(const char *body, int size, const char *signature,
	const char *signature_key, long long now, struct master_game *games,
	int maximum, char *error, int error_size)
{
	unsigned char bytes[64];
	if (error && error_size > 0) error[0] = 0;
	if (!body || size <= 0 || size > MASTER_MAXIMUM_ANSWER)
		return master_error(error, error_size, "invalid master response size");
	if (!signature_key || strcmp(signature_key, master_key_id))
		return master_error(error, error_size, "master Signature-Key mismatch");
	if (!signature || strncmp(signature, "ed25519=", 8) || strlen(signature) != 96 ||
		decode_base64(signature + 8, bytes, sizeof(bytes)) != 64 ||
		crypto_ed25519_check(bytes, master_public_key, (const unsigned char *)body, (size_t)size))
		return master_error(error, error_size, "master signature verification failed");
	return master_parse_authenticated(body, size, now, games, maximum, error, error_size);
}
