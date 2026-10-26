/* The Settings app's texture pane (Settings.bundle): the "Upscale textures" switch, the size of the upscaled
   textures, and a switch that deletes them, acted on at launch and whenever the app comes back to the foreground. */
#import <UIKit/UIKit.h>
#include "host_config.h"
#include "host_texture_settings.h"
#include "texture_cache.h"
#include "host_texture_upscale.h"

static NSString *cache_directory(const char *data_root)
{
	char path[1024];

	host_texture_cache_path(data_root, path, sizeof(path));
	return @(path);
}

/* publishes the cache's size for the Settings app to show */
/* the data root, where the prepared original textures (astc-cache) are */
static NSString *prepared_root;

static void publish_size(NSString *directory)
{
	NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
	uint64_t size = texture_cache_size(directory.fileSystemRepresentation);
	/* (the prepared original textures stay under the data root) */
	NSString *prepared = [prepared_root stringByAppendingPathComponent:@"astc-cache"];
	uint64_t prepared_size = texture_cache_size(prepared.fileSystemRepresentation);

	[defaults setObject:size ? [NSByteCountFormatter stringFromByteCount:(long long)size
		countStyle:NSByteCountFormatterCountStyleFile] : @"None" forKey:@"upscaled_textures_size"];
	[defaults setObject:prepared_size ? [NSByteCountFormatter stringFromByteCount:(long long)prepared_size
		countStyle:NSByteCountFormatterCountStyleFile] : @"None" forKey:@"prepared_textures_size"];
	/* how far the on-device upscaler has got, or that this device's profile does not upscale */
	char status[64];
	host_texture_upscale_status(status, sizeof(status));
	[defaults setObject:status[0] ? @(status) : @"Off" forKey:@"upscaled_textures_status"];
	NSLog(@"texture cache: %llu bytes in %@", (unsigned long long)size, directory);
}

/* writes the switch into config.toml (the data folder is the working directory), where the game reads it at the
   next level load; only when the player changed it in Settings since the last write, so a hand edit of
   config.toml is not overwritten at every launch. A switch never changed from its default (on, as the game's)
   writes nothing: the first launch neither overwrites a hand edit nor writes config.toml before the game does */
static void apply_switch(NSUserDefaults *defaults)
{
	if (getenv("HALO_BENCHMARK_SUITE") && *getenv("HALO_BENCHMARK_SUITE"))
		return;   /* a benchmark suite leaves config.toml alone */
	BOOL wanted = [defaults boolForKey:@"upscaled_textures"];
	id applied = [defaults objectForKey:@"upscaled_textures_applied"];

	if (!applied && wanted)
		[defaults setBool:YES forKey:@"upscaled_textures_applied"];
	else if (!applied || [applied boolValue] != wanted)
	{
		if (host_config_write_boolean("display.upscaled_textures", wanted))
			[defaults setBool:wanted forKey:@"upscaled_textures_applied"];
		else
			NSLog(@"settings: could not write display.upscaled_textures to config.toml");
	}
}

void host_texture_settings_apply(const char *data_root)
{
	prepared_root = @(data_root);
	NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
	NSString *directory = cache_directory(data_root);

	[defaults registerDefaults:@{ @"upscaled_textures": @YES, @"delete_upscaled_textures": @NO,
		@"upscaled_textures_size": @"None", @"prepared_textures_size": @"None", @"upscaled_textures_status": @"Off" }];
	apply_switch(defaults);
	if ([defaults boolForKey:@"delete_upscaled_textures"])
	{
		if (host_texture_upscale_delete_cache(directory.fileSystemRepresentation))
			NSLog(@"texture cache: could not delete everything in %@", directory);
		[defaults setBool:NO forKey:@"delete_upscaled_textures"];
	}
	publish_size(directory);
}

void host_texture_settings_observe(const char *data_root)
{
	NSString *root = @(data_root);

	host_texture_settings_apply(data_root);
	[NSNotificationCenter.defaultCenter addObserverForName:UIApplicationWillEnterForegroundNotification object:nil
		queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) {
			(void)note;
			host_texture_settings_apply(root.fileSystemRepresentation);
		}];
	/* the player opens Settings by backgrounding the app, so the size shown must be current as the app leaves;
	   only the size: the delete waits for launch or the foreground */
	[NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil
		queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) {
			(void)note;
			publish_size(cache_directory(root.fileSystemRepresentation));
		}];
}
