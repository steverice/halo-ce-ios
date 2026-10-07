/* Sheets and alerts over the game (spec section 3): every message box, and
the share sheet (posix_share_link). Each is presented from a window of its
own on the game's window scene, at the alert level, so it shows while
theater mode hides the game's window for the immersive space
(host_theater.m). */
#import <UIKit/UIKit.h>
#include "ios_host.h"

static UIWindowScene *game_scene(void)
{
	for (UIScene *scene in UIApplication.sharedApplication.connectedScenes)
		/* (the game's window scene: theater mode's immersive space is connected
		too, and shows no windows) */
		if ([scene isKindOfClass:UIWindowScene.class] && scene.activationState != UISceneActivationStateUnattached &&
			[scene.session.role isEqualToString:UIWindowSceneSessionRoleApplication])
			return (UIWindowScene *)scene;
	return nil;
}

UIWindow *host_present_window(void)
{
	UIWindowScene *scene = game_scene();
	UIWindow *window;

	if (!scene)
		return nil;
	window = [[UIWindow alloc] initWithWindowScene:scene];
	window.windowLevel = UIWindowLevelAlert;
	window.rootViewController = [UIViewController new];
	window.hidden = NO;
	return window;
}

int host_alert(const char *title, const char *message, const char *first, const char *second)
{
	__block int answer = -1;
	UIWindow *window;
	UIAlertController *alert;
	UIAlertAction *preferred;

	/* (UIKit only on the main thread, where the game's boxes come; SDL's box,
	which the caller falls back on, takes any other there itself) */
	if (!NSThread.isMainThread)
		return -1;
	window = host_present_window();
	if (!window)
		return -1;
	alert = [UIAlertController alertControllerWithTitle:@(title ? title : "") message:@(message ? message : "")
		preferredStyle:UIAlertControllerStyleAlert];
	preferred = [UIAlertAction actionWithTitle:@(first ? first : "OK") style:UIAlertActionStyleDefault
		handler:^(UIAlertAction *action) { (void)action; answer = 0; }];
	[alert addAction:preferred];
	if (second)
		[alert addAction:[UIAlertAction actionWithTitle:@(second) style:UIAlertActionStyleCancel
			handler:^(UIAlertAction *action) { (void)action; answer = 1; }]];
	/* (the first button answers Return, as SDL's box made it) */
	alert.preferredAction = preferred;
	[window.rootViewController presentViewController:alert animated:YES completion:nil];
	/* (the caller waits, as SDL's message box does) */
	while (answer < 0)
	{
		@autoreleasepool
		{
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
		}
	}
	window.hidden = YES;
	return answer;
}
