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

/* the window of the share sheet that is up, else nil. While it is set the
game's input waits (host_window_up) and posix_share_link offers no second
sheet; host_dismiss_window clears it on every way a sheet goes: chosen,
canceled, or its scene going to the background */
static UIWindow *sheet_window;

int host_window_up(void)
{
	return sheet_window != nil;
}

#if !TARGET_OS_TV
/* the game's keys and mouse buttons let go while the sheet is up, as when its
window loses focus (sdl_platform.c); host_sdl.c holds back the rest */
static void game_focus(int focused)
{
	SDL_Event event;

	SDL_zero(event);
	event.type = focused ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST;
	event.window.windowID = SDL_GetWindowID(host_sdl_window());
	SDL_PushEvent(&event);
}

/* puts away a window of host_present_window's once what it presented is
gone; the sheet's gives the game its input back. Again for the same window
does nothing more */
static void host_dismiss_window(UIWindow *window)
{
	window.hidden = YES;
	if (window == sheet_window)
	{
		sheet_window = nil;
		game_focus(1);
	}
}

/* the sheet, put away when its scene goes to the background (the app's, or
on visionOS the game's window closed): the system would keep it up, and the
game's input with it, for the return */
static void watch_background(void)
{
	static id observer;

	if (observer)
		return;
	observer = [NSNotificationCenter.defaultCenter addObserverForName:UISceneDidEnterBackgroundNotification object:nil
		queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) {
			UIWindow *window = sheet_window;

			if (!window || note.object != window.windowScene)
				return;
			[window.rootViewController dismissViewControllerAnimated:NO completion:nil];
			host_dismiss_window(window);
			host_logf(HOST_LOG_INFO, "share: the share sheet put away for the background");
		}];
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

	if (!url)
		return 0;
	/* (one sheet at a time) */
	if (sheet_window)
	{
		host_logf(HOST_LOG_INFO, "share: a share sheet is already up");
		return 0;
	}
	if (!(window = host_present_window()))
		return 0;
	watch_background();
	sheet = [[UIActivityViewController alloc] initWithActivityItems:@[ url ] applicationActivities:nil];
	/* an iPad's is a popover: from the middle of the screen */
	sheet.popoverPresentationController.sourceView = window.rootViewController.view;
	sheet.popoverPresentationController.sourceRect = CGRectMake(CGRectGetMidX(window.bounds), CGRectGetMidY(window.bounds), 0, 0);
	sheet.popoverPresentationController.permittedArrowDirections = 0;
	/* (chosen or canceled) */
	sheet.completionWithItemsHandler = ^(UIActivityType type, BOOL completed, NSArray *items, NSError *error) {
		(void)type;
		(void)completed;
		(void)items;
		(void)error;
		host_dismiss_window(window);
	};
	sheet_window = window;
	game_focus(0);
	[window.rootViewController presentViewController:sheet animated:YES completion:nil];
	/* (a sheet UIKit did not present never calls its completion: a second
	later, with nothing presented, the game has its input back) */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		if (window == sheet_window && !window.rootViewController.presentedViewController)
			host_dismiss_window(window);
	});
	host_logf(HOST_LOG_INFO, "share: the invite link in the share sheet");
	return 1;
#endif
}
