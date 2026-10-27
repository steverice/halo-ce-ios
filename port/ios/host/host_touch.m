/* UIKit controls feed a standard SDL gamepad. The first hardware controller
   shares player one with touch; additional hardware controllers keep their ports. */
#import <UIKit/UIKit.h>
#import <GameController/GameController.h>
#include <SDL3/SDL.h>
#include "ios_host.h"
#import "host_orientation.h"
#include <math.h>
#include "host_touch_replay.h"
#include "host_touch_controller.h"

static struct host_touch_replay_gate replay_gate;
static void touch_replay_ui_update(void);
/* whether an attached overlay is out, so controller input knows to put it away (host_touch_controller.h) */
static atomic_int controls_showing, controller_hide_pending;

static int pause_pending;
void host_ios_touch_pause(void) { if (!host_touch_replay_blocked(&replay_gate)) pause_pending=1; }
static SDL_Joystick *touch_joystick;
static SDL_JoystickID touch_id, primary_hardware;

static void button_state(int button, BOOL down) {
    if(touch_joystick) SDL_SetJoystickVirtualButton(touch_joystick,button,
        host_touch_replay_blocked(&replay_gate)?NO:down);
}
static void axis_state(int axis, float value) {
    if(host_touch_replay_blocked(&replay_gate))value=axis>=SDL_GAMEPAD_AXIS_LEFT_TRIGGER?-1:0;
    if(touch_joystick) SDL_SetJoystickVirtualAxis(touch_joystick,axis,(Sint16)(fmaxf(-1,fminf(1,value))*32767));
}
void host_ios_touch_initialize(void) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.name="Halo Touch Controls";
    touch_id=SDL_AttachVirtualJoystick(&desc);
    touch_joystick=SDL_OpenJoystick(touch_id);
    if(!touch_joystick)host_fatal("Could not initialize touch controls: %s",SDL_GetError());
    axis_state(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-1);
    axis_state(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-1);
}
int host_ios_gamepads(uint32_t *out,int capacity) {
    int count=0,used=0;SDL_JoystickID *ids=SDL_GetGamepads(&count);
    primary_hardware=0;
    if(capacity>0 && touch_id)out[used++]=touch_id;
    for(int i=0;i<count;i++) {
        if(ids[i]==touch_id)continue;
        if(!primary_hardware) {
            primary_hardware=ids[i];
            if(!SDL_GetGamepadFromID(ids[i]))SDL_OpenGamepad(ids[i]);
        } else if(used<capacity) out[used++]=ids[i];
    }
    SDL_free(ids);return used;
}
int host_ios_gamepad_type(SDL_Gamepad *pad) {
    /* Keep the shared touch/hardware controller in the first recognized port. */
    return SDL_GetGamepadID(pad)==touch_id?SDL_GAMEPAD_TYPE_XBOX360:SDL_GetGamepadType(pad);
}
/* The guest publishes this hint while any in-game menu (the campaign's pause
   screen, a network game's pause menu, the scores) is up. The guest ignores
   the player's movement, look and buttons then (input_abstraction_update), so
   the sticks and D-pad reach only the menu; fire and grenade (the triggers)
   and the buttons the menus do not read are kept off the pad here too. */
static int touch_menu_up(void) { return SDL_GetHintBoolean("VISR_PAUSE_MENU",0); }
/* What the menus read from the pad: accept, back, the D-pad and Start. */
static int touch_menu_button(int button) {
    return button==SDL_GAMEPAD_BUTTON_SOUTH || button==SDL_GAMEPAD_BUTTON_EAST ||
        button==SDL_GAMEPAD_BUTTON_START || button==SDL_GAMEPAD_BUTTON_DPAD_UP ||
        button==SDL_GAMEPAD_BUTTON_DPAD_DOWN || button==SDL_GAMEPAD_BUTTON_DPAD_LEFT ||
        button==SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
}
int host_ios_gamepad_axis(SDL_Gamepad *pad,int axis) {
    int value=SDL_GetGamepadAxis(pad,axis);
    if(SDL_GetGamepadID(pad)==touch_id && (host_touch_replay_blocked(&replay_gate) ||
        (axis>=SDL_GAMEPAD_AXIS_LEFT_TRIGGER && touch_menu_up())))
        value=0;
    if(SDL_GetGamepadID(pad)==touch_id && primary_hardware) {
        int physical=SDL_GetGamepadAxis(SDL_GetGamepadFromID(primary_hardware),axis);
        if(abs(physical)>abs(value))value=physical;
    }
    return value;
}
int host_ios_gamepad_button(SDL_Gamepad *pad,int button) {
    if(button==SDL_GAMEPAD_BUTTON_START && SDL_GetGamepadID(pad)==touch_id && pause_pending && !host_touch_replay_blocked(&replay_gate)) {
        pause_pending=0;return 1;
    }
    return (!(SDL_GetGamepadID(pad)==touch_id && (host_touch_replay_blocked(&replay_gate) ||
                (touch_menu_up() && !touch_menu_button(button)))) && SDL_GetGamepadButton(pad,button)) ||
        (SDL_GetGamepadID(pad)==touch_id && primary_hardware &&
         SDL_GetGamepadButton(SDL_GetGamepadFromID(primary_hardware),button));
}
void host_ios_touch_reset(void) {
    pause_pending=0;
    for(int i=0;i<SDL_GAMEPAD_BUTTON_COUNT;i++)button_state(i,NO);
    for(int i=0;i<SDL_GAMEPAD_AXIS_COUNT;i++)axis_state(i,i>=SDL_GAMEPAD_AXIS_LEFT_TRIGGER?-1:0);
}

void host_touch_replay_active(int active) {
    if(host_touch_replay_set(&replay_gate,active,host_ios_touch_reset))
        dispatch_async(dispatch_get_main_queue(),^{ touch_replay_ui_update(); });
}

@interface HaloButton : UIButton
@property(nonatomic) int gameButton;
@property(nonatomic) int gameAxis;
@end
/* HALO_TOUCH_LATENCY=1: how long UIKit took to hand each press and lift to the controls (the touch's own timestamp against now),
   and the longest stall of the main run loop every 5 s, so touch lag can be told apart from slow frames. */
static int touch_latency_enabled(void) {static int on=-1;if(on<0)on=getenv("HALO_TOUCH_LATENCY")!=NULL;return on;}
static void touch_latency_log(const char *what,UITouch *touch) {
    if(!touch_latency_enabled())return;
    host_logf(HOST_LOG_INFO,"touch latency: %s delivered %.1f ms after the touch",what,(NSProcessInfo.processInfo.systemUptime-touch.timestamp)*1000);
}
static void touch_latency_watch(void) {
    static dispatch_once_t once;
    dispatch_once(&once,^{
        if(!touch_latency_enabled())return;
        __block double last=0,worst=0,since=0;
        CFRunLoopObserverRef observer=CFRunLoopObserverCreateWithHandler(NULL,kCFRunLoopBeforeSources|kCFRunLoopAfterWaiting,true,0,^(CFRunLoopObserverRef o,CFRunLoopActivity a){
            (void)o;double now=NSProcessInfo.processInfo.systemUptime;
            if(a==kCFRunLoopBeforeSources&&last>0)worst=MAX(worst,now-last);   /* (a pass of the sources and timers, handler time included) */
            if(a==kCFRunLoopAfterWaiting)last=now;
            if(now-since>=5){host_logf(HOST_LOG_INFO,"touch latency: longest main run loop pass in 5 s %.1f ms",worst*1000);worst=0;since=now;}
        });
        CFRunLoopAddObserver(CFRunLoopGetMain(),observer,kCFRunLoopCommonModes);
    });
}
@implementation HaloButton
- (void)sendDown:(BOOL)down {
    if(host_touch_replay_blocked(&replay_gate))down=NO;
    self.highlighted=down;
    if(self.gameAxis>=0)axis_state(self.gameAxis,down?1:-1);
    else button_state(self.gameButton,down);
}
- (BOOL)beginTrackingWithTouch:(UITouch *)touch withEvent:(UIEvent *)event {
    touch_latency_log("press",touch);[super beginTrackingWithTouch:touch withEvent:event];[self sendDown:YES];return YES;
}
- (void)endTrackingWithTouch:(UITouch *)touch withEvent:(UIEvent *)event {
    touch_latency_log("lift",touch);[super endTrackingWithTouch:touch withEvent:event];[self sendDown:NO];
}
- (void)cancelTrackingWithEvent:(UIEvent *)event {
    [super cancelTrackingWithEvent:event];[self sendDown:NO];
}
- (BOOL)accessibilityActivate {
    [self sendDown:YES];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,150*NSEC_PER_MSEC),dispatch_get_main_queue(),^{[self sendDown:NO];});
    return YES;
}
@end

@interface HaloStick : UIControl
@property(nonatomic) int firstAxis;
@property(nonatomic) CGPoint position;
@property(nonatomic,copy) NSString *caption;
@end
@implementation HaloStick
- (void)updateTouch:(UITouch *)touch {
    if(host_touch_replay_blocked(&replay_gate)){[self reset];return;}
    CGPoint p=[touch locationInView:self];CGFloat radius=self.bounds.size.width*.34;
    CGFloat x=(p.x-self.bounds.size.width/2)/radius,y=(p.y-self.bounds.size.height/2)/radius;
    CGFloat length=hypot(x,y);if(length>1){x/=length;y/=length;}
    self.position=CGPointMake(x,y);axis_state(self.firstAxis,x);axis_state(self.firstAxis+1,y);
    [self setNeedsDisplay];
}
- (BOOL)beginTrackingWithTouch:(UITouch *)touch withEvent:(UIEvent *)event {[self updateTouch:touch];return YES;}
- (BOOL)continueTrackingWithTouch:(UITouch *)touch withEvent:(UIEvent *)event {[self updateTouch:touch];return YES;}
- (void)reset {self.position=CGPointZero;axis_state(self.firstAxis,0);axis_state(self.firstAxis+1,0);[self setNeedsDisplay];}
- (void)endTrackingWithTouch:(UITouch *)touch withEvent:(UIEvent *)event {[self reset];}
- (void)cancelTrackingWithEvent:(UIEvent *)event {[self reset];}
- (void)drawRect:(CGRect)rect {
    CGRect circle=CGRectInset(self.bounds,2,2);
    [[UIColor colorWithWhite:0 alpha:.2]setFill];UIBezierPath *path=[UIBezierPath bezierPathWithOvalInRect:circle];[path fill];
    [[UIColor colorWithWhite:1 alpha:.5]setStroke];path.lineWidth=1.5;[path stroke];
    CGFloat size=self.bounds.size.width,r=size*.34;
    CGRect knob=CGRectMake(size/2+self.position.x*r-21,size/2+self.position.y*r-21,42,42);
    [[UIColor colorWithWhite:1 alpha:.25]setFill];[[UIBezierPath bezierPathWithOvalInRect:knob]fill];
    NSDictionary *attrs=@{NSFontAttributeName:[UIFont systemFontOfSize:10 weight:UIFontWeightSemibold],NSForegroundColorAttributeName:[UIColor colorWithWhite:1 alpha:.8]};
    CGSize text=[self.caption sizeWithAttributes:attrs];
    [self.caption drawAtPoint:CGPointMake((size-text.width)/2,size-20) withAttributes:attrs];
}
@end

@interface HaloControls : UIView
@property(nonatomic,strong) HaloStick *moveStick;
@property(nonatomic,strong) HaloStick *lookStick;
@property(nonatomic,strong) NSMutableArray<HaloButton *> *buttons;
@property(nonatomic,strong) UIButton *toggle;
/* the player's own Hide controls choice, and the overlay stepping aside for a hardware controller */
@property(nonatomic) BOOL manuallyHidden;
@property(nonatomic) BOOL controllerHidden;
@end
@implementation HaloControls
- (HaloButton *)addButton:(NSString *)title label:(NSString *)label button:(int)button axis:(int)axis {
    HaloButton *b=[HaloButton buttonWithType:UIButtonTypeCustom];b.gameButton=button;b.gameAxis=axis;
    [b setTitle:title forState:UIControlStateNormal];b.titleLabel.font=[UIFont systemFontOfSize:14 weight:UIFontWeightSemibold];
    b.backgroundColor=[UIColor colorWithWhite:0 alpha:.28];b.layer.borderColor=[UIColor colorWithWhite:1 alpha:.5].CGColor;b.layer.borderWidth=1;
    b.accessibilityLabel=label;b.exclusiveTouch=NO;[self addSubview:b];[self.buttons addObject:b];return b;
}
- (instancetype)initWithFrame:(CGRect)frame {
    if(!(self=[super initWithFrame:frame]))return nil;
    self.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
    self.multipleTouchEnabled=YES;self.buttons=[NSMutableArray array];
    self.moveStick=[[HaloStick alloc]init];self.moveStick.firstAxis=SDL_GAMEPAD_AXIS_LEFTX;self.moveStick.caption=@"MOVE";
    self.lookStick=[[HaloStick alloc]init];self.lookStick.firstAxis=SDL_GAMEPAD_AXIS_RIGHTX;self.lookStick.caption=@"LOOK";
    for(HaloStick *s in @[self.moveStick,self.lookStick]){s.backgroundColor=UIColor.clearColor;s.isAccessibilityElement=YES;s.accessibilityLabel=s.caption;[self addSubview:s];}
    [self addButton:@"A" label:@"A — Jump or select" button:SDL_GAMEPAD_BUTTON_SOUTH axis:-1];
    [self addButton:@"B" label:@"B — Melee or back" button:SDL_GAMEPAD_BUTTON_EAST axis:-1];
    [self addButton:@"X" label:@"X — Reload or use" button:SDL_GAMEPAD_BUTTON_WEST axis:-1];
    [self addButton:@"Y" label:@"Y — Switch weapon" button:SDL_GAMEPAD_BUTTON_NORTH axis:-1];
    [self addButton:@"FIRE" label:@"Fire" button:0 axis:SDL_GAMEPAD_AXIS_RIGHT_TRIGGER];
    [self addButton:@"GRENADE" label:@"Throw grenade" button:0 axis:SDL_GAMEPAD_AXIS_LEFT_TRIGGER];
    [self addButton:@"↑" label:@"Menu up" button:SDL_GAMEPAD_BUTTON_DPAD_UP axis:-1];
    [self addButton:@"↓" label:@"Menu down" button:SDL_GAMEPAD_BUTTON_DPAD_DOWN axis:-1];
    [self addButton:@"←" label:@"Menu left" button:SDL_GAMEPAD_BUTTON_DPAD_LEFT axis:-1];
    [self addButton:@"→" label:@"Menu right" button:SDL_GAMEPAD_BUTTON_DPAD_RIGHT axis:-1];
    [self addButton:@"PAUSE" label:@"Pause or start" button:SDL_GAMEPAD_BUTTON_START axis:-1];
    [self addButton:@"CROUCH" label:@"Crouch" button:SDL_GAMEPAD_BUTTON_LEFT_STICK axis:-1];
    [self addButton:@"ZOOM" label:@"Zoom" button:SDL_GAMEPAD_BUTTON_RIGHT_STICK axis:-1];
    [self addButton:@"LIGHT" label:@"Flashlight" button:SDL_GAMEPAD_BUTTON_LEFT_SHOULDER axis:-1];
    [self addButton:@"SWAP G" label:@"Switch grenade" button:SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER axis:-1];
    self.toggle=[UIButton buttonWithType:UIButtonTypeSystem];[self.toggle setTitle:@"Hide controls" forState:UIControlStateNormal];
    [self.toggle setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];self.toggle.titleLabel.font=[UIFont systemFontOfSize:12];
    self.toggle.backgroundColor=[UIColor colorWithWhite:0 alpha:.3];self.toggle.layer.cornerRadius=12;
    [self.toggle addTarget:self action:@selector(toggleControls) forControlEvents:UIControlEventTouchUpInside];[self addSubview:self.toggle];
    [[NSNotificationCenter defaultCenter]addObserver:self selector:@selector(reset) name:UIApplicationWillResignActiveNotification object:nil];
    /* The overlay hides while a game controller is connected, and returns when the last one
       disconnects or on a touch of the screen, and goes again when the controller is used
       (controller_input_watch). (Approach from CoreCE PR #1 (CC0).) */
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(controllersChanged) name:GCControllerDidConnectNotification object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(controllerDisconnected:) name:GCControllerDidDisconnectNotification object:nil];
    [self controllersChanged];
    return self;
}
- (void)controllersChanged {
    self.controllerHidden=GCController.controllers.count>0;[self applyVisibility];
}
- (void)controllerDisconnected:(NSNotification *)note {
    /* a disconnecting controller may still be listed while its notification is delivered */
    NSUInteger others=0;for(GCController *c in GCController.controllers)if(c!=note.object)others++;
    self.controllerHidden=others>0;[self applyVisibility];
}
- (void)applyVisibility {
    [self reset];
    BOOL hidden=self.manuallyHidden||self.controllerHidden;
    self.moveStick.hidden=self.lookStick.hidden=hidden;for(UIView *b in self.buttons)b.hidden=hidden;
    self.toggle.hidden=self.controllerHidden;
    atomic_store(&controls_showing,!self.controllerHidden);
    [self.toggle setTitle:self.manuallyHidden?@"Show controls":@"Hide controls" forState:UIControlStateNormal];
}
/* A hardware controller in use puts the overlay away again after a touch brought it back,
   and covers a controller connected before its notification could reach us. */
- (void)controllerUsed {
    if(self.controllerHidden)return;
    self.controllerHidden=YES;[self applyVisibility];
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if(self.controllerHidden){self.controllerHidden=NO;self.manuallyHidden=NO;[self applyVisibility];return;}
    [super touchesBegan:touches withEvent:event];
}
- (void)reset {
    host_ios_touch_reset();[self.moveStick cancelTrackingWithEvent:nil];[self.lookStick cancelTrackingWithEvent:nil];
    for(HaloButton *button in self.buttons)[button cancelTrackingWithEvent:nil];
    [self.toggle cancelTrackingWithEvent:nil];
}
- (void)toggleControls {
    if(host_touch_replay_blocked(&replay_gate))return;
    self.manuallyHidden=!self.manuallyHidden;[self applyVisibility];
}
- (void)layoutSubviews {
    [super layoutSubviews];CGRect r=UIEdgeInsetsInsetRect(self.bounds,self.safeAreaInsets);
    CGFloat left=r.origin.x+16,right=CGRectGetMaxX(r)-16,bottom=CGRectGetMaxY(r)-12,top=r.origin.y+10;
    CGFloat stick=MIN(136,r.size.height*.32),s=44,gap=4;
    self.moveStick.frame=CGRectMake(left,bottom-stick,stick,stick);
    self.lookStick.frame=CGRectMake(right-3*s-20-stick,bottom-stick,stick,stick);
    // Four face buttons in the traditional diamond.
    CGPoint face[4]={{right-2*s-gap,bottom-s},{right-s,bottom-2*s-gap},{right-3*s-2*gap,bottom-2*s-gap},{right-2*s-gap,bottom-3*s-2*gap}};
    for(int i=0;i<4;i++)self.buttons[i].frame=CGRectMake(face[i].x,face[i].y,s,s);
    self.buttons[4].frame=CGRectMake(right-84,top+55,84,48);
    self.buttons[5].frame=CGRectMake(left,top+55,90,48);
    CGFloat d=34,dx=left+stick+12,dy=bottom-3*d;
    self.buttons[6].frame=CGRectMake(dx+d,dy,d,d);
    self.buttons[7].frame=CGRectMake(dx+d,dy+2*d,d,d);
    self.buttons[8].frame=CGRectMake(dx,dy+d,d,d);
    self.buttons[9].frame=CGRectMake(dx+2*d,dy+d,d,d);
    self.buttons[10].frame=CGRectMake(CGRectGetMidX(r)-38,top,76,34);
    self.buttons[11].frame=CGRectMake(left,bottom-stick-42,74,34);
    self.buttons[12].frame=CGRectMake(right-3*s-20-stick,bottom-stick-42,74,34);
    self.buttons[13].frame=CGRectMake(right-180,top+55,80,36);
    self.buttons[14].frame=CGRectMake(left+104,top+55,80,36);
    for(UIButton *b in self.buttons){b.layer.cornerRadius=MIN(b.bounds.size.width,b.bounds.size.height)/2;b.titleLabel.adjustsFontSizeToFitWidth=YES;}
    self.toggle.frame=CGRectMake(right-108,top,108,32);
}
- (BOOL)pointInside:(CGPoint)point withEvent:(UIEvent *)event {
    if(host_touch_replay_blocked(&replay_gate))return NO;
    if(self.controllerHidden)return YES;   /* a touch brings the overlay back (touchesBegan:) */
    for(UIView *view in self.subviews)if(!view.hidden && [view pointInside:[self convertPoint:point toView:view] withEvent:event])return YES;
    return NO;
}
@end

static NSHashTable<HaloControls *> *attached_controls;
static void touch_replay_ui_update(void) {
    BOOL blocked=host_touch_replay_blocked(&replay_gate);
    for(HaloControls *controls in attached_controls) {
        [controls reset];
        controls.hidden=blocked;
        controls.userInteractionEnabled=!blocked;
    }
}

static bool SDLCALL controller_input_watch(void *userdata,SDL_Event *event) {
    (void)userdata;
    enum host_touch_controller_input input=HOST_TOUCH_CONTROLLER_OTHER;uint32_t device=0;int value=0;
    if(event->type==SDL_EVENT_GAMEPAD_BUTTON_DOWN){input=HOST_TOUCH_CONTROLLER_BUTTON_DOWN;device=event->gbutton.which;}
    else if(event->type==SDL_EVENT_GAMEPAD_AXIS_MOTION){input=HOST_TOUCH_CONTROLLER_AXIS;device=event->gaxis.which;value=event->gaxis.value;}
    else return true;
    if(!atomic_load(&controls_showing) || !host_touch_controller_hides(input,device,touch_id,value))return true;
    if(atomic_exchange(&controller_hide_pending,1))return true;
    dispatch_async(dispatch_get_main_queue(),^{
        atomic_store(&controller_hide_pending,0);
        for(HaloControls *controls in attached_controls)[controls controllerUsed];
    });
    return true;
}

void host_ios_touch_attach(SDL_Window *window) {
    UIWindow *native=(__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER,NULL);
    /* Retire SDL's temporary launch window before entering the non-returning
       game loop. Its fade animation can otherwise keep accessibility/input
       attached to the launch controller on recent scene-based iOS versions. */
    for(UIWindow *candidate in native.windowScene.windows) {
        if(candidate!=native && [NSStringFromClass(candidate.rootViewController.class) hasPrefix:@"SDLLaunch"])
            candidate.hidden=YES;
    }
    [native makeKeyAndVisible];
    host_ios_require_landscape(native);
    /* Keep the shared controller bridge active while Mac input uses no touch overlay. */
    if(NSProcessInfo.processInfo.isMacCatalystApp || NSProcessInfo.processInfo.isiOSAppOnMac)return;
    touch_latency_watch();
    UIView *root=native.rootViewController.view;
    HaloControls *controls=[[HaloControls alloc]initWithFrame:root.bounds];
    [root addSubview:controls];
    if(!attached_controls)attached_controls=[NSHashTable weakObjectsHashTable];
    [attached_controls addObject:controls];
    static bool watching;
    if(!watching){watching=true;SDL_AddEventWatch(controller_input_watch,NULL);}
    touch_replay_ui_update();
    host_logf(HOST_LOG_INFO,"touch controls attached, %.0fx%.0f",root.bounds.size.width,root.bounds.size.height);
}
