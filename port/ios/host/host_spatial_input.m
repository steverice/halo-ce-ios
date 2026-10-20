/* The Sense controllers' buttons and haptics (host_spatial_input.h). */
#include "host_spatial_input.h"
#if TARGET_OS_VISION
#include "host.h"
#include <math.h>
#include <os/lock.h>
#include <string.h>
#include <time.h>

/* the tracking-loss buzz (the spec's section 1: one short buzz on the aiming
controller) */
#define BUZZ_INTENSITY 0.8f
#define BUZZ_SHARPNESS 0.5f
#define BUZZ_SECONDS 0.08

/* the first of names the state has as a button (nil: none) */
static id<GCButtonElement> button_named(id<GCDevicePhysicalInputState> state,
	NSArray<NSString<GCButtonElementName> *> *names)
{
	for (NSString<GCButtonElementName> *name in names)
	{
		id<GCButtonElement> button = state.buttons[name];

		if (button)
			return button;
	}
	return nil;
}

static float button_value(id<GCDevicePhysicalInputState> state,
	NSArray<NSString<GCButtonElementName> *> *names)
{
	id<GCButtonElement> button = button_named(state, names);

	return button ? button.pressedInput.value : 0.0f;
}

static int button_pressed(id<GCDevicePhysicalInputState> state,
	NSArray<NSString<GCButtonElementName> *> *names)
{
	id<GCButtonElement> button = button_named(state, names);

	return button ? button.pressedInput.pressed : 0;
}

void host_spatial_controls_read(id<GCDevicePhysicalInputState> state, struct host_spatial_controls *controls)
{
	id<GCDirectionPadElement> stick = nil;

	memset(controls, 0, sizeof(*controls));
	if (!state)
		return;
	if (@available(visionOS 26.0, *))
	{
		/* a spatial controller's own names (GCInputNames.h, visionOS 26),
		then a gamepad's for the same control. The face buttons: the lower
		(Cross, Square) is A or X, the upper (Circle, Triangle) B or Y */
		controls->trigger = button_value(state, @[GCInputTrigger, GCInputRightTrigger, GCInputLeftTrigger]);
		controls->grip = button_value(state, @[GCInputGripButton, GCInputRightShoulder, GCInputLeftShoulder]);
		controls->stick_click = button_pressed(state, @[GCInputThumbstickButton, GCInputRightThumbstickButton,
			GCInputLeftThumbstickButton]);
		stick = state.dpads[GCInputThumbstick];
		if (!stick)
			stick = state.dpads[GCInputRightThumbstick];
		if (!stick)
			stick = state.dpads[GCInputLeftThumbstick];
	}
	controls->face_low = button_pressed(state, @[GCInputButtonA, GCInputButtonX]);
	controls->face_high = button_pressed(state, @[GCInputButtonB, GCInputButtonY]);
	controls->menu = button_pressed(state, @[GCInputButtonMenu, GCInputButtonOptions, GCInputButtonShare]);
	if (stick)
	{
		/* (GameController's y is up positive, as the gamepad's) */
		controls->stick_x = stick.xAxis.value;
		controls->stick_y = stick.yAxis.value;
	}
}

void host_spatial_controls_log_names(GCController *controller, const char *hand)
{
	NSMutableArray<NSString *> *names = [NSMutableArray array];

	for (id<GCPhysicalInputElement> element in controller.input.elements)
	{
		/* each element's aliases, as "Button A/Cross" */
		NSArray<NSString *> *aliases = [element.aliases.allObjects sortedArrayUsingSelector:@selector(compare:)];

		[names addObject:aliases.count ? [aliases componentsJoinedByString:@"/"] : @"(unnamed)"];
	}
	[names sortUsingSelector:@selector(compare:)];
	host_logf(HOST_LOG_INFO, "spatial: the %s controller's inputs: %s", hand,
		names.count ? [names componentsJoinedByString:@", "].UTF8String : "none");
}

CHHapticEngine *host_spatial_haptics_start(GCController *controller, const char *hand)
{
	CHHapticEngine *engine = [controller.haptics createEngineWithLocality:GCHapticsLocalityDefault];
	NSError *error = nil;

	if (!engine)
	{
		host_logf(HOST_LOG_WARN, "spatial: the %s controller has no haptics; no buzz on it", hand);
		return nil;
	}
	/* the engine stops when the app is in the background, and resets after a
	server failure: it starts again at the next buzz */
	engine.playsHapticsOnly = YES;
	engine.autoShutdownEnabled = NO;
	if (![engine startAndReturnError:&error])
		host_logf(HOST_LOG_WARN, "spatial: the %s controller's haptics didn't start: %s; trying again at the first "
			"buzz", hand, error.localizedDescription.UTF8String);
	return engine;
}

/* the buzz's player on engine (nil: failed, with the error) */
static id<CHHapticPatternPlayer> buzz_player(CHHapticEngine *engine, NSError **error)
{
	CHHapticEventParameter *intensity = [[CHHapticEventParameter alloc]
		initWithParameterID:CHHapticEventParameterIDHapticIntensity value:BUZZ_INTENSITY];
	CHHapticEventParameter *sharpness = [[CHHapticEventParameter alloc]
		initWithParameterID:CHHapticEventParameterIDHapticSharpness value:BUZZ_SHARPNESS];
	/* a continuous event for its 80 ms: a transient event's length is the
	system's own, and ignores any duration */
	CHHapticEvent *event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
		parameters:@[intensity, sharpness] relativeTime:0.0 duration:BUZZ_SECONDS];
	CHHapticPattern *pattern = [[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:error];

	return pattern ? [engine createPlayerWithPattern:pattern error:error] : nil;
}

void host_spatial_haptics_buzz(CHHapticEngine *engine, const char *hand)
{
	NSError *error = nil;
	id<CHHapticPatternPlayer> player;
	int attempt;

	if (!engine)
		return;
	/* a stopped engine (the background, a reset) fails the first try: start
	it and try once more */
	for (attempt = 0; attempt < 2; attempt++)
	{
		error = nil;
		player = buzz_player(engine, &error);
		if (player && [player startAtTime:CHHapticTimeImmediate error:&error])
			return;
		if (attempt == 0 && ![engine startAndReturnError:&error])
			break;
	}
	host_logf(HOST_LOG_WARN, "spatial: the %s controller didn't buzz: %s", hand,
		error ? error.localizedDescription.UTF8String : "no error given");
}

/* ---------- the game's rumble */

/* an intensity change smaller than this isn't sent (the motors ease every
tick; each send is a message to the haptics server) */
#define RUMBLE_INTENSITY_STEP 0.01f
/* a hand whose rumble failed isn't tried again for this long: every try is
several calls to the haptics server, on the game thread */
#define RUMBLE_RETRY_SECONDS 1.0

static os_unfair_lock rumble_lock = OS_UNFAIR_LOCK_INIT;
/* all guarded by rumble_lock, by hand */
static CHHapticEngine *rumble_engines[2];
/* a hand's continuous event (nil until its first rumble, and again after
its engine stopped or reset) and the sharpness it was made with */
static id<CHHapticPatternPlayer> rumble_players[2];
static float rumble_sharpness[2];
static int rumble_playing[2];
static float rumble_intensity[2];      /* the last intensity sent while playing */
static int rumble_failed[2];           /* the failure is logged once until a rumble plays again */
static double rumble_retry[2];         /* after a failure, no other try before this time (seconds) */
static int rumble_started[2];          /* the first rumble on this engine is logged */
static int rumble_is_quiet;
static double rumble_sent;             /* when the last levels came (seconds) */
static int rumble_watched;             /* the stale check is scheduled */

static const char *rumble_hand_name(int hand)
{
	return hand == 0 ? "left" : "right";
}

static double rumble_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/* with rumble_lock held */
static void rumble_stop_locked(int hand)
{
	if (rumble_playing[hand])
		[rumble_players[hand] stopAtTime:CHHapticTimeImmediate error:NULL];
	rumble_playing[hand] = 0;
	rumble_intensity[hand] = 0.0f;
}

/* an engine stopped (the app in the background, the controller gone) or
reset (the haptics server restarted): its player is gone with it, and the
next rumble makes another */
static void rumble_engine_ended(CHHapticEngine *engine)
{
	int hand;

	os_unfair_lock_lock(&rumble_lock);
	for (hand = 0; hand < 2; hand++)
		if (rumble_engines[hand] == engine)
		{
			rumble_players[hand] = nil;
			rumble_playing[hand] = 0;
			rumble_intensity[hand] = 0.0f;
		}
	os_unfair_lock_unlock(&rumble_lock);
}

void host_spatial_rumble_attach(int hand, CHHapticEngine *engine)
{
	if (hand < 0 || hand > 1)
		return;
	os_unfair_lock_lock(&rumble_lock);
	if (rumble_engines[hand] != engine)
	{
		rumble_stop_locked(hand);
		rumble_players[hand] = nil;
		rumble_engines[hand] = engine;
		rumble_failed[hand] = rumble_started[hand] = 0;
		rumble_retry[hand] = 0.0;
	}
	os_unfair_lock_unlock(&rumble_lock);
	if (engine)
	{
		__weak CHHapticEngine *weak = engine;

		/* (both are called on a queue of the engine's) */
		engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) {
			(void)reason;
			rumble_engine_ended(weak);
		};
		engine.resetHandler = ^{
			rumble_engine_ended(weak);
		};
	}
}

/* the hand's continuous event at full intensity, lasting until it is
stopped: its dynamic intensity control is the motor. With rumble_lock held */
static id<CHHapticPatternPlayer> rumble_player(CHHapticEngine *engine, float sharpness, NSError **error)
{
	CHHapticEventParameter *intensity = [[CHHapticEventParameter alloc]
		initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f];
	CHHapticEventParameter *sharp = [[CHHapticEventParameter alloc]
		initWithParameterID:CHHapticEventParameterIDHapticSharpness value:sharpness];
	CHHapticEvent *event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
		parameters:@[intensity, sharp] relativeTime:0.0 duration:GCHapticDurationInfinite];
	CHHapticPattern *pattern = [[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:error];

	return pattern ? [engine createPlayerWithPattern:pattern error:error] : nil;
}

/* one hand at an intensity above 0. With rumble_lock held */
static void rumble_play_locked(int hand, float intensity, float sharpness)
{
	CHHapticEngine *engine = rumble_engines[hand];
	NSError *error = nil;
	int attempt;

	if (rumble_players[hand] && rumble_sharpness[hand] != sharpness)
	{
		rumble_stop_locked(hand);
		rumble_players[hand] = nil;
	}
	if (rumble_playing[hand] && fabsf(intensity - rumble_intensity[hand]) < RUMBLE_INTENSITY_STEP)
		return;
	if (rumble_failed[hand] && rumble_now() < rumble_retry[hand])
		return;
	/* a stopped engine (the background, a reset) fails the first try: start
	it, make the player again and try once more */
	for (attempt = 0; attempt < 2; attempt++)
	{
		CHHapticDynamicParameter *control;

		error = nil;
		if (!rumble_players[hand])
		{
			rumble_players[hand] = rumble_player(engine, sharpness, &error);
			rumble_sharpness[hand] = sharpness;
			rumble_playing[hand] = 0;
		}
		control = [[CHHapticDynamicParameter alloc]
			initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:intensity relativeTime:0.0];
		if (rumble_players[hand] &&
			[rumble_players[hand] sendParameters:@[control] atTime:CHHapticTimeImmediate error:&error] &&
			(rumble_playing[hand] || [rumble_players[hand] startAtTime:CHHapticTimeImmediate error:&error]))
		{
			rumble_playing[hand] = 1;
			rumble_intensity[hand] = intensity;
			rumble_failed[hand] = 0;
			if (!rumble_started[hand])
			{
				rumble_started[hand] = 1;
				host_logf(HOST_LOG_INFO, "spatial: the game's rumble plays on the %s controller (intensity %.2f, "
					"sharpness %.2f)", rumble_hand_name(hand), intensity, sharpness);
			}
			return;
		}
		rumble_players[hand] = nil;
		rumble_playing[hand] = 0;
		if (attempt == 0 && ![engine startAndReturnError:&error])
			break;
	}
	rumble_intensity[hand] = 0.0f;
	rumble_retry[hand] = rumble_now() + RUMBLE_RETRY_SECONDS;
	if (!rumble_failed[hand])
	{
		rumble_failed[hand] = 1;
		host_logf(HOST_LOG_WARN, "spatial: the %s controller didn't rumble: %s", rumble_hand_name(hand),
			error ? error.localizedDescription.UTF8String : "no error given");
	}
}

/* while a hand plays: stops both once the game's levels are stale (a game
that stalled mid-rumble sends no zeros), and looks again until none plays */
static void rumble_watch(void)
{
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(HOST_SPATIAL_RUMBLE_STALE_SECONDS / 2.0 * NSEC_PER_SEC)),
		dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
			int again;

			os_unfair_lock_lock(&rumble_lock);
			if ((rumble_playing[0] || rumble_playing[1]) && !host_spatial_rumble_fresh(rumble_sent, rumble_now()))
			{
				rumble_stop_locked(0);
				rumble_stop_locked(1);
				host_logf(HOST_LOG_INFO, "spatial: the game stopped sending its rumble; the controllers are quiet");
			}
			again = rumble_playing[0] || rumble_playing[1];
			rumble_watched = again;
			os_unfair_lock_unlock(&rumble_lock);
			if (again)
				rumble_watch();
		});
}

void host_spatial_rumble_play(const struct host_spatial_rumble *rumble)
{
	int hand, watch;

	os_unfair_lock_lock(&rumble_lock);
	rumble_sent = rumble_now();
	for (hand = 0; hand < 2; hand++)
	{
		if (rumble_is_quiet || !rumble_engines[hand] || !(rumble->intensity[hand] > 0.0f))
			rumble_stop_locked(hand);
		else
			rumble_play_locked(hand, rumble->intensity[hand], rumble->sharpness[hand]);
	}
	watch = (rumble_playing[0] || rumble_playing[1]) && !rumble_watched;
	if (watch)
		rumble_watched = 1;
	os_unfair_lock_unlock(&rumble_lock);
	if (watch)
		rumble_watch();
}

void host_spatial_rumble_quiet(int quiet)
{
	os_unfair_lock_lock(&rumble_lock);
	rumble_is_quiet = quiet != 0;
	if (quiet)
	{
		rumble_stop_locked(0);
		rumble_stop_locked(1);
	}
	os_unfair_lock_unlock(&rumble_lock);
}
#endif
