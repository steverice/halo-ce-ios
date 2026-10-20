/* Tracked controllers for controller aim (input.aim = "controller"): the
guest's imports host_spatial_sample and host_spatial_feedback (guest_host.h,
halo_spatial.h).

Each frame the guest takes two samples: the game sample from XInputGetState's
port 0 (the poses with the buttons of the same instant), and the drawing
sample at the frame's begin, after host_stereo_frame latched the eyes. Each
carries both hands in the room's level frame (host_spatial_pose.h) and the
head's yaw at its latch.

On every host platform debug.test_controller makes a synthetic right hand
(host_spatial_pose.c), held still in the room or sweeping, for checks on the
Mac runner and in the simulator, in place of any real controllers.

On visionOS 27 in HEAD mode, the PlayStation VR2 Sense controllers are tracked
through ARKit's accessory tracking provider. GameController announces them
(GCProductCategorySpatialController); the first asks for the accessory
tracking permission, each is loaded as an ARKit accessory, and once one has
loaded, the theater's session (host_theater.m) runs again with world
tracking and an accessory tracking provider over the loaded accessories.
Every callback (GameController's notifications, the permission, a load, the
provider's updates and the session's provider states) only appends to a
locked queue of changes; the game thread drains it before each sample, and
is the only thread that reads the provider's anchors (AR_MT_UNSAFE). The
game sample takes each hand's latest anchor; the drawing sample
(host_spatial_frame, inside host_stereo_frame) predicts each tracked hand to
the drawable's trackable-anchor time. On visionOS 26 the game logs once that
controller aim needs 27, and aims with the head.

The game sample also carries both controllers' buttons, read at the aiming
hand's anchor timestamp (host_spatial_input.m) and mapped onto the Xbox
gamepad (host_spatial_gamepad.c), which xinput_sdl.c merges into port 0.
host_spatial_feedback hides the user's real hands while a hand aims
(Theater.swift), plays the tracking-loss buzz on the aiming controller, and
plays the game's rumble on both (host_spatial_gamepad.h's mapping) while
controller aim owns them. */
#include "host_spatial_pose.h"
#include "host_spatial_record.h"
#include "host_stereo.h"
#include "host_config.h"
#include "host.h"
#include <TargetConditionals.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#if TARGET_OS_VISION
#import <Foundation/Foundation.h>
#import <ARKit/ARKit.h>
#import <CompositorServices/CompositorServices.h>
#import <GameController/GameController.h>
#import <QuartzCore/QuartzCore.h>
#include <os/lock.h>
#include <simd/simd.h>
#include "host_spatial_input.h"
#include "host_theater.h"
#endif

/* the aim location's axes (HOST_SPATIAL_AXIS_*) that are the barrel and its
up. ARKit doesn't document them: these are measured on the PlayStation VR2
Sense controllers (headset session 1, 2026-10-07: the axes step of
headset-session-aim.sh with debug.controller_axes, held still pointing ahead;
the aim's -z axis was 14 degrees from the head's forward and its +y 14 from
the head's up, steady within half a degree). debug.controller_aim_axes
overrides them for another controller */
#define HOST_SPATIAL_AIM_FORWARD HOST_SPATIAL_AXIS_MINUS_Z
#define HOST_SPATIAL_AIM_UP HOST_SPATIAL_AXIS_PLUS_Y

/* the settings, read once (read_settings) */
static int settings_read;
static int aim_controller;     /* input.aim = "controller" */
static int aiming_hand = 1;    /* input.aim_hand: 0 left, 1 right */
static int head_mode;          /* display.stereo = "head" */
static int synthetic_on;       /* debug.test_controller parsed */
static int replay_on;          /* debug.controller_replay loaded: its samples answer every call */
static struct host_spatial_synthetic synthetic;
static char synthetic_value[64];
/* the aim location's barrel and up axes (debug.controller_aim_axes, else
HOST_SPATIAL_AIM_FORWARD and _UP) */
static int aim_forward = HOST_SPATIAL_AIM_FORWARD, aim_up = HOST_SPATIAL_AIM_UP;
static int correction_rendered; /* debug.controller_correction = "rendered" */
static int axes_log;            /* debug.controller_axes */
/* the synthetic hand's clock: seconds from the first sample */
static double synthetic_start = -1.0;
/* the last reason logged, -1 none yet */
static int reason_logged = -1;
/* the aiming controller against the head (host_spatial_pose.h's misplaced
test), judged on each game sample; an ARKit restart it asked for and the
tracker's frame hasn't made yet; and how many this launch has had */
static struct host_spatial_misplaced misplaced;
static atomic_int misplaced_restart_wanted;
static int misplaced_restarts;
#define MISPLACED_RESTARTS 2
/* host_spatial_feedback's last values */
static int feedback_aiming, feedback_buzz_serial;
/* 1 while controller aim owns the Sense controllers: the tracker runs with
both loaded, so their buttons merge into port 0 (the game sample's
gamepad_valid); host_sdl.c leaves them out of SDL's gamepads only then.
Written on the game thread, read by host_sdl.c */
static int owns_sense_controllers;

/* when the game sample last had both Sense controllers' menu buttons down
(the gamepad's Back and Start), in milliseconds of CACurrentMediaTime; 0
while they aren't. Written on the game thread, read by host_sdl.c */
static _Atomic uint64_t sense_menus_down_ms;

/* for host_sdl.c (declared in ios_host.h) */
int host_spatial_sense_menus_held(void);
int host_spatial_sense_menus_held(void)
{
#if TARGET_OS_VISION
	uint64_t down = atomic_load(&sense_menus_down_ms);

	/* (a sample older than a quarter second says nothing: the samples stop
	as the space closes) */
	return down != 0 && (uint64_t)(CACurrentMediaTime() * 1000.0) - down < 250;
#else
	return atomic_load(&sense_menus_down_ms) != 0;
#endif
}

/* for host_sdl.c (declared in ios_host.h) */
int host_spatial_owns_sense_controllers(void);
int host_spatial_owns_sense_controllers(void)
{
	return __atomic_load_n(&owns_sense_controllers, __ATOMIC_ACQUIRE);
}

/* the guest's imports (guest_host.h): the structs cross as they are */
void host_spatial_sample(int kind, struct halo_spatial_sample *sample);
void host_spatial_feedback(const struct halo_spatial_feedback *feedback);

static double seconds_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

#if TARGET_OS_VISION
/* the tracker (a HostSpatialTracker, visionOS 27), made at the first sample
in HEAD mode with input.aim = "controller" and no synthetic hand */
static id tracker;
/* visionOS 26 in that case: controller aim can't run (logged once) */
static int needs_27;
static void tracker_start(void);
/* the game's rumble: input.controller_rumble, and for the log the largest
intensities mapped onto the hands since the last line, and when that was */
static float rumble_strength = 1.0f;
static float rumble_peak[2];
static double rumble_logged;
/* at most one "spatial: rumble" line in this many seconds */
#define RUMBLE_LOG_SECONDS 1.0
#endif

static void read_settings(void)
{
	char value[64];

	settings_read = 1;
	host_config_string("input.aim", "controller", value, sizeof(value));
	aim_controller = !strcmp(value, "controller");
	if (!aim_controller && strcmp(value, "head"))
		host_logf(HOST_LOG_WARN, "controller aim: input.aim \"%s\" is not recognized; aiming with the head", value);
	host_config_string("input.aim_hand", "right", value, sizeof(value));
	aiming_hand = strcmp(value, "left") ? 1 : 0;
	host_config_string("display.stereo", "off", value, sizeof(value));
	head_mode = !strcmp(value, "head");
	host_config_string("debug.test_controller", "", synthetic_value, sizeof(synthetic_value));
	if (synthetic_value[0])
	{
		synthetic_on = host_spatial_synthetic_parse(synthetic_value, &synthetic);
		if (!synthetic_on)
			host_logf(HOST_LOG_WARN, "spatial: debug.test_controller \"%s\" isn't \"fixed:<yaw>,<pitch>\", \"sweep\" "
				"or \"loss:<seconds>\" (up to 5); no synthetic controller", synthetic_value);
	}
	host_config_string("debug.controller_record", "", value, sizeof(value));
	host_spatial_record_start(value);
	host_config_string("debug.controller_replay", "", value, sizeof(value));
	replay_on = host_spatial_replay_start(value);
	host_config_string("debug.controller_aim_axes", "", value, sizeof(value));
	if (value[0])
	{
		if (host_spatial_axes_parse(value, &aim_forward, &aim_up))
			host_logf(HOST_LOG_INFO, "spatial: debug.controller_aim_axes: the aim's forward is its %s axis and its up "
				"its %s", host_spatial_axis_name(aim_forward), host_spatial_axis_name(aim_up));
		else
			host_logf(HOST_LOG_WARN, "spatial: debug.controller_aim_axes \"%s\" isn't \"<forward>,<up>\" (each one of "
				"+x, -x, +y, -y, +z and -z, on different lines); the aim's forward is its %s axis and its up its %s",
				value, host_spatial_axis_name(aim_forward), host_spatial_axis_name(aim_up));
	}
	host_config_string("debug.controller_correction", "none", value, sizeof(value));
	correction_rendered = !strcmp(value, "rendered");
	if (!correction_rendered && strcmp(value, "none"))
		host_logf(HOST_LOG_WARN, "spatial: debug.controller_correction \"%s\" isn't \"none\" or \"rendered\"; none",
			value);
	host_config_string("debug.controller_axes", "false", value, sizeof(value));
	axes_log = !strcmp(value, "true");
#if TARGET_OS_VISION
	rumble_strength = (float)host_config_real("input.controller_rumble", 1.0);
	if (!(rumble_strength >= 0.0f && rumble_strength <= HOST_SPATIAL_RUMBLE_STRENGTH_MAXIMUM))
	{
		float clamped = rumble_strength > HOST_SPATIAL_RUMBLE_STRENGTH_MAXIMUM ? HOST_SPATIAL_RUMBLE_STRENGTH_MAXIMUM :
			0.0f;

		host_logf(HOST_LOG_WARN, "spatial: input.controller_rumble %g is outside 0 to %g; using %g",
			(double)rumble_strength, (double)HOST_SPATIAL_RUMBLE_STRENGTH_MAXIMUM, (double)clamped);
		rumble_strength = clamped;
	}
	if (aim_controller && head_mode && !synthetic_on && !replay_on)
		tracker_start();
#endif
}

#if TARGET_OS_VISION
/* ---------- visionOS 27: the Sense controllers through ARKit */

/* the drawing sample's axes log (debug.controller_axes): twice a second */
#define AXES_LOG_SECONDS 0.5
/* each controller's input states GameController keeps, for the reading at
the aim's anchor timestamp */
#define INPUT_STATE_QUEUE_DEPTH 8
/* the changes between reading the buttons at the anchor's timestamp and
live that are logged (host_spatial_gamepad.h's clock decides) */
#define READ_MODE_LOGS 20

enum spatial_event_kind
{
	EVENT_CONNECT,          /* controller */
	EVENT_DISCONNECT,       /* controller */
	EVENT_AUTHORIZATION,    /* status (ar_authorization_status_t, -1 with an error), text */
	EVENT_LOADED,           /* controller, accessory (nil: failed), text */
	EVENT_UPDATED,          /* status (1 success), text */
	EVENT_PROVIDER_STATE    /* status (ar_data_provider_state_t), text (an error) */
};

enum { AUTHORIZATION_NONE, AUTHORIZATION_REQUESTED, AUTHORIZATION_ALLOWED, AUTHORIZATION_DENIED };

/* one change from a callback, queued for the game thread */
API_AVAILABLE(visionos(27.0))
@interface HostSpatialEvent : NSObject
@property (nonatomic) int kind;
@property (nonatomic) long status;
@property (nonatomic, strong) GCController *controller;
@property (nonatomic, strong) ar_accessory_t accessory;
@property (nonatomic, copy) NSString *text;
@end

@implementation HostSpatialEvent
@end

/* a connected Sense controller */
API_AVAILABLE(visionos(27.0))
@interface HostSpatialController : NSObject
@property (nonatomic, strong) GCController *controller;
@property (nonatomic, strong) ar_accessory_t accessory;   /* nil until loaded */
@property (nonatomic) int loading;
/* loads started since the last permission result: a failed load is tried once more */
@property (nonatomic) int attempts;
@property (nonatomic) int hand;                           /* 0 left, 1 right; -1 until loaded */
/* the drawing sample's prediction, made once */
@property (nonatomic, strong) ar_accessory_anchor_t predicted;
/* the buzz's engine, started as the controller loads (nil: no haptics) */
@property (nonatomic, strong) CHHapticEngine *haptics;
/* the aim's and the grip's last room positions while the position was
tracked: an orientation-only state keeps them */
@property (nonatomic) simd_float3 aim_position, grip_position;
@property (nonatomic) int position_known;
@end

@implementation HostSpatialController
@end

static NSString *error_text(ar_error_t error) API_AVAILABLE(visionos(27.0))
{
	if (!error)
		return @"no error given";
	NSError *cf = (__bridge_transfer NSError *)ar_error_copy_cf_error(error);
	return [NSString stringWithFormat:@"%@ (code %ld)", cf.localizedDescription, (long)ar_error_get_error_code(error)];
}

static const char *controller_name(GCController *controller)
{
	NSString *name = controller.vendorName;

	return name.length ? name.UTF8String : "a Sense controller";
}

static const char *hand_name(int hand)
{
	return hand == 0 ? "left" : "right";
}

static int state_of(ar_accessory_anchor_tracking_state_t state) API_AVAILABLE(visionos(27.0))
{
	switch (state)
	{
	case ar_accessory_anchor_tracking_state_orientation_tracked: return HALO_SPATIAL_ORIENTATION;
	case ar_accessory_anchor_tracking_state_position_orientation_tracked: return HALO_SPATIAL_TRACKED;
	case ar_accessory_anchor_tracking_state_position_orientation_tracked_low_accuracy: return HALO_SPATIAL_LOW_ACCURACY;
	default: return HALO_SPATIAL_UNTRACKED;
	}
}

/* a location's transform in the room: the anchor's origin transform times
the location's, with debug.controller_correction's correction on both */
static simd_float4x4 origin_from_location(ar_accessory_anchor_t anchor, const char *name) API_AVAILABLE(visionos(27.0))
{
	ar_transform_correction_t correction = correction_rendered ? ar_transform_correction_rendered :
		ar_transform_correction_none;

	return simd_mul(ar_accessory_anchor_get_origin_from_anchor_transform_with_correction(anchor, correction),
		ar_accessory_anchor_get_anchor_from_location_transform_with_correction(anchor, name, correction));
}

/* a room transform into the room's level frame from the head */
static void location_of(simd_float4x4 transform, const float *head_position, struct halo_spatial_location *location)
{
	float room_from_location[16];

	memcpy(room_from_location, &transform, sizeof(room_from_location));
	host_spatial_location(room_from_location, head_position, aim_forward, aim_up, location);
	/* The head and both hand locations must share the same origin correction,
	including position relative to the head and the held body yaw at a pole. */
	host_stereo_room_vector(location->forward);
	host_stereo_room_vector(location->left);
	host_stereo_room_vector(location->up);
	host_stereo_room_vector(location->position);
}

API_AVAILABLE(visionos(27.0))
@interface HostSpatialTracker : NSObject
@end

@implementation HostSpatialTracker
{
	os_unfair_lock lock;
	NSMutableArray<HostSpatialEvent *> *events;          /* guarded by lock */
	NSMutableArray<HostSpatialController *> *controllers;
	id connect_observer, disconnect_observer;
	int supported;
	int authorization;
	int wants_authorization;
	/* the session the permission was last looked at in (a new one, the
	space opened again, asks again after an undecided answer), and the
	undecided answer's log, once */
	ar_session_t seen_session;
	int undecided_logged;
	/* the provider, the session it runs in, and a change to its accessories
	waiting (dirty) or under way (updating) */
	ar_accessory_tracking_provider_t provider;
	ar_session_t ran_session;
	dispatch_queue_t provider_queue;
	int dirty, updating;
	/* the device's last placed position, and a re-run's log waiting for
	the next one */
	simd_float3 last_device_position;
	int last_device_known;
	simd_float3 rerun_device_position;
	int rerun_pending, rerun_device_known;
	/* the drawing sample, from host_spatial_frame this frame */
	struct halo_spatial_sample draw_sample;
	int draw_fresh;
	double axes_logged_at;
	int hand_logged;
	/* the gamepad last logged ("spatial: buttons"), and whether each way
	of reading the buttons (at the anchor's timestamp, live) was logged yet */
	struct halo_spatial_gamepad logged_gamepad;
	/* when the buttons are read, and the last way logged (-1 none yet) with
	the count of those logs */
	struct host_spatial_button_clock button_clock;
	int read_mode_logged, read_mode_logs;
}

- (instancetype)init
{
	if (!(self = [super init]))
		return nil;
	lock = OS_UNFAIR_LOCK_INIT;
	read_mode_logged = -1;
	events = [NSMutableArray array];
	controllers = [NSMutableArray array];
	supported = ar_accessory_tracking_provider_is_supported();
	if (!supported)
	{
		host_logf(HOST_LOG_INFO, "spatial: accessory tracking isn't supported here; no controllers");
		return self;
	}
	provider_queue = dispatch_queue_create("org.steverice.visr.spatial", DISPATCH_QUEUE_SERIAL);
	__weak HostSpatialTracker *weak = self;
	NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
	connect_observer = [center addObserverForName:GCControllerDidConnectNotification object:nil queue:nil
		usingBlock:^(NSNotification *note) { [weak push:EVENT_CONNECT controller:note.object]; }];
	disconnect_observer = [center addObserverForName:GCControllerDidDisconnectNotification object:nil queue:nil
		usingBlock:^(NSNotification *note) { [weak push:EVENT_DISCONNECT controller:note.object]; }];
	/* those connected already */
	NSArray<GCController *> *connected = GCController.controllers;
	for (GCController *controller in connected)
		[self push:EVENT_CONNECT controller:controller];
	host_logf(HOST_LOG_INFO, "spatial: watching for Sense controllers (%lu controller%s connected now)",
		(unsigned long)connected.count, connected.count == 1 ? "" : "s");
	return self;
}

/* ---------- the queue of changes: any thread */

- (void)push:(HostSpatialEvent *)event
{
	os_unfair_lock_lock(&lock);
	[events addObject:event];
	os_unfair_lock_unlock(&lock);
}

- (void)push:(int)kind controller:(id)controller
{
	HostSpatialEvent *event;

	if (![controller isKindOfClass:GCController.class])
		return;
	event = [HostSpatialEvent new];
	event.kind = kind;
	event.controller = controller;
	[self push:event];
}

- (void)push:(int)kind status:(long)status text:(NSString *)text
{
	HostSpatialEvent *event = [HostSpatialEvent new];

	event.kind = kind;
	event.status = status;
	event.text = text;
	[self push:event];
}

/* ---------- the game thread */

- (HostSpatialController *)entryFor:(GCController *)controller
{
	for (HostSpatialController *entry in controllers)
		if (entry.controller == controller)
			return entry;
	return nil;
}

- (void)connect:(GCController *)controller
{
	HostSpatialController *entry;

	if (![controller.productCategory isEqualToString:GCProductCategorySpatialController] ||
		[self entryFor:controller])
		return;
	entry = [HostSpatialController new];
	entry.controller = controller;
	entry.hand = -1;
	[controllers addObject:entry];
	/* the buttons are read at the aim's anchor timestamp from these */
	controller.input.inputStateQueueDepth = INPUT_STATE_QUEUE_DEPTH;
	host_logf(HOST_LOG_INFO, "spatial: %s connected; loading it", controller_name(controller));
	wants_authorization = 1;
	[self load:entry];
}

/* loads a controller as an ARKit accessory (asynchronous: its handler only
queues the result) */
- (void)load:(HostSpatialController *)entry
{
	GCController *controller = entry.controller;
	__weak HostSpatialTracker *weak = self;

	entry.loading = 1;
	entry.attempts++;
	ar_accessory_load_from_device(controller, ^(id<GCDevice> device, bool successful, ar_error_t error,
		ar_accessory_t accessory) {
		HostSpatialEvent *event = [HostSpatialEvent new];

		event.kind = EVENT_LOADED;
		event.controller = controller;
		event.accessory = successful ? accessory : nil;
		event.text = successful ? nil : error_text(error);
		[weak push:event];
	});
}

- (void)loaded:(HostSpatialEvent *)event
{
	HostSpatialController *entry = [self entryFor:event.controller];
	NSMutableString *locations;
	int hand;

	/* disconnected while it loaded */
	if (!entry)
		return;
	entry.loading = 0;
	if (!event.accessory)
	{
		/* once more after a failure (the permission granted meanwhile, say);
		the permission's result tries again too */
		host_logf(HOST_LOG_WARN, "spatial: %s didn't load: %s%s", controller_name(entry.controller),
			event.text.UTF8String, entry.attempts < 2 ? "; trying again" : "");
		if (entry.attempts < 2)
			[self load:entry];
		return;
	}
	switch (ar_accessory_get_inherent_chirality(event.accessory))
	{
	case ar_accessory_chirality_left: hand = 0; break;
	case ar_accessory_chirality_right: hand = 1; break;
	default:
		/* no hand of its own: the free one, the right first */
		hand = 1;
		for (HostSpatialController *other in controllers)
			if (other != entry && other.hand == 1)
				hand = 0;
		break;
	}
	for (HostSpatialController *other in controllers)
		if (other != entry && other.hand == hand)
		{
			host_logf(HOST_LOG_WARN, "spatial: %s is a second %s controller; it isn't tracked",
				controller_name(entry.controller), hand_name(hand));
			return;
		}
	entry.accessory = event.accessory;
	entry.hand = hand;
	locations = [NSMutableString string];
	ar_strings_t names = ar_accessory_copy_location_names(event.accessory);
	ar_strings_enumerate_strings(names, ^bool(const char *name) {
		[locations appendFormat:@"%s%s", locations.length ? " " : "", name];
		return true;
	});
	host_logf(HOST_LOG_INFO, "spatial: %s %s loaded; locations %s", ar_accessory_get_name(event.accessory),
		hand_name(hand), locations.UTF8String);
	host_spatial_controls_log_names(entry.controller, hand_name(hand));
	if (!entry.haptics)
		entry.haptics = host_spatial_haptics_start(entry.controller, hand_name(hand));
	host_spatial_rumble_attach(hand, entry.haptics);
	dirty = 1;
	[self publishOwnership];
}

- (void)disconnect:(GCController *)controller
{
	HostSpatialController *entry = [self entryFor:controller];

	if (!entry)
		return;
	host_logf(HOST_LOG_INFO, "spatial: %s%s%s disconnected", controller_name(controller), entry.hand >= 0 ? " " : "",
		entry.hand >= 0 ? hand_name(entry.hand) : "");
	if (entry.accessory)
	{
		dirty = 1;
		/* its rumble stops with it; the other hand's at the next feedback,
		since one controller alone isn't controller aim's */
		host_spatial_rumble_attach(entry.hand, nil);
	}
	[controllers removeObject:entry];
	[self publishOwnership];
}

/* host_spatial_owns_sense_controllers: both hands loaded */
- (void)publishOwnership
{
	int loaded = 0;

	for (HostSpatialController *entry in controllers)
		loaded += entry.accessory != nil;
	__atomic_store_n(&owns_sense_controllers, loaded == 2, __ATOMIC_RELEASE);
}

- (void)authorized:(HostSpatialEvent *)event
{
	if (event.status == ar_authorization_status_allowed)
	{
		authorization = AUTHORIZATION_ALLOWED;
		host_logf(HOST_LOG_INFO, "spatial: accessory tracking allowed");
		/* a load that failed before the permission (if loading needs it)
		gets a fresh try */
		for (HostSpatialController *entry in controllers)
			if (!entry.accessory && !entry.loading)
			{
				host_logf(HOST_LOG_INFO, "spatial: loading %s again now that tracking is allowed",
					controller_name(entry.controller));
				entry.attempts = 0;
				[self load:entry];
			}
		return;
	}
	if (event.status == ar_authorization_status_not_determined)
	{
		/* undecided (a dismissed prompt): WAITING, and asked again at the
		next controller's connect or the next opening of the space */
		authorization = AUTHORIZATION_NONE;
		wants_authorization = 0;
		if (!undecided_logged)
		{
			undecided_logged = 1;
			host_logf(HOST_LOG_INFO, "spatial: the accessory tracking permission wasn't decided; aiming with the "
				"head until it's asked again (the next controller to connect, or the next time the space opens)");
		}
		return;
	}
	authorization = AUTHORIZATION_DENIED;
	host_logf(HOST_LOG_WARN, "spatial: accessory tracking %s; aiming with the head",
		event.status == ar_authorization_status_denied ? "denied" : event.text.UTF8String);
}

- (void)drain
{
	NSArray<HostSpatialEvent *> *taken = nil;
	ar_session_t session;

	os_unfair_lock_lock(&lock);
	if (events.count)
	{
		taken = [events copy];
		[events removeAllObjects];
	}
	os_unfair_lock_unlock(&lock);
	for (HostSpatialEvent *event in taken)
		switch (event.kind)
		{
		case EVENT_CONNECT: [self connect:event.controller]; break;
		case EVENT_DISCONNECT: [self disconnect:event.controller]; break;
		case EVENT_AUTHORIZATION: [self authorized:event]; break;
		case EVENT_LOADED: [self loaded:event]; break;
		case EVENT_UPDATED:
			updating = 0;
			if (event.status)
				host_logf(HOST_LOG_INFO, "spatial: the accessory provider now tracks %s", event.text.UTF8String);
			else
				host_logf(HOST_LOG_WARN, "spatial: the accessory provider's update failed: %s", event.text.UTF8String);
			break;
		case EVENT_PROVIDER_STATE:
			host_logf(event.text ? HOST_LOG_WARN : HOST_LOG_INFO, "spatial: ARKit's data providers are %s%s%s",
				event.status == ar_data_provider_state_running ? "running" :
				event.status == ar_data_provider_state_paused ? "paused" :
				event.status == ar_data_provider_state_stopped ? "stopped" : "initialized",
				event.text ? ": " : "", event.text ? event.text.UTF8String : "");
			break;
		}
	if (!supported)
		return;
	/* the space closed, or opened again with a new session: a provider
	doesn't outlive its session */
	session = host_theater_session();
	if (ran_session && session != ran_session)
	{
		provider = nil;
		ran_session = nil;
		updating = 0;
		host_logf(HOST_LOG_INFO, "spatial: ARKit's session ended; the accessory provider starts again with the next");
	}
	if (!session)
		return;
	/* a new session (the space opened again) asks again after an undecided
	answer */
	if (session != seen_session)
	{
		seen_session = session;
		if (authorization == AUTHORIZATION_NONE && controllers.count)
			wants_authorization = 1;
	}
	if (wants_authorization && authorization == AUTHORIZATION_NONE)
	{
		__weak HostSpatialTracker *weak = self;

		authorization = AUTHORIZATION_REQUESTED;
		host_logf(HOST_LOG_INFO, "spatial: asking for the accessory tracking permission");
		/* the handler runs on an arbitrary queue */
		ar_session_request_authorization(session, ar_accessory_tracking_provider_get_required_authorization_type(),
			^(ar_authorization_results_t results, ar_error_t error) {
			__block long status = error ? -1 : ar_authorization_status_not_determined;

			if (results)
				ar_authorization_results_enumerate_results(results, ^bool(ar_authorization_result_t result) {
					status = ar_authorization_result_get_status(result);
					return true;
				});
			[weak push:EVENT_AUTHORIZATION status:status text:error ? error_text(error) : nil];
		});
	}
	if (authorization == AUTHORIZATION_ALLOWED)
		[self track:session];
}

/* the loaded accessories, and their hands for the log */
- (ar_accessories_t)loadedAccessories:(NSMutableString *)names
{
	ar_accessories_t accessories = ar_accessories_create();

	for (HostSpatialController *entry in controllers)
		if (entry.accessory)
		{
			ar_accessories_add_accessory(accessories, entry.accessory);
			[names appendFormat:@"%@the %s controller", names.length ? @" and " : @"", hand_name(entry.hand)];
		}
	return accessories;
}

/* the provider over the loaded accessories: made with the first, then
updated as they come and go */
- (void)track:(ar_session_t)session
{
	NSMutableString *names = [NSMutableString string];
	ar_accessories_t accessories;
	__weak HostSpatialTracker *weak = self;

	if (provider)
	{
		if (!dirty || updating)
			return;
		dirty = 0;
		accessories = [self loadedAccessories:names];
		/* (none left: the provider keeps the last, and their hands read as
		disconnected) */
		if (ar_accessories_get_count(accessories) == 0)
			return;
		updating = 1;
		NSString *tracked = [names copy];
		ar_accessory_tracking_provider_update_accessories(provider, accessories, ^(bool success, ar_error_t error) {
			[weak push:EVENT_UPDATED status:success text:success ? tracked : error_text(error)];
		});
		return;
	}
	accessories = [self loadedAccessories:names];
	if (ar_accessories_get_count(accessories) == 0)
		return;
	dirty = 0;
	ar_accessory_tracking_configuration_t configuration = ar_accessory_tracking_configuration_create();
	ar_accessory_tracking_configuration_set_accessories(configuration, accessories);
	provider = ar_accessory_tracking_provider_create(configuration);
	/* routine anchor updates are off without a handler
	(accessory_tracking.h); the samples read the latest anchors themselves */
	ar_accessory_tracking_provider_set_update_handler(provider, provider_queue,
		^(ar_accessory_anchors_t added, ar_accessory_anchors_t updated, ar_accessory_anchors_t removed) {});
	ar_session_set_data_provider_state_change_handler(session, provider_queue,
		^(ar_data_providers_t providers, ar_data_provider_state_t state, ar_error_t error,
		ar_data_provider_t failed) {
		[weak push:EVENT_PROVIDER_STATE status:state text:error ? error_text(error) : nil];
	});
	rerun_device_position = last_device_position;
	rerun_device_known = last_device_known;
	rerun_pending = 1;
	host_theater_session_run(provider);
	ran_session = session;
	host_logf(HOST_LOG_INFO, "spatial: the accessory provider tracks %s; ARKit's session runs again with it",
		names.UTF8String);
}

- (int)reason
{
	int connected = 0, loaded = 0, loading = 0;

	if (!supported)
		return HALO_SPATIAL_NO_CONTROLLERS;
	if (authorization == AUTHORIZATION_DENIED)
		return HALO_SPATIAL_DENIED;
	for (HostSpatialController *entry in controllers)
	{
		connected++;
		loaded += entry.accessory != nil;
		loading += entry.loading;
	}
	if (connected == 0)
		return HALO_SPATIAL_NO_CONTROLLERS;
	/* the permission or a load pending, or the provider not yet tracking
	what has loaded */
	if (loading || authorization != AUTHORIZATION_ALLOWED || (loaded && (!provider || updating || dirty)))
		return HALO_SPATIAL_WAITING;
	return loaded >= 2 ? HALO_SPATIAL_READY : loaded == 1 ? HALO_SPATIAL_ONE_CONTROLLER : HALO_SPATIAL_NO_CONTROLLERS;
}

/* each loaded controller with its newest anchor in the provider's latest
(nil: none yet). The collection is the provider's and changes with its next
call, so each anchor is used here and not kept */
- (void)latest:(void (^)(HostSpatialController *entry, ar_accessory_anchor_t anchor))each
{
	NSUInteger count = controllers.count;
	NSMutableArray *newest = [NSMutableArray arrayWithCapacity:count];

	for (NSUInteger index = 0; index < count; index++)
		[newest addObject:NSNull.null];
	if (provider && ran_session)
	{
		ar_accessory_anchors_t latest = ar_accessory_tracking_provider_get_latest_anchors(provider);

		ar_accessory_anchors_enumerate_anchors(latest, ^bool(ar_accessory_anchor_t anchor) {
			ar_accessory_t accessory = ar_accessory_anchor_get_accessory(anchor);

			for (NSUInteger index = 0; index < count; index++)
			{
				HostSpatialController *entry = controllers[index];
				id held = newest[index];

				if (!entry.accessory || !ar_accessory_is_equal_to_accessory(entry.accessory, accessory))
					continue;
				if (held == NSNull.null ||
					ar_accessory_anchor_get_timestamp(anchor) > ar_accessory_anchor_get_timestamp(held))
					newest[index] = anchor;
			}
			return true;
		});
	}
	for (NSUInteger index = 0; index < count; index++)
		if (controllers[index].accessory)
			each(controllers[index], newest[index] == NSNull.null ? nil : newest[index]);
}

/* a hand from its anchor (nil: connected, no anchor yet): the state, held,
the held hand, and both locations; an orientation-only state keeps the
last tracked positions */
- (void)hand:(struct halo_spatial_hand *)hand entry:(HostSpatialController *)entry
	anchor:(ar_accessory_anchor_t)anchor head:(const float *)head_position
{
	simd_float4x4 aim, grip;

	memset(hand, 0, sizeof(*hand));
	hand->state = HALO_SPATIAL_UNTRACKED;
	if (!anchor)
		return;
	hand->state = state_of(ar_accessory_anchor_get_tracking_state(anchor));
	hand->held = ar_accessory_anchor_is_held(anchor);
	switch (ar_accessory_anchor_get_held_chirality(anchor))
	{
	case ar_accessory_chirality_left: hand->chirality = 1; break;
	case ar_accessory_chirality_right: hand->chirality = 2; break;
	default: hand->chirality = 0; break;
	}
	aim = origin_from_location(anchor, ar_accessory_location_name_aim);
	grip = origin_from_location(anchor, ar_accessory_location_name_grip);
	if (hand->state == HALO_SPATIAL_TRACKED || hand->state == HALO_SPATIAL_LOW_ACCURACY)
	{
		entry.aim_position = aim.columns[3].xyz;
		entry.grip_position = grip.columns[3].xyz;
		entry.position_known = 1;
	}
	else if (hand->state == HALO_SPATIAL_ORIENTATION && entry.position_known)
	{
		aim.columns[3].xyz = entry.aim_position;
		grip.columns[3].xyz = entry.grip_position;
	}
	location_of(aim, head_position, &hand->aim);
	location_of(grip, head_position, &hand->grip);
}

/* debug.controller_axes: the right controller's aim and grip axes in the
head's frame (ARKit's axes: x right, y up, z back), twice a second */
- (void)axes:(ar_accessory_anchor_t)anchor device:(simd_float4x4)origin_from_device
{
	double now = CACurrentMediaTime();
	simd_float4x4 device_from_origin, aim, grip;

	if (now - axes_logged_at < AXES_LOG_SECONDS)
		return;
	axes_logged_at = now;
	device_from_origin = simd_inverse(origin_from_device);
	aim = simd_mul(device_from_origin, origin_from_location(anchor, ar_accessory_location_name_aim));
	grip = simd_mul(device_from_origin, origin_from_location(anchor, ar_accessory_location_name_grip));
	host_logf(HOST_LOG_INFO, "spatial: right aim x (%.3f %.3f %.3f) y (%.3f %.3f %.3f) z (%.3f %.3f %.3f) "
		"grip x (%.3f %.3f %.3f) y (%.3f %.3f %.3f) z (%.3f %.3f %.3f) in the head's frame",
		aim.columns[0].x, aim.columns[0].y, aim.columns[0].z, aim.columns[1].x, aim.columns[1].y, aim.columns[1].z,
		aim.columns[2].x, aim.columns[2].y, aim.columns[2].z, grip.columns[0].x, grip.columns[0].y, grip.columns[0].z,
		grip.columns[1].x, grip.columns[1].y, grip.columns[1].z, grip.columns[2].x, grip.columns[2].y,
		grip.columns[2].z);
}

/* the reason, the hands and the held-hand log, for either sample: with a
drawable's trackable-anchor time (above 0), each hand tracked in position
predicted to it, and with the device's pose, the axes log. Returns the
aiming hand's anchor timestamp, 0 with none */
- (double)fill:(struct halo_spatial_sample *)sample head:(const float *)head_position predictTo:(double)time
	device:(const simd_float4x4 *)origin_from_device
{
	__block double aim_timestamp = 0.0;

	sample->reason = [self reason];
	[self latest:^(HostSpatialController *entry, ar_accessory_anchor_t anchor) {
		ar_accessory_anchor_t used = anchor;
		int held;

		if (anchor && time > 0.0 && ar_accessory_anchor_get_tracking_state(anchor) >=
			ar_accessory_anchor_tracking_state_position_orientation_tracked)
		{
			if (!entry.predicted)
				entry.predicted = ar_accessory_anchor_create();
			/* (it fails: the latest anchor stands) */
			if (ar_accessory_tracking_provider_predict_anchor_at_timestamp(self->provider, anchor, time,
				entry.predicted))
				used = entry.predicted;
		}
		[self hand:&sample->hands[entry.hand] entry:entry anchor:used head:head_position];
		if (used && origin_from_device && axes_log && entry.hand == 1)
			[self axes:used device:*origin_from_device];
		if (!anchor || entry.hand != aiming_hand)
			return;
		aim_timestamp = ar_accessory_anchor_get_timestamp(anchor);
		held = sample->hands[entry.hand].chirality;
		if (held && held - 1 != aiming_hand && !self->hand_logged)
		{
			self->hand_logged = 1;
			host_logf(HOST_LOG_INFO, "spatial: input.aim_hand is %s but the aiming controller is held in the %s hand",
				hand_name(aiming_hand), hand_name(held - 1));
		}
	}];
	return aim_timestamp;
}

/* the game sample: the latest anchors, the head of the last latch, and both
controllers' buttons of one instant (both at the aiming hand's anchor
timestamp, or both live, as host_spatial_gamepad.h's clock decides) mapped
onto the gamepad */
- (void)sampleGame:(struct halo_spatial_sample *)sample head:(const float *)head_position
{
	struct host_spatial_controls controls[2];
	id<GCDevicePhysicalInputState> states[2] = { nil, nil };
	double timestamp, now, read_time;
	int read = 0, aligned;

	[self drain];
	timestamp = [self fill:sample head:head_position predictTo:0.0 device:NULL];
	/* The origin can move between this game sample and the next head latch.
	Keep the previous drawing pose/head paired until that latch rebases; the
	controls below still come from this instant, and pose loss stays live. */
	if (host_stereo_room_recenter_pending())
		host_spatial_hold_poses(sample, &draw_sample);
	now = CACurrentMediaTime();
	memset(controls, 0, sizeof(controls));
	/* one way and one instant for both controllers (host_spatial_gamepad.h) */
	aligned = host_spatial_button_clock_choose(&button_clock, timestamp, now, &read_time);
	for (HostSpatialController *entry in controllers)
		if (entry.accessory)
		{
			read++;
			if (aligned)
				states[entry.hand] = [entry.controller.input inputStateForSpatialAccessoryAnchorTimestamp:read_time];
		}
	if (aligned && read == 2 && (!states[0] || !states[1]))
	{
		/* no state at that time for one of them: both live */
		host_spatial_button_clock_failed(&button_clock, now);
		aligned = 0;
	}
	if (read && aligned != read_mode_logged && read_mode_logs < READ_MODE_LOGS)
	{
		read_mode_logged = aligned;
		read_mode_logs++;
		if (aligned)
			host_logf(HOST_LOG_INFO, "spatial: the buttons are read at the aiming hand's anchor timestamp (%.3f s "
				"old)%s", now - timestamp, read_mode_logs == READ_MODE_LOGS ? "; no more of these" : "");
		else if (timestamp <= 0.0)
			host_logf(HOST_LOG_INFO, "spatial: the buttons are read live (the aiming hand has no anchor)%s",
				read_mode_logs == READ_MODE_LOGS ? "; no more of these" : "");
		else
			host_logf(HOST_LOG_INFO, "spatial: the buttons are read live (the aiming hand's anchor is %.3f s old)%s",
				now - timestamp, read_mode_logs == READ_MODE_LOGS ? "; no more of these" : "");
	}
	for (HostSpatialController *entry in controllers)
		if (entry.accessory)
			host_spatial_controls_read(aligned ? states[entry.hand] : [entry.controller.input capture],
				&controls[entry.hand]);
	/* both controllers, or none: one alone isn't supported */
	sample->gamepad_valid = read == 2;
	if (!sample->gamepad_valid)
	{
		atomic_store(&sense_menus_down_ms, 0);
		return;
	}
	host_spatial_gamepad_map(&controls[0], &controls[1], aiming_hand, &sample->gamepad);
	/* both menu buttons, for the chord that leaves the immersive space
	(host_sdl.c: SDL doesn't list the controllers while controller aim owns
	them) */
	atomic_store(&sense_menus_down_ms, controls[0].menu && controls[1].menu ?
		(uint64_t)(CACurrentMediaTime() * 1000.0) : 0);
	/* the buttons and analog buttons as they change (not the sticks), for
	the setup session's buttons step */
	if (sample->gamepad.buttons != logged_gamepad.buttons ||
		memcmp(sample->gamepad.analog, logged_gamepad.analog, sizeof(logged_gamepad.analog)))
	{
		const uint8_t *analog = sample->gamepad.analog;

		logged_gamepad = sample->gamepad;
		host_logf(HOST_LOG_INFO, "spatial: buttons %04x analog %u %u %u %u %u %u %u %u", sample->gamepad.buttons,
			analog[0], analog[1], analog[2], analog[3], analog[4], analog[5], analog[6], analog[7]);
	}
}

/* the tracking-loss buzz on the aiming hand's controller */
- (void)buzz
{
	for (HostSpatialController *entry in controllers)
		if (entry.accessory && entry.hand == aiming_hand)
			host_spatial_haptics_buzz(entry.haptics, hand_name(entry.hand));
}

/* the drawing sample, from host_stereo_frame (a NULL drawable: none yet
this frame) */
- (void)frame:(cp_drawable_t)drawable device:(const simd_float4x4 *)origin_from_device
{
	float back[3], position[3] = { 0.0f, 0.0f, 0.0f };
	struct halo_spatial_sample reference;
	double time;
	int level = 1, hold_reference;

	draw_fresh = 0;
	if (!drawable)
		return;
	/* the misplaced test's restart, on the thread that reads the device
	anchor: a new session and world tracking provider (host_theater.m); the
	accessory provider follows it (drain) */
	if (atomic_exchange(&misplaced_restart_wanted, 0))
		host_theater_session_restart();
	hold_reference = !origin_from_device && host_stereo_room_recenter_pending();
	if (hold_reference)
		reference = draw_sample;
	[self drain];
	memset(&draw_sample, 0, sizeof(draw_sample));
	draw_sample.aiming_hand = aiming_hand;
	draw_sample.head_level = 1;
	if (origin_from_device)
	{
		simd_float4x4 device = *origin_from_device;

		back[0] = device.columns[2].x;
		back[1] = device.columns[2].y;
		back[2] = device.columns[2].z;
		position[0] = device.columns[3].x;
		position[1] = device.columns[3].y;
		position[2] = device.columns[3].z;
		draw_sample.head_yaw = host_spatial_head_yaw(back, &level) + host_stereo_room_yaw_offset();
		draw_sample.head_level = level;
		if (rerun_pending)
		{
			rerun_pending = 0;
			if (rerun_device_known)
				host_logf(HOST_LOG_INFO, "spatial: session re-run; device origin moved %.3f m (the device at %.2f %.2f "
					"%.2f in ARKit's frame)", simd_distance(rerun_device_position, device.columns[3].xyz),
					device.columns[3].x, device.columns[3].y, device.columns[3].z);
			else
				host_logf(HOST_LOG_INFO, "spatial: session re-run; no device anchor before it to compare");
		}
		last_device_position = device.columns[3].xyz;
		last_device_known = 1;
	}
	else if (host_stereo_last_head(back, position))
	{
		draw_sample.head_yaw = host_spatial_head_yaw(back, &level) + host_stereo_room_yaw_offset();
		draw_sample.head_level = level;
	}
	time = cp_time_to_cf_time_interval(cp_frame_timing_get_trackable_anchor_time(
		cp_drawable_get_frame_timing(drawable)));
	[self fill:&draw_sample head:position predictTo:time device:origin_from_device];
	/* A drawable without its device anchor cannot rebase the origin yet.
	Preserve the last paired drawing reference as well as the game one. */
	if (hold_reference)
		host_spatial_hold_poses(&draw_sample, &reference);
	draw_fresh = 1;
}

- (int)drawSample:(struct halo_spatial_sample *)sample
{
	if (!draw_fresh)
		return 0;
	*sample = draw_sample;
	return 1;
}

@end

static void tracker_start(void)
{
	if (@available(visionOS 27.0, *))
		tracker = [HostSpatialTracker new];
	else
	{
		needs_27 = 1;
		host_logf(HOST_LOG_INFO, "spatial: controller aim needs visionOS 27; aiming with the head");
	}
}

void host_spatial_frame(cp_drawable_t drawable, const simd_float4x4 *origin_from_device)
{
	if (!settings_read)
		read_settings();
	if (@available(visionOS 27.0, *))
		[(HostSpatialTracker *)tracker frame:drawable device:origin_from_device];
}

/* the tracker's sample of this kind: the drawing sample host_stereo_frame
took this frame, else the latest anchors with the last latched head */
static void tracker_sample(int kind, struct halo_spatial_sample *sample, const float head_position[3])
{
	if (@available(visionOS 27.0, *))
	{
		HostSpatialTracker *spatial = tracker;

		if (!spatial || (kind == HALO_SPATIAL_SAMPLE_DRAW && [spatial drawSample:sample]))
			return;
		if (kind == HALO_SPATIAL_SAMPLE_GAME)
			[spatial sampleGame:sample head:head_position];
		else
		{
			[spatial drain];
			[spatial fill:sample head:head_position predictTo:0.0 device:NULL];
		}
	}
}
#endif

void host_spatial_sample(int kind, struct halo_spatial_sample *sample)
{
	float back[3], position[3] = { 0.0f, 0.0f, 0.0f };

	if (!settings_read)
		read_settings();
	memset(sample, 0, sizeof(*sample));
	sample->aiming_hand = aiming_hand;
	/* the head at the last host_stereo_frame's latch: for the drawing
	sample, taken after it, this frame's */
	sample->head_level = 1;
	if (host_stereo_last_head(back, position))
	{
		int level;

		sample->head_yaw = host_spatial_head_yaw(back, &level) + host_stereo_room_yaw_offset();
		sample->head_level = level;
	}
	if (replay_on && host_spatial_replay_next(kind, sample))
		;
	else if (synthetic_on)
	{
		double now = seconds_now();

		if (synthetic_start < 0.0)
			synthetic_start = now;
		sample->reason = HALO_SPATIAL_READY;
		sample->synthetic = 1;
		/* the left hand stays HALO_SPATIAL_NONE */
		host_spatial_synthetic_hand(&synthetic, now - synthetic_start, &sample->hands[1]);
	}
	else
	{
		sample->reason = aim_controller ? HALO_SPATIAL_NO_CONTROLLERS : HALO_SPATIAL_OFF;
#if TARGET_OS_VISION
		if (needs_27)
			sample->reason = HALO_SPATIAL_NEEDS_27;
		tracker_sample(kind, sample, position);
#else
		(void)kind;
#endif
	}
	host_spatial_record_write(kind, sample);
	/* a controller that reads well above the head isn't where it reads
	(the recording above keeps the sample as it came). Judged on the game
	sample, live or replayed (a replay's head is the recording's: its yaw is
	exactly 0 while there was none); the head aims until they agree */
	if (kind == HALO_SPATIAL_SAMPLE_GAME && sample->reason == HALO_SPATIAL_READY && sample->synthetic != 1)
	{
		const struct halo_spatial_hand *hand = &sample->hands[sample->aiming_hand ? 1 : 0];
		int has_head = sample->synthetic == 2 ? sample->head_yaw != 0.0f : host_stereo_last_head(back, position);
		int change = host_spatial_misplaced_update(&misplaced, hand, has_head, seconds_now());

		if (change > 0)
		{
			int restart = sample->synthetic != 2 && misplaced_restarts < MISPLACED_RESTARTS;

			host_logf(HOST_LOG_WARN, "spatial: the aiming controller reads %.2f m above the head (the head at %.2f "
				"%.2f %.2f in ARKit's frame): its position and the head's are not in one frame; the head aims "
				"until they agree%s", misplaced.up, position[0], position[1], position[2],
				restart ? "; starting ARKit's session again" : "");
			if (restart)
			{
				misplaced_restarts++;
				atomic_store(&misplaced_restart_wanted, 1);
			}
		}
		else if (change < 0)
			host_logf(HOST_LOG_INFO, "spatial: the aiming controller is back under the head (%.2f m): the hand aims again",
				misplaced.up);
	}
	if (misplaced.flagged && sample->reason == HALO_SPATIAL_READY)
		sample->reason = HALO_SPATIAL_MISPLACED;
	if (sample->reason != reason_logged)
	{
		reason_logged = sample->reason;
		if (sample->synthetic == 2)
			host_logf(HOST_LOG_INFO, "spatial: controllers %d (replayed)", sample->reason);
		else if (synthetic_on)
			host_logf(HOST_LOG_INFO, "spatial: controllers %d (synthetic right hand: %s)", sample->reason,
				synthetic_value);
		else
			host_logf(HOST_LOG_INFO, "spatial: controllers %d", sample->reason);
	}
}

void host_spatial_feedback(const struct halo_spatial_feedback *feedback)
{
	int aiming = feedback->aiming != 0;

	if (aiming != feedback_aiming)
	{
		feedback_aiming = aiming;
		host_logf(HOST_LOG_INFO, "spatial: hands %s", aiming ? "hidden" : "shown");
#if TARGET_OS_VISION
		/* the drawn gun stands in for the hand (Theater.swift; the main
		queue's) */
		host_theater_set_upper_limbs(!aiming);
#endif
	}
	if (feedback->buzz_serial != feedback_buzz_serial)
	{
		feedback_buzz_serial = feedback->buzz_serial;
		host_logf(HOST_LOG_INFO, "spatial: buzz");
#if TARGET_OS_VISION
		if (@available(visionOS 27.0, *))
			[(HostSpatialTracker *)tracker buzz];
#endif
	}
#if TARGET_OS_VISION
	if (tracker)
	{
		struct host_spatial_rumble rumble;
		double now;

		/* only while both controllers are controller aim's: one alone is
		SDL's gamepad again (host_sdl_controller_aim_owns) */
		host_spatial_rumble_map(feedback->rumble[0], feedback->rumble[1], aiming_hand,
			host_spatial_owns_sense_controllers() ? rumble_strength : 0.0f, &rumble);
		host_spatial_rumble_play(&rumble);
		/* for a headset session's log: what reached the controllers */
		rumble_peak[0] = fmaxf(rumble_peak[0], rumble.intensity[0]);
		rumble_peak[1] = fmaxf(rumble_peak[1], rumble.intensity[1]);
		now = seconds_now();
		if ((rumble_peak[0] > 0.0f || rumble_peak[1] > 0.0f) && now - rumble_logged >= RUMBLE_LOG_SECONDS)
		{
			host_logf(HOST_LOG_INFO, "spatial: rumble left %.2f right %.2f (motors %.2f %.2f)", rumble_peak[0],
				rumble_peak[1], feedback->rumble[0], feedback->rumble[1]);
			rumble_logged = now;
			rumble_peak[0] = rumble_peak[1] = 0.0f;
		}
	}
#endif
}

/* for host_lifecycle.m and host_exit (declared in ios_host.h): the app in
the background (1) or back (0), or quitting (1); any thread */
void host_sdl_rumble_hold(int hold);
void host_spatial_rumble_hold(int hold)
{
#if TARGET_OS_VISION
	host_spatial_rumble_quiet(hold);
#endif
	/* and SDL's gamepads, on every platform */
	host_sdl_rumble_hold(hold);
}
