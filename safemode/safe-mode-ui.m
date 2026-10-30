#import "safe-mode-ui.h"
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#include <errno.h>
#include <unistd.h>
#include <math.h>

@interface LHSafeModeIndicatorWindow : UIWindow
@end

@implementation LHSafeModeIndicatorWindow
// 常驻入口不抢占桌面或前台应用的键盘焦点。
- (BOOL)canBecomeKeyWindow { return NO; }
// 保持系统状态栏原有的颜色、方向和显示策略。
- (BOOL)_canAffectStatusBarAppearance { return NO; }
@end

@interface LHSafeModeIndicatorView : UIView
@property(nonatomic, copy) void (^activate)(void);
@end

@implementation LHSafeModeIndicatorView
// 让旁白的激活动作与普通轻点共用恢复入口。
- (BOOL)accessibilityActivate {
    if (!self.activate) return NO;
    self.activate();
    return YES;
}
@end

@interface LHSafeModeLayoutController : UIViewController
@property(nonatomic, copy) void (^layoutChanged)(void);
@end

@implementation LHSafeModeLayoutController
// 自有窗口参与系统布局时刷新状态栏几何，不修改系统控制器的方法。
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    if (self.layoutChanged) self.layoutChanged();
}
// 安全区域变化通常伴随状态栏高度变化，合并到同一布局请求。
- (void)viewSafeAreaInsetsDidChange {
    [super viewSafeAreaInsetsDidChange];
    if (self.layoutChanged) self.layoutChanged();
}
// 旋转结束后使用最终场景坐标，避免定时轮询造成旧命中区域残留。
- (void)viewWillTransitionToSize:(CGSize)size withTransitionCoordinator:(id<UIViewControllerTransitionCoordinator>)coordinator {
    [super viewWillTransitionToSize:size withTransitionCoordinator:coordinator];
    __weak typeof(self) weakSelf = self;
    [coordinator animateAlongsideTransition:nil completion:^(id<UIViewControllerTransitionCoordinatorContext> context) {
        LHSafeModeLayoutController *controller = weakSelf;
        if (controller.layoutChanged) controller.layoutChanged();
    }];
}
@end

@interface LHSafeModeController : NSObject <UIGestureRecognizerDelegate>
@property(nonatomic, copy) NSString *markerPath;
@property(nonatomic) int persistenceError;
@property(nonatomic, strong) LHSafeModeIndicatorWindow *indicatorWindow;
@property(nonatomic, strong) UIWindow *alertWindow;
@property(nonatomic, weak) UIWindow *previousKeyWindow;
@property(nonatomic, strong) CALayer *pulseLayer;
@property(nonatomic, strong) NSTimer *sceneRetry;
@property(nonatomic, strong) NSMutableArray *observers;
@property(nonatomic, weak) UIWindowScene *suspendedScene;
@property(nonatomic) NSUInteger sceneRetryCount;
@property(nonatomic) BOOL sceneReady;
@property(nonatomic) BOOL applicationInactive;
@property(nonatomic) BOOL running;
@property(nonatomic) BOOL stopped;
@property(nonatomic) BOOL layoutPending;
@property(nonatomic) BOOL initialAlertPending;
@property(nonatomic) BOOL showingAlert;
- (void)start;
- (void)stop;
- (void)refreshLayout;
- (void)showAlert;
@end

@implementation LHSafeModeController
// 限定主屏系统状态栏场景，避免岛屿、外接屏和本模块窗口触发无关工作。
- (BOOL)isStatusBarScene:(UIScene *)candidate {
    if (![candidate isKindOfClass:UIWindowScene.class]) return NO;
    UIWindowScene *scene = (UIWindowScene *)candidate;
    if (scene.screen != UIScreen.mainScreen) return NO;
    Class statusWindowClass = NSClassFromString(@"SBStatusBarWindow");
    for (UIWindow *window in scene.windows) {
        if (statusWindowClass && [window isKindOfClass:statusWindowClass]) return YES;
    }
    return [scene.session.persistentIdentifier isEqualToString:@"com.apple.springboard"];
}

// 优先使用真实状态栏所属场景，避开灵动岛和外接屏场景。
- (UIWindowScene *)mainScene {
    Class statusWindowClass = NSClassFromString(@"SBStatusBarWindow");
    UIWindowScene *fallback = nil;
    for (UIScene *candidate in UIApplication.sharedApplication.connectedScenes) {
        if (![candidate isKindOfClass:UIWindowScene.class]) continue;
        UIWindowScene *scene = (UIWindowScene *)candidate;
        if (scene.screen != UIScreen.mainScreen ||
            (scene.activationState != UISceneActivationStateForegroundActive &&
             scene.activationState != UISceneActivationStateForegroundInactive)) continue;
        for (UIWindow *window in scene.windows) {
            if (statusWindowClass && [window isKindOfClass:statusWindowClass]) return scene;
        }
        if ([scene.session.persistentIdentifier isEqualToString:@"com.apple.springboard"]) fallback = scene;
    }
    return fallback;
}

// 合并同一轮场景与布局通知，窗口不变时不会反复提交布局。
- (void)requestLayout {
    if (!self.running || self.stopped || self.layoutPending) return;
    self.layoutPending = YES;
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        LHSafeModeController *controller = weakSelf;
        controller.layoutPending = NO;
        if (controller.running && !controller.stopped) [controller refreshLayout];
    });
}

// 撤销唯一的启动重试；稳定显示、退后台和停止时都不留下周期任务。
- (void)cancelSceneRetry {
    [self.sceneRetry invalidate];
    self.sceneRetry = nil;
}

// 仅首次场景尚未准备时有限退避；找到场景后完全由事件驱动。
- (void)scheduleSceneRetry {
    static const NSTimeInterval delays[] = {0.1, 0.25, 0.5, 1.0, 2.0, 4.0};
    if (!self.running || self.stopped || self.applicationInactive || self.suspendedScene ||
            self.sceneReady || self.sceneRetry || self.sceneRetryCount >= sizeof(delays) / sizeof(*delays)) return;
    NSTimeInterval delay = delays[self.sceneRetryCount++];
    __weak typeof(self) weakSelf = self;
    self.sceneRetry = [NSTimer timerWithTimeInterval:delay repeats:NO block:^(NSTimer *timer) {
        LHSafeModeController *controller = weakSelf;
        controller.sceneRetry = nil;
        if (controller.running && !controller.stopped) [controller refreshLayout];
    }];
    self.sceneRetry.tolerance = delay * 0.2;
    [NSRunLoop.mainRunLoop addTimer:self.sceneRetry forMode:NSRunLoopCommonModes];
}

// 通过公开生命周期事件暂停界面，恢复后立即更新，不轮询屏幕状态。
- (void)handleNotification:(NSNotification *)notification {
    NSString *name = notification.name;
    id object = notification.object;
    if ([object isKindOfClass:UIWindow.class]) {
        UIWindow *window = object;
        if (window == self.indicatorWindow || window == self.alertWindow ||
                ![self isStatusBarScene:window.windowScene]) return;
    } else if ([object isKindOfClass:UIScene.class]) {
        UIWindowScene *scene = (UIWindowScene *)object;
        if (scene != self.indicatorWindow.windowScene && scene != self.suspendedScene &&
                ![self isStatusBarScene:scene]) return;
        if ([name isEqualToString:UISceneWillDeactivateNotification] ||
                [name isEqualToString:UISceneDidEnterBackgroundNotification]) self.suspendedScene = scene;
        else if ([name isEqualToString:UISceneDidActivateNotification] ||
                [name isEqualToString:UISceneDidDisconnectNotification]) self.suspendedScene = nil;
    }
    if ([name isEqualToString:UIApplicationWillResignActiveNotification] ||
            [name isEqualToString:UIApplicationDidEnterBackgroundNotification]) self.applicationInactive = YES;
    else if ([name isEqualToString:UIApplicationDidBecomeActiveNotification]) {
        self.applicationInactive = NO;
        if (self.suspendedScene.activationState == UISceneActivationStateForegroundActive) self.suspendedScene = nil;
    }
    if (self.applicationInactive || self.suspendedScene) [self cancelSceneRetry];
    [self requestLayout];
}

// 注册场景、状态栏和窗口事件，只在启动场景未就绪时安排有限重试。
- (void)start {
    if (self.running) return;
    self.running = YES;
    self.stopped = NO;
    self.sceneRetryCount = 0;
    self.sceneReady = NO;
    self.suspendedScene = nil;
    self.initialAlertPending = YES;
    self.applicationInactive = UIApplication.sharedApplication.applicationState == UIApplicationStateBackground;
    self.observers = [NSMutableArray new];
    __weak typeof(self) weakSelf = self;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    for (NSNotificationName name in @[UIApplicationDidBecomeActiveNotification,
            UIApplicationWillResignActiveNotification, UIApplicationDidEnterBackgroundNotification,
            UIApplicationDidChangeStatusBarFrameNotification,
            UISceneDidActivateNotification, UISceneWillDeactivateNotification,
            UISceneDidEnterBackgroundNotification, UISceneDidDisconnectNotification,
            UIWindowDidBecomeVisibleNotification, UIWindowDidBecomeHiddenNotification,
            UIAccessibilityReduceMotionStatusDidChangeNotification]) {
        id token = [NSNotificationCenter.defaultCenter addObserverForName:name object:nil
            queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
                [weakSelf handleNotification:notification];
            }];
        [self.observers addObject:token];
    }
#pragma clang diagnostic pop
    [self refreshLayout];
}

// 结束展示时移除定时器、通知和窗口，避免留下触摸遮挡。
- (void)stop {
    self.running = NO;
    self.stopped = YES;
    [self cancelSceneRetry];
    for (id token in self.observers) [NSNotificationCenter.defaultCenter removeObserver:token];
    self.observers = nil;
    [self closeAlert];
    [self.pulseLayer removeAllAnimations];
    self.indicatorWindow.hidden = YES;
    self.indicatorWindow = nil;
    self.pulseLayer = nil;
}

// 只动画红色背景，保持点击区域和系统时间、电量的透明度不变。
- (void)updatePulse {
    BOOL animate = self.indicatorWindow && !self.indicatorWindow.hidden && !UIAccessibilityIsReduceMotionEnabled();
    if (!animate) {
        if ([self.pulseLayer animationForKey:@"safeModePulse"]) [self.pulseLayer removeAnimationForKey:@"safeModePulse"];
        if (self.pulseLayer.opacity != 0.24f) self.pulseLayer.opacity = 0.24f;
    } else if (![self.pulseLayer animationForKey:@"safeModePulse"]) {
        CABasicAnimation *animation = [CABasicAnimation animationWithKeyPath:@"opacity"];
        animation.fromValue = @0.12;
        animation.toValue = @0.42;
        animation.duration = 1.8;
        animation.autoreverses = YES;
        animation.repeatCount = HUGE_VALF;
        animation.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
        [self.pulseLayer addAnimation:animation forKey:@"safeModePulse"];
    }
}

// 按场景坐标限定窗口尺寸，状态栏以外不建立常驻触摸区域。
- (void)refreshLayout {
    if (self.stopped) return;
    UIWindowScene *scene = [self mainScene];
    BOOL suspended = self.applicationInactive || (self.suspendedScene && self.suspendedScene == scene);
    if (self.alertWindow && (self.alertWindow.windowScene != scene || suspended)) {
        self.initialAlertPending = YES;
        [self closeAlert];
    }
    if (!scene || suspended) {
        if (!self.indicatorWindow.hidden) self.indicatorWindow.hidden = YES;
        [self updatePulse];
        if (suspended) [self cancelSceneRetry];
        else [self scheduleSceneRetry];
        return;
    }
    self.sceneReady = YES;
    [self cancelSceneRetry];
    if (self.indicatorWindow.windowScene != scene) {
        self.indicatorWindow.hidden = YES;
        self.indicatorWindow = [[LHSafeModeIndicatorWindow alloc] initWithWindowScene:scene];
        self.indicatorWindow.windowLevel = UIWindowLevelStatusBar + 1;
        self.indicatorWindow.backgroundColor = UIColor.clearColor;
        LHSafeModeLayoutController *layoutController = [LHSafeModeLayoutController new];
        self.indicatorWindow.rootViewController = layoutController;
        LHSafeModeIndicatorView *view = [LHSafeModeIndicatorView new];
        __weak typeof(self) weakSelf = self;
        layoutController.layoutChanged = ^{ [weakSelf requestLayout]; };
        view.activate = ^{ [weakSelf showAlert]; };
        self.indicatorWindow.rootViewController.view = view;
        view.backgroundColor = UIColor.clearColor;
        view.isAccessibilityElement = YES;
        view.accessibilityTraits = UIAccessibilityTraitButton;
        view.accessibilityLabel = @"安全模式";
        view.accessibilityHint = @"轻点打开退出安全模式提示";
        UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(statusBarTapped:)];
        tap.cancelsTouchesInView = NO;
        tap.delegate = self;
        [view addGestureRecognizer:tap];
        self.pulseLayer = [CALayer layer];
        self.pulseLayer.backgroundColor = UIColor.redColor.CGColor;
        self.pulseLayer.opacity = 0.24;
        [view.layer addSublayer:self.pulseLayer];
    }
    UIStatusBarManager *manager = scene.statusBarManager;
    CGRect frame = manager ? CGRectIntersection(manager.statusBarFrame, scene.coordinateSpace.bounds) : CGRectZero;
    BOOL visible = manager && !manager.statusBarHidden && !CGRectIsEmpty(frame) && !CGRectIsNull(frame) &&
        (scene.activationState == UISceneActivationStateForegroundActive ||
         scene.activationState == UISceneActivationStateForegroundInactive);
    if (visible) {
        if (!CGRectEqualToRect(self.indicatorWindow.frame, frame)) self.indicatorWindow.frame = frame;
        CGRect bounds = self.indicatorWindow.bounds;
        if (!CGRectEqualToRect(self.indicatorWindow.rootViewController.view.frame, bounds))
            self.indicatorWindow.rootViewController.view.frame = bounds;
        if (!CGRectEqualToRect(self.pulseLayer.frame, bounds)) {
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            self.pulseLayer.frame = bounds;
            [CATransaction commit];
        }
    }
    if (self.indicatorWindow.hidden == visible) self.indicatorWindow.hidden = !visible;
    if (self.indicatorWindow.userInteractionEnabled == self.showingAlert)
        self.indicatorWindow.userInteractionEnabled = !self.showingAlert;
    [self updatePulse];
    if (self.initialAlertPending && visible) {
        self.initialAlertPending = NO;
        [self showAlert];
    }
}

// 仅完整轻点打开提示，拖动或下拉不触发退出操作。
- (void)statusBarTapped:(UITapGestureRecognizer *)recognizer {
    if (recognizer.state == UIGestureRecognizerStateRecognized) [self showAlert];
}

// 允许系统手势与轻点共同观察触摸，不主动阻止下拉手势。
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gestureRecognizer
        shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer *)otherGestureRecognizer {
    return YES;
}

// 统一释放提示窗口，并把焦点交还原来的系统窗口。
- (void)closeAlert {
    UIWindow *window = self.alertWindow;
    BOOL restoreKey = window.isKeyWindow;
    window.hidden = YES;
    self.alertWindow = nil;
    if (restoreKey && self.previousKeyWindow && !self.previousKeyWindow.hidden &&
            self.previousKeyWindow.windowScene == window.windowScene) [self.previousKeyWindow makeKeyWindow];
    self.previousKeyWindow = nil;
    self.showingAlert = NO;
    self.indicatorWindow.userInteractionEnabled = YES;
}

// 为提示选择同一主场景，创建前设置门禁，防止连续点击叠加窗口。
- (void)presentAlert:(UIAlertController *)alert {
    if (self.showingAlert || self.stopped || self.applicationInactive) return;
    UIWindowScene *scene = [self mainScene];
    if (!scene || scene == self.suspendedScene) return;
    self.showingAlert = YES;
    self.indicatorWindow.userInteractionEnabled = NO;
    for (UIWindow *window in scene.windows) {
        if (window.isKeyWindow) { self.previousKeyWindow = window; break; }
    }
    self.alertWindow = [[UIWindow alloc] initWithWindowScene:scene];
    self.alertWindow.frame = scene.coordinateSpace.bounds;
    self.alertWindow.windowLevel = UIWindowLevelAlert + 1;
    self.alertWindow.backgroundColor = UIColor.clearColor;
    self.alertWindow.rootViewController = [UIViewController new];
    self.alertWindow.rootViewController.view.backgroundColor = UIColor.clearColor;
    [self.alertWindow makeKeyAndVisible];
    [self.alertWindow.rootViewController presentViewController:alert animated:YES completion:nil];
}

// 删除失败保留安全模式，并提供可关闭的错误提示和后续重试入口。
- (void)showExitError:(int)error {
    NSString *reason = [NSError errorWithDomain:NSPOSIXErrorDomain code:error userInfo:nil].localizedDescription;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"暂时无法退出安全模式"
        message:[NSString stringWithFormat:@"无法清除安全模式标记：%@。处理后可再次点击红色状态栏重试。", reason]
        preferredStyle:UIAlertControllerStyleAlert];
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"保持安全模式" style:UIAlertActionStyleCancel
        handler:^(UIAlertAction *action) { [weakSelf closeAlert]; }]];
    [self presentAlert:alert];
}

// 状态栏点击和首次启动共用同一退出流程，保持操作只关闭弹窗。
- (void)showAlert {
    if (self.showingAlert) return;
    NSString *message = @"插件暂未加载。可先卸载最近安装的插件，再退出安全模式。选择保持后，可点击缓慢闪红的状态栏重新打开此提示。";
    if (self.persistenceError) {
        NSString *reason = [NSError errorWithDomain:NSPOSIXErrorDomain code:self.persistenceError userInfo:nil].localizedDescription;
        message = [message stringByAppendingFormat:@"\n\n本次 SpringBoard 已停用插件，但无法保存安全模式标记：%@。再次重启可能恢复插件加载，请先处理故障插件。", reason];
    }
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"libhooker 安全模式"
        message:message
        preferredStyle:UIAlertControllerStyleAlert];
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"保持安全模式" style:UIAlertActionStyleCancel
        handler:^(UIAlertAction *action) { [weakSelf closeAlert]; }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"退出安全模式" style:UIAlertActionStyleDestructive
        handler:^(UIAlertAction *action) {
            LHSafeModeController *controller = weakSelf;
            if (!controller) return;
            if (unlink(controller.markerPath.fileSystemRepresentation) == 0 || errno == ENOENT) exit(0);
            int error = errno;
            [controller closeAlert];
            dispatch_async(dispatch_get_main_queue(), ^{ [controller showExitError:error]; });
        }]];
    [self presentAlert:alert];
}
@end

// 由崩溃保护初始化完成后的 SpringBoard 启动通知调用，所有 UI 留在主线程。
void LHStartSafeModeUI(NSString *markerPath, int persistenceError) {
    dispatch_async(dispatch_get_main_queue(), ^{
        static LHSafeModeController *controller;
        if (controller) return;
        controller = [LHSafeModeController new];
        controller.markerPath = markerPath;
        controller.persistenceError = persistenceError;
        [controller start];
    });
}
