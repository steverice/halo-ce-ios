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
#if !TARGET_OS_TV
#import "host_orientation.h"
#endif

/* a window of host_present_window's: it takes the key window from the scene
(on tvOS focus lives only in the key window, and an iPad's keyboard answers
the key window's alert) and gives it back when it goes */
@interface HostPresentWindow : UIWindow
@property(nonatomic, weak) UIWindow *previousKey;
@end
@implementation HostPresentWindow
@end

static UIWindowScene *game_scene(void);

#if TARGET_OS_TV
#define HostRoot UIViewController
#define HostAlert UIAlertController
#else
/* the window's controller, the alert and the sheet keep the game's landscape
and its lock while they are up: the scene asks the key window's controller
and the one presented on top, and UIKit's own allow portrait and unlock it.
The lock follows the game's scene, as host_orientation.m's follows a
controller's, which only has a window once shown */
static BOOL game_landscape(void)
{
	return UIInterfaceOrientationIsLandscape(game_scene().interfaceOrientation);
}
#define HOST_LANDSCAPE \
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; } \
- (UIInterfaceOrientation)preferredInterfaceOrientationForPresentation { return UIInterfaceOrientationLandscapeRight; } \
- (BOOL)prefersInterfaceOrientationLocked { return game_landscape(); }
@interface HostRoot : UIViewController
@end
@implementation HostRoot
HOST_LANDSCAPE
@end
@interface HostAlert : UIAlertController
@end
@implementation HostAlert
HOST_LANDSCAPE
@end
@interface HostSheet : UIActivityViewController
@end
@implementation HostSheet
HOST_LANDSCAPE
@end
#endif

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
	HostPresentWindow *window;

	if (!scene)
		return nil;
	window = [[HostPresentWindow alloc] initWithWindowScene:scene];
	window.windowLevel = UIWindowLevelAlert;
	window.rootViewController = [HostRoot new];
	window.previousKey = scene.keyWindow;
	[window makeKeyAndVisible];
	return window;
}

int host_is_present_window(UIWindow *window)
{
	return [window isKindOfClass:HostPresentWindow.class];
}

/* the window of the share sheet that is up, else nil. While it is set the
game's input waits (host_window_up) and posix_share_link offers no second
sheet; host_dismiss_window clears it on every way a sheet goes: chosen,
canceled, or its scene going to the background */
static UIWindow *sheet_window;
#if !TARGET_OS_TV
/* the sheet window's scene, kept apart: once the scene disconnects the window
no longer gives it (watch_background) */
static UIScene *sheet_scene;
#endif

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
#endif

/* puts away a window of host_present_window's once what it presented is
gone, giving the key window back (unless theater mode hid it meanwhile) with
the game's landscape lock; the sheet's gives the game its input back. Again
for the same window does nothing more */
static void host_dismiss_window(UIWindow *window)
{
	UIWindow *previous = ((HostPresentWindow *)window).previousKey;

	if (!window.hidden)
	{
		window.hidden = YES;
		if (previous && !previous.hidden)
		{
			[previous makeKeyWindow];
#if !TARGET_OS_TV
			host_ios_require_landscape(previous);
#endif
		}
	}
#if !TARGET_OS_TV
	if (window == sheet_window)
	{
		sheet_window = nil;
		sheet_scene = nil;
		game_focus(1);
	}
#endif
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
	host_logf(HOST_LOG_INFO, "alert: \"%s\" up", title ? title : "");
	alert = [HostAlert alertControllerWithTitle:@(title ? title : "") message:@(message ? message : "")
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
	host_dismiss_window(window);
	host_logf(HOST_LOG_INFO, "alert: \"%s\" answered with button %d", title ? title : "", answer);
	return answer;
}

int host_window_up(void)
{
	return sheet_window != nil;
}

#if !TARGET_OS_TV
/* the sheet, put away when its scene goes to the background (the app's, or
on visionOS the game's window closed) or disconnects: the system would keep
it up, and the game's input with it, for the return */
static void watch_background(void)
{
	static BOOL watching;
	void (^put_away)(NSNotification *) = ^(NSNotification *note) {
		UIWindow *window = sheet_window;

		if (!window || note.object != sheet_scene)
			return;
		[window.rootViewController dismissViewControllerAnimated:NO completion:nil];
		host_dismiss_window(window);
		host_logf(HOST_LOG_INFO, "share: the share sheet put away for the %s",
			[note.name isEqualToString:UISceneDidDisconnectNotification] ? "scene's disconnect" : "background");
	};

	if (watching)
		return;
	watching = YES;
	[NSNotificationCenter.defaultCenter addObserverForName:UISceneDidEnterBackgroundNotification object:nil
		queue:NSOperationQueue.mainQueue usingBlock:put_away];
	[NSNotificationCenter.defaultCenter addObserverForName:UISceneDidDisconnectNotification object:nil
		queue:NSOperationQueue.mainQueue usingBlock:put_away];
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

	/* (UIKit only on the main thread, as host_alert) */
	if (!url || !NSThread.isMainThread)
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
	sheet = [[HostSheet alloc] initWithActivityItems:@[ url ] applicationActivities:nil];
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
	sheet_scene = window.windowScene;
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
