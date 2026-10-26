/* texture_policy.c on hand-made bitmaps: the same cases as step-tools/upscale_lib/tests/test_policy.py */
#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "texture_policy.h"

static const char *const names[] = { "decal", "decals", "display", "monitor", "monitors", "registry", "keypad",
	"numbers" };
static const char *const groups[] = { "smet" };
static const char *const prefixes[] = { "effects\\decals\\" };

static const char *const materials[] = { "rock", "cliff", "tech", "metal" };

/* every damage test turned off (-1, as tools/embed_texture_policy.py writes a null), so a test that reuses this table
does not inherit a zero share, which would call every graphic damaged; no material names (material_rule adds them) */
static struct texture_policy_table table(const struct texture_policy_override *overrides, size_t count)
{
	struct texture_policy_table t = { 1, 1, TEXTURE_POLICY_S4G, TEXTURE_POLICY_BPF, TEXTURE_POLICY_BPF,
		TEXTURE_POLICY_BPF, 12, 30, 1, 3, names, 8, groups, 1, prefixes, 1, overrides, count, -1, -1, -1, 1,
		NULL, 0, TEXTURE_POLICY_S4G };
	return t;
}

static char logged[4096];
static void log_line(void *context, const char *line)
{
	(void)context;
	strncat(logged, line, sizeof(logged) - strlen(logged) - 2);
	strcat(logged, "\n");
}

static void add(struct texture_policy_catalog *c, const char *tag, int index, uint64_t hash, uint32_t size,
	int format, int usage, int type, const char *group, const char *slot, const char *shader)
{
	struct texture_policy_reference ref;
	struct texture_policy_bitmap b;

	memset(&ref, 0, sizeof(ref));
	snprintf(ref.group, sizeof(ref.group), "%s", group);
	snprintf(ref.slot, sizeof(ref.slot), "%s", slot);
	snprintf(ref.shader, sizeof(ref.shader), "%s", shader);
	memset(&b, 0, sizeof(b));
	b.tag = tag; b.index = index; b.has_hash = type == 0; b.hash = hash; b.width = b.height = size;
	b.type = type; b.format = format; b.flags = 0x1; b.usage = usage; b.refs = &ref; b.ref_count = 1;
	assert(texture_policy_add_bitmap(c, &b) == 0);
}

static const struct texture_policy_decision *find(const struct texture_policy_decision *d, size_t n, uint64_t hash,
	uint32_t size)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (d[i].hash == hash && d[i].width == size)
			return &d[i];
	assert(!"no decision");
	return NULL;
}

#define DXT1 14
#define DXT5 16
#define P8 17

static void surfaces_companions_bumps_and_signals(void)
{
	struct texture_policy_catalog *c = texture_policy_catalog_new();
	struct texture_policy_table t = table(NULL, 0);
	struct texture_policy_decision *d;
	size_t n;

	add(c, "levels\\a10\\bitmaps\\panel generic", 0, 1, 64, DXT1, 1, 0, "senv", "env.base", "shaders\\wall");
	add(c, "levels\\a10\\bitmaps\\panel glow", 0, 2, 64, DXT1, 1, 0, "senv", "env.self_illumination", "shaders\\wall");
	add(c, "levels\\a10\\bitmaps\\panel generic bump", 0, 3, 64, P8, 2, 0, "senv", "env.bump", "shaders\\wall");
	add(c, "levels\\a10\\bitmaps\\poa-registry", 0, 4, 64, DXT1, 1, 0, "senv", "env.base", "shaders\\hull");
	add(c, "levels\\a10\\decals\\bitmaps\\caution current", 0, 5, 64, DXT1, 1, 0, "deca", "deca", "decals\\c");
	add(c, "effects\\decals\\bitmaps\\bullet hole", 0, 6, 64, DXT1, 1, 0, "deca", "deca", "effects\\decals\\h");
	add(c, "vehicles\\warthog\\bitmaps\\speedo", 0, 7, 64, DXT1, 1, 0, "smet", "smet", "shaders\\speedo");
	add(c, "levels\\a10\\bitmaps\\decal2 arrows", 0, 8, 64, DXT1, 1, 0, "senv", "env.base", "shaders\\floor");
	add(c, "levels\\a10\\bitmaps\\lightmap", 0, 9, 64, 6, 4, 0, "sbsp", "sbsp", "levels\\a10\\a10");
	add(c, "levels\\a10\\bitmaps\\panel 16", 0, 10, 64, 9, 1, 0, "senv", "env.base", "shaders\\p16");
	add(c, "levels\\a10\\bitmaps\\panel 16 glow", 0, 11, 64, DXT1, 1, 0, "senv", "env.self_illumination",
		"shaders\\p16");
	n = texture_policy_classify(c, &t, &d, NULL, NULL);
	assert(n == 11);
	assert(find(d, n, 1, 64)->kind == TEXTURE_POLICY_SURFACE && find(d, n, 1, 64)->result == TEXTURE_POLICY_BPF);
	assert(find(d, n, 2, 64)->kind == TEXTURE_POLICY_COMPANION && find(d, n, 2, 64)->result == TEXTURE_POLICY_BPF);
	assert(find(d, n, 3, 64)->kind == TEXTURE_POLICY_BUMP && find(d, n, 3, 64)->result == TEXTURE_POLICY_BPF);
	assert(find(d, n, 4, 64)->result == TEXTURE_POLICY_S4G);
	assert(find(d, n, 5, 64)->kind == TEXTURE_POLICY_GRAPHIC);
	assert(find(d, n, 6, 64)->kind == TEXTURE_POLICY_SURFACE);
	assert(find(d, n, 7, 64)->kind == TEXTURE_POLICY_GRAPHIC);
	assert(find(d, n, 8, 64)->kind == TEXTURE_POLICY_GRAPHIC);
	assert(find(d, n, 9, 64)->result == TEXTURE_POLICY_ORIGINAL);
	assert(find(d, n, 10, 64)->result == TEXTURE_POLICY_ORIGINAL);   /* a4r4g4b4: not decoded yet */
	assert(find(d, n, 11, 64)->result == TEXTURE_POLICY_ORIGINAL);   /* its base map is original */
	free(d);
	texture_policy_catalog_free(c);
}

static void pins(void)
{
	static const struct texture_policy_override conflict[] = {
		{ "levels\\a10\\bitmaps\\tech rack mount", -1, NULL, TEXTURE_POLICY_S4G },
		{ "levels\\c40\\bitmaps\\tech rack mount copy", -1, NULL, TEXTURE_POLICY_BPF },
		{ "levels\\a10\\bitmaps\\wall", 0, "00000000000000aa", TEXTURE_POLICY_ORIGINAL },
		{ "levels\\a10\\bitmaps\\black big", -1, NULL, TEXTURE_POLICY_ORIGINAL },
		{ "levels\\x\\bitmaps\\gone", -1, NULL, TEXTURE_POLICY_S4G },
	};
	struct texture_policy_catalog *c = texture_policy_catalog_new();
	struct texture_policy_table t = table(conflict, 5);
	struct texture_policy_decision *d;
	size_t n;

	add(c, "levels\\a10\\bitmaps\\tech rack mount", 0, 0xaa1, 64, DXT1, 1, 0, "senv", "env.base", "shaders\\r");
	add(c, "levels\\c40\\bitmaps\\tech rack mount copy", 0, 0xaa1, 64, DXT1, 1, 0, "senv", "env.base", "shaders\\r2");
	add(c, "levels\\a10\\bitmaps\\wall", 0, 0xbb, 64, DXT1, 1, 0, "senv", "env.base", "shaders\\w");
	add(c, "levels\\a10\\bitmaps\\wall glow", 0, 0xbc, 64, DXT1, 1, 0, "senv", "env.self_illumination", "shaders\\w");
	add(c, "levels\\a10\\bitmaps\\black", 0, 0xcc, 16, DXT1, 1, 0, "senv", "env.base", "shaders\\a");
	add(c, "levels\\a10\\bitmaps\\black big", 0, 0xcc, 64, DXT1, 1, 0, "senv", "env.base", "shaders\\b");
	logged[0] = 0;
	n = texture_policy_classify(c, &t, &d, log_line, NULL);
	assert(n == 5);
	assert(find(d, n, 0xaa1, 64)->result == TEXTURE_POLICY_BPF);      /* original, then bpf, then s4g */
	assert(find(d, n, 0xbb, 64)->result == TEXTURE_POLICY_ORIGINAL);  /* a stale hash still pins by path */
	assert(find(d, n, 0xbc, 64)->result == TEXTURE_POLICY_ORIGINAL);  /* and takes its glow with it */
	assert(find(d, n, 0xcc, 16)->result == TEXTURE_POLICY_BPF);       /* same hash, other size: its own entry */
	assert(find(d, n, 0xcc, 64)->result == TEXTURE_POLICY_ORIGINAL);
	assert(strstr(logged, "00000000000000bb") && strstr(logged, "00000000000000aa"));
	assert(strstr(logged, "gone") && strstr(logged, "matches no bitmap"));
	free(d);
	texture_policy_catalog_free(c);
}

/* tag and shader paths fold ASCII case only, whatever the host's locale: latin-1 letters compare as bytes */
static void ascii_case_and_pin_logs(void)
{
	static const struct texture_policy_override pinned[] = {
		{ "LEVELS\\A10\\BITMAPS\\Panel", -1, NULL, TEXTURE_POLICY_ORIGINAL },
		{ "levels\\a10\\bitmaps\\\xe9" "cran", -1, NULL, TEXTURE_POLICY_ORIGINAL },
		{ "levels\\x\\bitmaps\\gone", 3, NULL, TEXTURE_POLICY_S4G },
	};
	struct texture_policy_catalog *c = texture_policy_catalog_new();
	struct texture_policy_table t = table(pinned, 3);
	struct texture_policy_decision *d;
	size_t n;

	setlocale(LC_ALL, "en_US.ISO8859-1");
	add(c, "levels\\a10\\bitmaps\\panel", 0, 0x21, 64, DXT1, 1, 0, "senv", "env.base", "Shaders\\Panel");
	add(c, "levels\\a10\\bitmaps\\panel glow", 0, 0x22, 64, DXT1, 1, 0, "senv", "env.self_illumination",
		"shaders\\panel");
	add(c, "levels\\a10\\bitmaps\\\xc9" "cran", 0, 0x23, 64, DXT1, 1, 0, "senv", "env.base", "shaders\\\xc9");
	add(c, "levels\\a10\\bitmaps\\\xc9" "cran glow", 0, 0x24, 64, DXT1, 1, 0, "senv", "env.self_illumination",
		"shaders\\\xe9");
	logged[0] = 0;
	n = texture_policy_classify(c, &t, &d, log_line, NULL);
	setlocale(LC_ALL, "C");
	assert(n == 4);
	assert(find(d, n, 0x21, 64)->result == TEXTURE_POLICY_ORIGINAL);   /* ASCII case folds */
	assert(find(d, n, 0x22, 64)->result == TEXTURE_POLICY_ORIGINAL);   /* and so does the shader path */
	assert(find(d, n, 0x23, 64)->result == TEXTURE_POLICY_BPF);        /* \xc9 is not \xe9 */
	assert(find(d, n, 0x24, 64)->result == TEXTURE_POLICY_BPF);        /* nor is its shader */
	assert(strstr(logged, "\xe9" "cran: matches no bitmap"));
	assert(strstr(logged, "override levels\\x\\bitmaps\\gone [3]: matches no bitmap"));   /* as policy.py words it */
	free(d);
	texture_policy_catalog_free(c);
}

/* the shipped policy's "damage" block, embedded: the device carries the damage rule's parameters (piece 3 applies
them); a test turned off (null) is a negative threshold */
static void embedded_damage_block(void)
{
	const struct texture_policy_table *t = &texture_policy_embedded;

	assert(t->damage_share == 0.10f);
	assert(t->damage_structure_loss < 0 && t->damage_bpf_structure_loss < 0);
	assert(t->damage_measure_version == 1);
	assert(t->low == 12 && t->high == 30 && t->sigma == 1 && t->iterations == 3);
}

/* the material rule (policy.py _materials): a surface or bump named for a solid material gets material_result; a
graphic, a companion, a pin and a bump whose base map is original do not */
static void material_rule(void)
{
	static const struct texture_policy_override pinned[] = {
		{ "levels\\b30\\bitmaps\\metal kept", -1, NULL, TEXTURE_POLICY_BPF },
		{ "levels\\b30\\bitmaps\\dark rock", -1, NULL, TEXTURE_POLICY_ORIGINAL },
	};
	struct texture_policy_catalog *c = texture_policy_catalog_new();
	struct texture_policy_table t = table(pinned, 2);
	struct texture_policy_decision *d;
	size_t n;

	t.material_names = materials;
	t.material_name_count = 4;
	add(c, "levels\\b30\\bitmaps\\cliff rock", 0, 0x31, 64, DXT1, 1, 0, "senv", "env.base", "shaders\\cliff");
	add(c, "levels\\b30\\bitmaps\\cliff rock bump", 0, 0x32, 64, P8, 2, 0, "senv", "env.bump", "shaders\\cliff");
	add(c, "levels\\b30\\bitmaps\\sand2", 0, 0x33, 64, DXT1, 1, 0, "senv", "env.base", "shaders\\sand");
	add(c, "levels\\b30\\bitmaps\\metal registry", 0, 0x34, 64, DXT1, 1, 0, "senv", "env.base", "shaders\\sign");
	add(c, "levels\\b30\\bitmaps\\tech glow", 0, 0x35, 64, DXT1, 1, 0, "senv", "env.self_illumination",
		"shaders\\cliff");
	add(c, "levels\\b30\\bitmaps\\metal kept", 0, 0x36, 64, DXT1, 1, 0, "senv", "env.base", "shaders\\kept");
	add(c, "levels\\b30\\bitmaps\\dark rock", 0, 0x37, 64, DXT1, 1, 0, "senv", "env.base", "shaders\\dark");
	add(c, "levels\\b30\\bitmaps\\rock2 bump", 0, 0x38, 64, P8, 2, 0, "senv", "env.bump", "shaders\\dark");
	add(c, "levels\\b30\\bitmaps\\Tech_Floor", 0, 0x39, 64, DXT1, 1, 0, "senv", "env.base", "shaders\\tf");
	n = texture_policy_classify(c, &t, &d, NULL, NULL);
	assert(n == 9);
	assert(find(d, n, 0x31, 64)->kind == TEXTURE_POLICY_SURFACE && find(d, n, 0x31, 64)->result == TEXTURE_POLICY_S4G);
	assert(find(d, n, 0x32, 64)->kind == TEXTURE_POLICY_BUMP && find(d, n, 0x32, 64)->result == TEXTURE_POLICY_S4G);
	assert(find(d, n, 0x33, 64)->result == TEXTURE_POLICY_BPF);                /* sand is not in this list */
	assert(find(d, n, 0x34, 64)->kind == TEXTURE_POLICY_GRAPHIC);              /* a graphic signal comes first */
	assert(find(d, n, 0x35, 64)->kind == TEXTURE_POLICY_COMPANION && find(d, n, 0x35, 64)->result == TEXTURE_POLICY_BPF);
	assert(find(d, n, 0x36, 64)->result == TEXTURE_POLICY_BPF);                /* a pin wins */
	assert(find(d, n, 0x38, 64)->result == TEXTURE_POLICY_ORIGINAL);           /* its base map is original */
	assert(find(d, n, 0x39, 64)->result == TEXTURE_POLICY_S4G);                /* case folds, '_' separates */
	t.material_result = TEXTURE_POLICY_BPF;
	free(d);
	n = texture_policy_classify(c, &t, &d, NULL, NULL);
	assert(find(d, n, 0x31, 64)->result == TEXTURE_POLICY_BPF && find(d, n, 0x32, 64)->result == TEXTURE_POLICY_BPF);
	free(d);
	texture_policy_catalog_free(c);
}

static void embedded_material_block(void)
{
	const struct texture_policy_table *t = &texture_policy_embedded;
	size_t i;
	int rock = 0;

	for (i = 0; i < t->material_name_count; i++)
		rock |= !strcmp(t->material_names[i], "rock");
	assert(rock && t->material_result == TEXTURE_POLICY_S4G);
}

static void hand_built_table_turns_damage_off(void)
{
	struct texture_policy_table t = table(NULL, 0);

	assert(t.damage_share < 0 && t.damage_structure_loss < 0 && t.damage_bpf_structure_loss < 0);
	assert(t.damage_measure_version == 1);
}

/* Steve, 2026-10-26: the main menu's art (the big HALO logo, ui\\shell\\...) is upscaled; the in-game HUD (ui\\hud\\...,
   replaced by its own high-resolution art) and bitmaps in formats the upscaler cannot read stay original */
static void main_menu_art(void)
{
	struct texture_policy_catalog *c = texture_policy_catalog_new();
	struct texture_policy_table t = table(NULL, 0);
	struct texture_policy_decision *d;
	const struct texture_policy_decision *logo, *list, *hud, *plain;
	size_t n;

	add(c, "ui\\shell\\main_menu\\halo_logo", 0, 0x21, 64, DXT5, 1, 0, "DeLa", "DeLa", "");
	add(c, "ui\\shell\\bitmaps\\list_field", 0, 0x22, 64, DXT5, 1, 0, "DeLa", "DeLa", "");
	add(c, "ui\\hud\\bitmaps\\hud_sweeper", 0, 0x23, 64, DXT5, 1, 0, "matg", "matg", "");
	add(c, "ui\\shell\\bitmaps\\gradient", 0, 0x24, 64, 11, 1, 0, "DeLa", "DeLa", "");
	n = texture_policy_classify(c, &t, &d, NULL, NULL);
	assert(n == 4);
	logo = find(d, n, 0x21, 64);
	list = find(d, n, 0x22, 64);
	hud = find(d, n, 0x23, 64);
	plain = find(d, n, 0x24, 64);
	assert(logo->result == TEXTURE_POLICY_S4G && logo->kind == TEXTURE_POLICY_GRAPHIC && !logo->wrap);
	assert(list->result == TEXTURE_POLICY_S4G && list->kind == TEXTURE_POLICY_GRAPHIC);
	assert(hud->result == TEXTURE_POLICY_ORIGINAL);
	assert(plain->result == TEXTURE_POLICY_ORIGINAL);
	free(d);
	texture_policy_catalog_free(c);
}

int main(void)
{
	main_menu_art();
	embedded_damage_block();
	hand_built_table_turns_damage_off();
	surfaces_companions_bumps_and_signals();
	pins();
	ascii_case_and_pin_logs();
	material_rule();
	embedded_material_block();
	puts("texture_policy_probe: ok");
	return 0;
}
