#import <TargetConditionals.h>
#include "host_audio_output.h"

#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST && !TARGET_OS_VISION
#import <UIKit/UIKit.h>
#import <AVKit/AVKit.h>
#include <SDL3/SDL.h>
#include "ios_host.h"

static UIButton *audio_button;
static UIViewController *audio_panel;

@interface VISRAudioOutput : NSObject
+ (void)show;
+ (void)done;
@end
@implementation VISRAudioOutput
+ (void)show {
    UIWindow *window=(__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(host_sdl_window()),
        SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER,NULL);
    UIViewController *presenter=window.rootViewController;
    if(!presenter || presenter.presentedViewController)return;
    audio_panel=[UIViewController new];
    audio_panel.title=@"Audio Output";
    audio_panel.view.backgroundColor=UIColor.systemBackgroundColor;
    audio_panel.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc]
        initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(done)];
    UILabel *title=[UILabel new];title.text=@"Audio Output";
    title.numberOfLines=0;title.font=[UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    AVRoutePickerView *picker=[AVRoutePickerView new];
    picker.prioritizesVideoDevices=NO;
    picker.accessibilityLabel=@"Choose audio output";
    [picker.heightAnchor constraintEqualToConstant:52].active=YES;
    UILabel *delay=[UILabel new];delay.text=@"Wireless audio can be delayed. Use this device's speakers or wired audio for faster response.";
    delay.numberOfLines=0;delay.font=[UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    UILabel *tv=[UILabel new];tv.text=@"Play on a TV: open Control Center, tap Screen Mirroring, and choose your TV. Keep your controller connected to this device.";
    tv.numberOfLines=0;tv.font=[UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    UIStackView *stack=[[UIStackView alloc]initWithArrangedSubviews:@[title,picker,delay,tv]];
    stack.axis=UILayoutConstraintAxisVertical;stack.spacing=16;stack.translatesAutoresizingMaskIntoConstraints=NO;
    UIScrollView *scroll=[UIScrollView new];scroll.translatesAutoresizingMaskIntoConstraints=NO;
    [audio_panel.view addSubview:scroll];[scroll addSubview:stack];
    for(UILabel *label in @[title,delay,tv]) {
        label.adjustsFontForContentSizeCategory=YES;
        [label setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisVertical];
    }
    UILayoutGuide *safe=audio_panel.view.safeAreaLayoutGuide;
    UILayoutGuide *content=scroll.contentLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [scroll.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [scroll.topAnchor constraintEqualToAnchor:safe.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor],
        [stack.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:24],
        [stack.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-24],
        [stack.topAnchor constraintEqualToAnchor:content.topAnchor constant:24],
        [stack.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-24],
        [stack.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor constant:-48]]];
    UINavigationController *navigation=[[UINavigationController alloc]initWithRootViewController:audio_panel];
    navigation.modalPresentationStyle=UIModalPresentationPageSheet;
    [presenter presentViewController:navigation animated:YES completion:nil];
}
+ (void)done {
    [audio_panel.navigationController dismissViewControllerAnimated:YES completion:nil];audio_panel=nil;
}
@end

int host_audio_output_up(void) {
    return audio_panel.navigationController.presentingViewController!=nil;
}
void host_audio_output_poll(void) {
    SDL_Window *sdl_window=host_sdl_window();
    if(!sdl_window)return;
    BOOL paused=SDL_GetHintBoolean("VISR_PAUSE_MENU",false);
    if(!paused && !audio_button)return;
    UIWindow *window=(__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(sdl_window),
        SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER,NULL);
    UIView *view=window.rootViewController.view;
    if(!view)return;
    if(!audio_button) {
        audio_button=[UIButton buttonWithType:UIButtonTypeSystem];
        audio_button.configuration=[UIButtonConfiguration filledButtonConfiguration];
        [audio_button setTitle:@"Audio Output" forState:UIControlStateNormal];
        [audio_button addTarget:VISRAudioOutput.class action:@selector(show) forControlEvents:UIControlEventTouchUpInside];
        audio_button.translatesAutoresizingMaskIntoConstraints=NO;
        [view addSubview:audio_button];
        [NSLayoutConstraint activateConstraints:@[
            [audio_button.trailingAnchor constraintEqualToAnchor:view.safeAreaLayoutGuide.trailingAnchor constant:-12],
            [audio_button.topAnchor constraintEqualToAnchor:view.safeAreaLayoutGuide.topAnchor constant:8],
            [audio_button.heightAnchor constraintGreaterThanOrEqualToConstant:44]]];
    }
    audio_button.hidden=!paused;
    if(paused)[view bringSubviewToFront:audio_button];
    else if(audio_panel) {
        [audio_panel.navigationController dismissViewControllerAnimated:NO completion:nil];audio_panel=nil;
    }
}
#else
int host_audio_output_up(void) { return 0; }
void host_audio_output_poll(void) {}
#endif
