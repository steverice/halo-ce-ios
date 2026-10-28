/* The authenticated-body seam stays private: synthetic envelopes never alter
production's immutable public-key pin. This translation unit includes the parser
so it can exercise entry validation with saved host-signed listings. */
#include "../../linux/src/master_list.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "line %d: %s\n", __LINE__, #test); return 1; } } while (0)

static char *load(const char *folder, const char *name, int *size)
{
	char path[1024];
	FILE *file;
	char *body;
	snprintf(path, sizeof(path), "%s/%s", folder, name);
	file = fopen(path, "rb");
	if (!file) return NULL;
	fseek(file, 0, SEEK_END);
	*size = (int)ftell(file);
	rewind(file);
	body = malloc((size_t)*size + 1);
	if (!body || fread(body, 1, (size_t)*size, file) != (size_t)*size) exit(2);
	fclose(file);
	body[*size] = 0;
	return body;
}

int main(int argc, char **argv)
{
	static const char signature[] = "ed25519=j5C8DsZ6SQ0A+9jlChcsFqvS0R713ILSl1PvOBYdPdkX9EOuN/6RGHIjpu+gvV+4BIYcznicNfwVtAlXLvl2CA==";
	struct master_game games[MASTER_MAXIMUM_GAMES + 1];
	char error[160];
	char *empty, *populated;
	int size, populated_size, count, index, length;
	char listing[400], entry[1024], body[400000];
	const char *first, *end;
	CHECK(argc == 2);
	empty = load(argv[1], "official-empty.json", &size);
	populated = load(argv[1], "official-populated.json", &populated_size);
	CHECK(empty && populated);
	CHECK(master_parse_list(empty, size, signature, "fe290a97f7ab592b", 1792961972, games, 512, error, sizeof(error)) == 0);
	CHECK(!error[0]);
	CHECK(master_parse_list(empty, size, NULL, "fe290a97f7ab592b", 1792961972, games, 512, error, sizeof(error)) == -1);
	CHECK(master_parse_list(empty, size, signature, NULL, 1792961972, games, 512, error, sizeof(error)) == -1);
	empty[2] ^= 1;
	CHECK(master_parse_list(empty, size, signature, "fe290a97f7ab592b", 1792961972, games, 512, error, sizeof(error)) == -1);
	empty[2] ^= 1;
	CHECK(master_parse_list(empty, size, signature, "fe290a97f7ab592b", 1792962273, games, 512, error, sizeof(error)) == -1);
	CHECK(strstr(error, "clock"));
	CHECK(master_parse_list(empty, size, signature, "fe290a97f7ab592b", 1792961671, games, 512, error, sizeof(error)) == -1);
	CHECK(master_parse_list(empty, size, signature, "fe290a97f7ab592b", 1792961672, games, 512, error, sizeof(error)) == 0);
	CHECK(master_parse_list(empty, size, signature, "fe290a97f7ab592b", 1792962272, games, 512, error, sizeof(error)) == 0);
	CHECK(master_parse_list(empty, size, signature, "bad", 1792961972, games, 512, error, sizeof(error)) == -1);
	{
		static const char *const protocols[] = { "true", "1.0", "1e0", "-1", "2", "null", "\"1\"" };
		static const char *const times[] = { "true", "1793142772.0", "-1", "18446744073709551616" };
		for (index = 0; index < (int)(sizeof(protocols) / sizeof(protocols[0])); index++)
		{
			length = snprintf(body, sizeof(body), "{\"protocol\":%s,\"master\":{\"key_id\":\"fe290a97f7ab592b\",\"time\":1793142772},\"servers\":[]}", protocols[index]);
			CHECK(master_parse_authenticated(body, length, 1793142772, games, 512, error, sizeof(error)) == -1);
		}
		for (index = 0; index < (int)(sizeof(times) / sizeof(times[0])); index++)
		{
			length = snprintf(body, sizeof(body), "{\"protocol\":1,\"master\":{\"key_id\":\"fe290a97f7ab592b\",\"time\":%s},\"servers\":[]}", times[index]);
			CHECK(master_parse_authenticated(body, length, 1793142772, games, 512, error, sizeof(error)) == -1);
		}
	}
	count = master_parse_authenticated(populated, populated_size, 1793142772, games, 512, error, sizeof(error));
	CHECK(count == 23);
	CHECK(!strcmp(games[0].id, "e96ba7f6d35578aa"));
	CHECK(!strcmp(games[0].region, "eu"));
	/* Each valid listing below comes from the captured host, while the
	master envelope is deliberately synthetic and enters after verification. */
	first = strstr(populated, "\"listing\":\"");
	CHECK(first);
	first += strlen("\"listing\":\"");
	end = strchr(first, '"');
	CHECK(end && end - first < (int)sizeof(listing));
	memcpy(listing, first, (size_t)(end - first));
	listing[end - first] = 0;
	snprintf(entry, sizeof(entry), "{\"id\":\"0123456789ABCDEF\",\"region\":\"n\\u0000a\\t-\\u007feast\",\"updated\":2,\"listing\":\"%s\"}", listing);
	length = snprintf(body, sizeof(body), "{\"protocol\":1,\"master\":{\"key_id\":\"fe290a97f7ab592b\",\"time\":1793142772},\"servers\":[null,{\"id\":true},%s]}", entry);
	CHECK(master_parse_authenticated(body, length, 1793142772, games, 512, error, sizeof(error)) == 1);
	CHECK(!strcmp(games[0].region, "na-east"));
	CHECK(!strcmp(games[0].id, "0123456789abcdef"));
	/* (an operator's missing or null region is no region, not a bad entry) */
	{
		char bare[1024];
		int bare_length;

		snprintf(entry, sizeof(entry), "{\"id\":\"0123456789abcdef\",\"updated\":2,\"listing\":\"%s\"},"
			"{\"id\":\"0123456789abcde0\",\"region\":null,\"updated\":2,\"listing\":\"%s\"}", listing, listing);
		bare_length = snprintf(bare, sizeof(bare), "{\"protocol\":1,\"master\":{\"key_id\":\"fe290a97f7ab592b\",\"time\":1793142772},\"servers\":[%s]}", entry);
		CHECK(bare_length < (int)sizeof(bare));
		CHECK(master_parse_authenticated(bare, bare_length, 1793142772, games, 512, error, sizeof(error)) == 2);
		CHECK(!games[0].region[0] && !games[1].region[0]);
	}
	snprintf(entry, sizeof(entry), "{\"id\":\"0123456789ABCDEF\",\"region\":\"n\\u0000a\\t-\\u007feast\",\"updated\":2,\"listing\":\"%s\"}", listing);
	body[12] = '2';
	CHECK(master_parse_authenticated(body, length, 1793142772, games, 512, error, sizeof(error)) == -1);
	body[12] = '1';
	first = strstr(body, "fe290");
	body[first - body] = '0';
	CHECK(master_parse_authenticated(body, length, 1793142772, games, 512, error, sizeof(error)) == -1);
	length = snprintf(body, sizeof(body), "{\"protocol\":1,\"master\":{\"key_id\":\"fe290a97f7ab592b\",\"time\":1793142772},\"servers\":[");
	for (index = 0; index < 514; index++)
		length += snprintf(body + length, sizeof(body) - (size_t)length, "%s{\"id\":\"%016x\",\"region\":\"eu\",\"updated\":1,\"listing\":\"%s\"}", index ? "," : "", index, listing);
	length += snprintf(body + length, sizeof(body) - (size_t)length, ", {\"id\":\"0000000000000000\",\"region\":\"new\",\"updated\":3,\"listing\":\"%s\"}]}", listing);
	CHECK(master_parse_authenticated(body, length, 1793142772, games, 513, error, sizeof(error)) == 512);
	CHECK(games[0].updated == 3 && !strcmp(games[0].region, "new"));
	{
		static const char *const bad_fields[] = {
			"\"id\":\"0123456789abcdef0\",\"region\":\"eu\",\"updated\":1",
			"\"id\":\"0000000000000001\",\"region\":false,\"updated\":1",
			"\"id\":\"0000000000000001\",\"region\":\"eu\",\"updated\":true",
			"\"id\":\"0000000000000001\",\"region\":\"eu\",\"updated\":1.0",
			"\"id\":\"0000000000000001\",\"region\":\"eu\",\"updated\":0",
			"\"id\":\"0000000000000001\",\"region\":\"eu\",\"updated\":-1",
		};
		for (index = 0; index < (int)(sizeof(bad_fields) / sizeof(bad_fields[0])); index++)
		{
			length = snprintf(body, sizeof(body), "{\"protocol\":1,\"master\":{\"key_id\":\"fe290a97f7ab592b\",\"time\":1793142772},\"servers\":[{%s,\"listing\":\"%s\"},%s]}", bad_fields[index], listing, entry);
			CHECK(master_parse_authenticated(body, length, 1793142772, games, 512, error, sizeof(error)) == 1);
			CHECK(!strcmp(games[0].id, "0123456789abcdef"));
		}
	}
	/* A malformed host signature must skip only its own entry. */
	listing[100] = listing[100] == 'A' ? 'B' : 'A';
	length = snprintf(body, sizeof(body), "{\"protocol\":1,\"master\":{\"key_id\":\"fe290a97f7ab592b\",\"time\":1793142772},\"servers\":[{\"id\":\"1111111111111111\",\"region\":\"eu\",\"updated\":1,\"listing\":\"%s\"},%s]}", listing, entry);
	CHECK(master_parse_authenticated(body, length, 1793142772, games, 512, error, sizeof(error)) == 1);
	CHECK(!strcmp(games[0].id, "0123456789abcdef"));
	free(empty);
	free(populated);
	puts("master list parser passed");
	return 0;
}
