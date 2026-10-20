/* tvOS and visionOS stand-ins for the iOS touch controls and orientation lock.
   Apple TV has no touch screen: the Siri Remote stands in for the touch pad.
   On visionOS a game controller plays, the Mac's or a paired keyboard's
   arrows, Return and Escape work the menus as the remote's do, and a note asks
   for a controller while none is connected. Each platform's importer is its
   own file: host_tv_import.m on tvOS, host_import.m on visionOS. */
#import <UIKit/UIKit.h>
#include <SDL3/SDL.h>
#include "ios_host.h"
#include "host_config.h"
#include <stdlib.h>

/* The Siri Remote isn't a joystick (host_main.m), so SDL delivers its presses
   and swipes as keys. They drive a virtual "Halo Remote" gamepad that, like the
   iOS touch pad, owns player one (its buttons are only ever read through
   remote_read) and merges in the first hardware controller:
   either can work the menus, and Back backs out (B). The remote's keys are
   consumed here: the game's keyboard mapping (xinput_sdl.c) would otherwise
   also read Back, which SDL reports as Escape, as Start. */
void host_ios_touch_pause(void) {}
void host_touch_replay_active(int active) { (void)active; }
static SDL_Joystick *remote_joystick;
#if TARGET_OS_VISION
/* "Connect a game controller", shown while none is (host_ios_gamepads) */
static UILabel *controller_note;
#endif
static SDL_JoystickID remote_id, primary_hardware;
#if TARGET_OS_TV
/* true while tvOS's text dialog is up, and after it until every button of the
   hardware controller is let go: the game reads no controller then (below) */
static bool text_input_blocked(void);
#else
#define text_input_blocked() false
#endif
/* Remote presses latch until the game reads them: a swipe's key down and up
   arrive together, and a slow frame could otherwise miss a short press. Key
   events are pumped by the game's own poll, so all of this is on one thread. */
static bool remote_down[SDL_GAMEPAD_BUTTON_COUNT], remote_latched[SDL_GAMEPAD_BUTTON_COUNT];

static int remote_button(SDL_Scancode key) {
    switch(key) {
    case SDL_SCANCODE_UP:return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case SDL_SCANCODE_DOWN:return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case SDL_SCANCODE_LEFT:return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case SDL_SCANCODE_RIGHT:return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    case SDL_SCANCODE_RETURN:return SDL_GAMEPAD_BUTTON_SOUTH;
    case SDL_SCANCODE_ESCAPE:return SDL_GAMEPAD_BUTTON_EAST;
    case SDL_SCANCODE_PAUSE:return SDL_GAMEPAD_BUTTON_START;
    default:return -1;
    }
}
static bool SDLCALL remote_event(void *userdata,SDL_Event *event) {
    (void)userdata;
#if TARGET_OS_VISION
    /* Which events look and pinch makes: UIKit documents an indirect touch
       (SDL finger events, which the game ignores); a paired trackpad is an
       indirect pointer (mouse events). Logged once each, to find out. */
    static bool finger_seen,mouse_seen;
    if(event->type==SDL_EVENT_FINGER_DOWN && !finger_seen){finger_seen=true;host_logf(HOST_LOG_INFO,"first finger event (look and pinch?)");}
    if(event->type==SDL_EVENT_MOUSE_BUTTON_DOWN && !mouse_seen){mouse_seen=true;host_logf(HOST_LOG_INFO,"first mouse button event (pointer)");}
#endif
    if(event->type!=SDL_EVENT_KEY_DOWN && event->type!=SDL_EVENT_KEY_UP)return true;
    int button=remote_button(event->key.scancode);
    if(button<0)return true;
    if(text_input_blocked())return false;
    if(!event->key.repeat) {
        remote_down[button]=event->key.down;
        if(event->key.down)remote_latched[button]=true;
    }
    return false;
}
static bool remote_read(int button) {
    if(button<0 || button>=SDL_GAMEPAD_BUTTON_COUNT)return false;
    bool pressed=remote_down[button] || remote_latched[button];
    remote_latched[button]=false;
    return pressed;
}

void host_ios_touch_initialize(void) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.name="Halo Remote";
    remote_id=SDL_AttachVirtualJoystick(&desc);
    remote_joystick=SDL_OpenJoystick(remote_id);
    if(!remote_joystick)host_fatal("Could not initialize remote controls: %s",SDL_GetError());
    SDL_SetJoystickVirtualAxis(remote_joystick,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-32768);
    SDL_SetJoystickVirtualAxis(remote_joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768);
    SDL_SetEventFilter(remote_event,NULL);
}
void host_ios_touch_reset(void) {
    for(int i=0;i<SDL_GAMEPAD_BUTTON_COUNT;i++)remote_down[i]=remote_latched[i]=false;
}

void host_ios_touch_attach(SDL_Window *window) {
    UIWindow *native=(__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER,NULL);
    /* Same launch-window retirement as the iOS host (host_touch.m). */
    for(UIWindow *candidate in native.windowScene.windows) {
        if(candidate!=native && [NSStringFromClass(candidate.rootViewController.class) hasPrefix:@"SDLLaunch"])
            candidate.hidden=YES;
    }
    [native makeKeyAndVisible];
#if TARGET_OS_VISION
    controller_note=[UILabel new];
    controller_note.text=@"Connect a game controller to play Halo.";
    controller_note.font=[UIFont preferredFontForTextStyle:UIFontTextStyleTitle2];
    controller_note.textColor=UIColor.whiteColor;
    controller_note.backgroundColor=[UIColor colorWithWhite:0 alpha:0.6];
    controller_note.textAlignment=NSTextAlignmentCenter;
    controller_note.layer.cornerRadius=12;controller_note.clipsToBounds=YES;
    controller_note.translatesAutoresizingMaskIntoConstraints=NO;
    UIView *root=native.rootViewController.view;
    [root addSubview:controller_note];
    [controller_note.centerXAnchor constraintEqualToAnchor:root.centerXAnchor].active=YES;
    [controller_note.bottomAnchor constraintEqualToAnchor:root.bottomAnchor constant:-40].active=YES;
    /* 520 points, or the window's width less a margin if the window is narrower */
    NSLayoutConstraint *preferred=[controller_note.widthAnchor constraintEqualToConstant:520];
    preferred.priority=UILayoutPriorityDefaultHigh;preferred.active=YES;
    [controller_note.widthAnchor constraintLessThanOrEqualToAnchor:root.widthAnchor constant:-32].active=YES;
    [controller_note.heightAnchor constraintEqualToConstant:64].active=YES;
    host_logf(HOST_LOG_INFO,"visionOS window attached, %.0fx%.0f",native.bounds.size.width,native.bounds.size.height);
#else
    host_logf(HOST_LOG_INFO,"tvOS window attached, %.0fx%.0f",native.bounds.size.width,native.bounds.size.height);
#endif
}

/* Port assignment and merging mirror host_touch.m, with the remote in place of touch. */
int host_ios_gamepads(uint32_t *out,int capacity) {
    int count=0,used=0;SDL_JoystickID *ids=SDL_GetGamepads(&count);
    primary_hardware=0;
    if(capacity>0 && remote_id)out[used++]=remote_id;
    for(int i=0;i<count;i++) {
        if(ids[i]==remote_id || host_sdl_controller_aim_owns((uint32_t)ids[i]))continue;
        if(!primary_hardware) {
            primary_hardware=ids[i];
            if(!SDL_GetGamepadFromID(ids[i]))SDL_OpenGamepad(ids[i]);
        } else if(used<capacity) out[used++]=ids[i];
    }
#if TARGET_OS_VISION
    controller_note.hidden=primary_hardware!=0;
#endif
    SDL_free(ids);return used;
}
int host_ios_gamepad_type(SDL_Gamepad *pad) {
    SDL_Gamepad *physical=primary_hardware?SDL_GetGamepadFromID(primary_hardware):NULL;
    if(SDL_GetGamepadID(pad)!=remote_id)return SDL_GetGamepadType(pad);
    return physical?SDL_GetGamepadType(physical):SDL_GAMEPAD_TYPE_XBOX360;
}
int host_ios_gamepad_axis(SDL_Gamepad *pad,int axis) {
    if(text_input_blocked())return 0;
    int value=SDL_GetGamepadAxis(pad,axis);
    if(SDL_GetGamepadID(pad)==remote_id && primary_hardware) {
        int physical=SDL_GetGamepadAxis(SDL_GetGamepadFromID(primary_hardware),axis);
        if(abs(physical)>abs(value))value=physical;
    }
    return value;
}
int host_ios_gamepad_button(SDL_Gamepad *pad,int button) {
    if(text_input_blocked())return 0;
    if(SDL_GetGamepadID(pad)!=remote_id)return SDL_GetGamepadButton(pad,button);
    return remote_read(button) ||
        (primary_hardware && SDL_GetGamepadButton(SDL_GetGamepadFromID(primary_hardware),button));
}

#if TARGET_OS_TV
/* ---------- a menu's text field (host_sdl.c's host_sdl_text_input): tvOS's
   own dialog, a text field over its full-screen keyboard (SDL's hidden text
   field brings none up on an Apple TV). Done gives the text whole, Cancel
   nothing (host_tv_text_result); a field put away by the game dismisses it.
   On the main thread, as host_sdl_poll_event is. */
static UIAlertController *text_prompt;
static int text_prompt_state;
static char text_prompt_result[512];
/* the dialog as UIKit has it, until it is gone (text_prompt is nil from the
   moment Done or Cancel is chosen, while the dialog still animates away) */
static __weak UIAlertController *text_prompt_shown;
static bool text_prompt_gate, text_prompt_hint_set, text_prompt_hint_had;
static char text_prompt_hint_saved[16];

static bool text_prompt_up(void) {
    return text_prompt || text_prompt_shown.presentingViewController || text_prompt_shown.beingPresented;
}

/* Any button or stick of a hardware controller held down */
static bool hardware_held(void) {
    int count=0;SDL_JoystickID *ids=SDL_GetGamepads(&count);
    bool held=false;
    for(int i=0;i<count && !held;i++) {
        SDL_Gamepad *pad=SDL_GetGamepadFromID(ids[i]);
        if(!pad || ids[i]==remote_id)continue;
        for(int b=0;b<SDL_GAMEPAD_BUTTON_COUNT && !held;b++)held=SDL_GetGamepadButton(pad,(SDL_GamepadButton)b);
        for(int a=0;a<SDL_GAMEPAD_AXIS_COUNT && !held;a++)held=abs(SDL_GetGamepadAxis(pad,(SDL_GamepadAxis)a))>12000;
    }
    SDL_free(ids);
    return held;
}

/* SDL's tvOS view controller is a GCEventViewController, whose
   controllerUserInteractionEnabled stays NO unless SDL_HINT_APPLE_TV_CONTROLLER_UI_EVENTS
   is "1" (SDL's hint callback sets it, also when the hint changes later). With
   it off a controller's input never reaches UIKit's focus engine, so the
   dialog could not be reached while the game went on reading the same
   controller. So: the hint is "1" for as long as the dialog is up, and the
   game reads no controller then, nor while the button that closed the dialog
   is still held. */
static void text_prompt_hint(bool on) {
    if(on==text_prompt_hint_set)return;
    if(on) {
        const char *old=SDL_GetHint(SDL_HINT_APPLE_TV_CONTROLLER_UI_EVENTS);
        text_prompt_hint_had=old!=NULL;
        SDL_strlcpy(text_prompt_hint_saved,old?old:"",sizeof(text_prompt_hint_saved));
        SDL_SetHint(SDL_HINT_APPLE_TV_CONTROLLER_UI_EVENTS,"1");
    } else {
        SDL_SetHint(SDL_HINT_APPLE_TV_CONTROLLER_UI_EVENTS,text_prompt_hint_had?text_prompt_hint_saved:NULL);
    }
    text_prompt_hint_set=on;
}

static bool text_input_blocked(void) {
    if(text_prompt_up()) {
        text_prompt_gate=true;
        return true;
    }
    if(text_prompt_hint_set)text_prompt_hint(false);
    if(!text_prompt_gate)return false;
    if(hardware_held())return true;
    text_prompt_gate=false;
    host_ios_touch_reset();
    return false;
}

/* each poll (host_sdl.c's text_input_update): the controller goes back to the
   game as soon as the dialog is gone, however it went */
void host_tv_text_prompt_tick(void) {
    (void)text_input_blocked();
}

/* Done: the dialog's text, whole, for host_tv_text_result */
static void text_prompt_done(NSString *typed) {
    SDL_strlcpy(text_prompt_result,typed.UTF8String?:"",sizeof(text_prompt_result));
    text_prompt_state=1;
    text_prompt=nil;
}

/* the dialog put away with the full-screen keyboard presented over it
   (which outlasts the dialog when only the dialog is dismissed): everything
   its presenter presents goes */
static void text_prompt_dismiss(UIAlertController *alert) {
    UIViewController *presenter=alert.presentingViewController;
    [alert.textFields.firstObject resignFirstResponder];
    if(presenter)[presenter dismissViewControllerAnimated:YES completion:nil];
    else [alert dismissViewControllerAnimated:YES completion:nil];
}

/* Menu (a remote's, or B on a controller, which UIKit turns into Menu while
   controllerUserInteractionEnabled is on) is taken here, so it is never left
   unhandled: a Menu press nothing handles goes to the system, which leaves the
   app for the Home Screen. It puts away the keyboard if one is up, otherwise
   the dialog, as Cancel does. */
@interface HaloTextPromptMenu : NSObject
+ (void)menu:(UITapGestureRecognizer *)recognizer;
@end
@implementation HaloTextPromptMenu
+ (void)menu:(UITapGestureRecognizer *)recognizer {
    (void)recognizer;
    UIAlertController *alert=text_prompt;
    if(!alert)return;
    if(alert.presentedViewController) {
        host_logf(HOST_LOG_INFO,"keyboard: Menu or B over the text dialog: the keyboard goes");
        [alert dismissViewControllerAnimated:YES completion:nil];
        return;
    }
    host_logf(HOST_LOG_INFO,"keyboard: Menu or B over the text dialog: cancelled");
    text_prompt_state=2;
    text_prompt=nil;
    text_prompt_dismiss(alert);
}
@end

/* what asks the app to end while the dialog is up, logged to find out */
static bool SDLCALL text_prompt_watch(void *userdata,SDL_Event *event) {
    (void)userdata;
    if(text_prompt_up() && (event->type==SDL_EVENT_QUIT || event->type==SDL_EVENT_TERMINATING ||
        event->type==SDL_EVENT_WINDOW_CLOSE_REQUESTED || event->type==SDL_EVENT_WILL_ENTER_BACKGROUND))
        host_logf(HOST_LOG_INFO,"keyboard: SDL event %d while the text dialog is up",(int)event->type);
    return true;
}

/* the keyboard's own done: the dialog's Done too, so one press finishes */
@interface HaloTextPromptTarget : NSObject
@end
@implementation HaloTextPromptTarget
- (void)keyboardDone:(UITextField *)field {
    UIAlertController *alert=text_prompt;
    if(!alert || alert.textFields.firstObject!=field)return;
    text_prompt_done(field.text?:@"");
    text_prompt_dismiss(alert);
}
@end

int host_tv_text_prompt(int on, const char *title, const char *text, int masked) {
    if(!on){
        if(text_prompt)text_prompt_dismiss(text_prompt);
        text_prompt=nil;
        return 1;
    }
    if(text_prompt)return 1;
    SDL_Window *window=host_sdl_window();
    UIWindow *native=window?(__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER,NULL):nil;
    UIViewController *presenter=native.rootViewController;
    /* (a dialog still going away, a field ended and begun again at once: a
    presentation over it would be dropped, so this is asked again on the
    next poll) */
    if(!presenter || presenter.presentedViewController)return 0;
    text_prompt_state=0;
    UIAlertController *alert=[UIAlertController alertControllerWithTitle:@(title&&*title?title:"Text")
        message:nil preferredStyle:UIAlertControllerStyleAlert];
    NSString *initial=@(text?text:"");
    BOOL secure=masked!=0;
    [alert addTextFieldWithConfigurationHandler:^(UITextField *field){
        field.text=initial;
        field.secureTextEntry=secure;
        field.keyboardType=UIKeyboardTypeASCIICapable;
        field.autocorrectionType=UITextAutocorrectionTypeNo;
        field.autocapitalizationType=secure?UITextAutocapitalizationTypeNone:UITextAutocapitalizationTypeAllCharacters;
        static HaloTextPromptTarget *target;
        if(!target)target=[HaloTextPromptTarget new];
        [field addTarget:target action:@selector(keyboardDone:) forControlEvents:UIControlEventEditingDidEndOnExit];
    }];
    __weak UIAlertController *weak=alert;
    UIAlertAction *done=[UIAlertAction actionWithTitle:@"Done" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action){
        (void)action;
        text_prompt_done(weak.textFields.firstObject.text?:@"");
    }];
    [alert addAction:done];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction *action){
        (void)action;
        text_prompt_state=2;
        text_prompt=nil;
    }]];
    alert.preferredAction=done;
    UITapGestureRecognizer *menu=[[UITapGestureRecognizer alloc] initWithTarget:HaloTextPromptMenu.class action:@selector(menu:)];
    menu.allowedPressTypes=@[@(UIPressTypeMenu)];
    [alert.view addGestureRecognizer:menu];
    static bool watching;
    if(!watching){watching=true;SDL_AddEventWatch(text_prompt_watch,NULL);}
    text_prompt=alert;
    text_prompt_shown=alert;
    text_prompt_gate=true;
    text_prompt_hint(true);
    host_ios_touch_reset();
    /* (the field is chosen with the remote, which brings tvOS's full-screen
    keyboard up; a keyboard brought up from here instead stayed on screen
    after the dialog went, in the simulator) */
    [presenter presentViewController:alert animated:YES completion:^{
        /* the keyboard comes straight up, and a hardware keyboard types into the field */
        UIAlertController *shown=weak;
        if(shown && shown==text_prompt)[shown.textFields.firstObject becomeFirstResponder];
    }];
    /* (UIKit drops a presentation it cannot make now, saying nothing: then
    the dialog has no presenter, and this is asked again on the next poll) */
    if(!alert.presentingViewController){
        static int refusals;
        if(!refusals++)host_logf(HOST_LOG_INFO,"keyboard: tvOS's text dialog could not be shown yet; asking again");
        text_prompt=nil;
        text_prompt_hint(false);
        return 0;
    }
    host_logf(HOST_LOG_INFO,"keyboard: tvOS's text dialog up for a text field");
    /* debug.test_text_answer: answered for the simulator, whose keyboard
    cannot type into it */
    char answer[256];
    host_config_string("debug.test_text_answer","",answer,sizeof(answer));
    if(*answer){
        NSString *typed=@(answer);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,3*NSEC_PER_SEC),dispatch_get_main_queue(),^{
            UIAlertController *shown=weak;
            if(!shown || shown!=text_prompt)return;
            shown.textFields.firstObject.text=typed;
            host_logf(HOST_LOG_INFO,"keyboard: debug.test_text_answer typed into the dialog, and Done");
            text_prompt_done(typed);
            text_prompt_dismiss(shown);
        });
    }
    return 1;
}

int host_tv_text_result(char *buffer, uint32_t size) {
    int state=text_prompt_state;
    if(state==1 && buffer && size)SDL_strlcpy(buffer,text_prompt_result,size);
    text_prompt_state=0;
    return state;
}
#endif
