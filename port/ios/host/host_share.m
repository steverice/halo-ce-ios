/* Sheets and alerts over the game (spec section 3): every message box, and
the share sheet (posix_share_link). Each is presented from a window of its
own on the game's window scene, at the alert level, so it shows while
theater mode hides the game's window for the immersive space
(host_theater.m). */
#import <UIKit/UIKit.h>
#include <TargetConditionals.h>
#include <SDL3/SDL.h>
#include "ios_host.h"
#include "posix.h"

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

#if !TARGET_OS_TV
/* puts away a window of host_present_window's once what it presented is
gone */
static void host_dismiss_window(UIWindow *window)
{
	window.hidden = YES;
}

/* the game's controllers are ignored while the sheet is up, as when its
window loses focus (sdl_platform.c's input_state.focused) */
static void game_focus(int focused)
{
	SDL_Event event;

	SDL_zero(event);
	event.type = focused ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST;
	event.window.windowID = SDL_GetWindowID(host_sdl_window());
	SDL_PushEvent(&event);
}
#endif

int posix_share_link(const char *link)
{
#if TARGET_OS_TV
	(void)link;
	return 0;
#else
	NSURL *url = link ? [NSURL URLWithString:@(link)] : nil;
	UIWindow *window;
	UIActivityViewController *sheet;

	if (!url || !(window = host_present_window()))
		return 0;
	sheet = [[UIActivityViewController alloc] initWithActivityItems:@[ url ] applicationActivities:nil];
	/* an iPad's is a popover: from the middle of the screen */
	sheet.popoverPresentationController.sourceView = window.rootViewController.view;
	sheet.popoverPresentationController.sourceRect = CGRectMake(CGRectGetMidX(window.bounds), CGRectGetMidY(window.bounds), 0, 0);
	sheet.popoverPresentationController.permittedArrowDirections = 0;
	sheet.completionWithItemsHandler = ^(UIActivityType type, BOOL completed, NSArray *items, NSError *error) {
		(void)type;
		(void)completed;
		(void)items;
		(void)error;
		host_dismiss_window(window);
		game_focus(1);
	};
	game_focus(0);
	[window.rootViewController presentViewController:sheet animated:YES completion:nil];
	host_logf(HOST_LOG_INFO, "share: the invite link in the share sheet");
	return 1;
#endif
}
