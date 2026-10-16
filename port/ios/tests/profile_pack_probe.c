/* Downloaded snapshots must be wholly valid, not merely readable in parts. */
#include "host_profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[128];
static void put(const char *name, const char *text)
{
	char path[256]; snprintf(path, sizeof(path), "%s/%s", root, name);
	FILE *f = fopen(path, "wb"); assert(f); fputs(text, f); fclose(f);
}
int main(int argc, char **argv)
{
	assert(argc == 2 && host_profile_validate_folder(argv[1], NULL));
	snprintf(root, sizeof(root), "/tmp/profile-pack-XXXXXX"); assert(mkdtemp(root));
	char path[256]; snprintf(path, sizeof(path), "%s/hardware", root); assert(!mkdir(path, 0700));
	put("hardware/catalog.toml", "schema=1\nversion=\"test\"\nhardware=[]\nfallback=[]\n");
	const char *platforms[] = { "ios", "tvos", "visionos", "mac" };
	for (int i = 0; i < 4; i++)
	{
		char name[48], text[256]; snprintf(name, sizeof(name), "%s.toml", platforms[i]);
		snprintf(text, sizeof(text), "schema=1\nname=\"Class\"\nkind=\"class\"\n[match]\nplatform=\"%s\"\ngpu_family=[1,0]\n", platforms[i]); put(name, text);
	}
	assert(host_profile_validate_folder(root, NULL));
	put("bad.toml", "schema=1\nname=\"x\\ndisplay.render_height\\t99999\\tpack\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"Phone1,1\"]\n[display]\nrender_height=720\n");
	assert(!host_profile_validate_folder(root, NULL));
	put("bad.toml", "schema=1\nname=\"Bad\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"Phone1,1\"]\n[display]\nrender_height=-1\n");
	assert(!host_profile_validate_folder(root, NULL));
	put("bad.toml", "schema=1\nname=\"Bad\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"Phone1,1\"]\n[network]\nonline=true\n");
	assert(!host_profile_validate_folder(root, NULL));
	put("bad.toml", "schema=1\nname=\"Bad\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"Phone1,1\"]\n");
	assert(host_profile_validate_folder(root, NULL));
	put("bad.toml", "schema=1\nname=\"External\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"Phone1,1\"]\n[external.display]\noutput_height=1080\nrender_height=720\n[external.scene.cutscene.display]\nrender_scale=0.75\n");
	assert(host_profile_validate_folder(root, NULL));
	put("bad.toml", "schema=1\nname=\"External\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"Phone1,1\"]\n[external.display]\nrenderer=\"metal\"\n");
	assert(!host_profile_validate_folder(root, NULL));
	put("bad.toml", "schema=1\nname=\"External\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"Phone1,1\"]\n[external.display]\nrender_height=720\n");
	put("duplicate.toml", "schema=1\nname=\"Duplicate\"\nkind=\"model\"\n[match]\nplatform=\"ios\"\nmodels=[\"Phone1,1\"]\n");
	assert(!host_profile_validate_folder(root, NULL));
	snprintf(path, sizeof(path), "%s/duplicate.toml", root); unlink(path);
	put("hardware/catalog.toml", "schema=3\n"); assert(!host_profile_validate_folder(root, NULL));
	put("hardware/catalog.toml", "schema=1\nversion=\"test\"\nhardware=[]\nfallback=[]\n");
	snprintf(path, sizeof(path), "%s/link.toml", root); assert(!symlink("bad.toml", path));
	assert(!host_profile_validate_folder(root, NULL)); unlink(path);
	snprintf(path, sizeof(path), "%s/ios.toml", root); unlink(path);
	assert(!host_profile_validate_folder(root, NULL));
	/* Temporary named fixtures only; do not remove a shared directory. */
	const char *names[] = { "bad.toml", "tvos.toml", "visionos.toml", "mac.toml", "hardware/catalog.toml" };
	for (int i = 0; i < 5; i++) { snprintf(path, sizeof(path), "%s/%s", root, names[i]); unlink(path); }
	snprintf(path, sizeof(path), "%s/hardware", root); rmdir(path); rmdir(root);
	/* a bundled or drop-in profile's name reaches the settings encoding through each value's source */
	{
		struct host_profile profile = {0};
		char encoded[512];
		snprintf(profile.values[0].name, sizeof(profile.values[0].name), "display.render_height");
		snprintf(profile.values[0].text, sizeof(profile.values[0].text), "720");
		snprintf(profile.values[0].source, sizeof(profile.values[0].source), "model x\ndisplay.fov\t1\tpack\x7f");
		profile.count = 1;
		host_profile_encode(&profile, encoded, sizeof(encoded));
		assert(!strcmp(encoded, "display.render_height\t720\tmodel x display.fov 1 pack \n"));
	}
	puts("PASS: complete profile pack validation");
	return 0;
}
