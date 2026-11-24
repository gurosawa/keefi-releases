#include "PlatformSecurity.h"

#import <AppKit/AppKit.h>
#import <IOKit/pwr_mgt/IOPMLib.h>

@interface KEEFISecurityObserver : NSObject {
@public
    keefi::PlatformSecurity* owner;
    IOPMAssertionID assertion;
}
- (void)screenLocked:(NSNotification*)notification;
- (void)sessionInactive:(NSNotification*)notification;
- (void)systemSleep:(NSNotification*)notification;
@end

@implementation KEEFISecurityObserver
- (void)screenLocked:(NSNotification*)notification {
    Q_UNUSED(notification);
    owner->requestLock(QStringLiteral("screen_locked"));
}
- (void)sessionInactive:(NSNotification*)notification {
    Q_UNUSED(notification);
    owner->requestLock(QStringLiteral("session_inactive"));
}
- (void)systemSleep:(NSNotification*)notification {
    Q_UNUSED(notification);
    owner->requestLock(QStringLiteral("system_sleep_or_resume"));
}
@end

namespace keefi {
void* installMacSecurityObservers(PlatformSecurity* owner) {
    auto* observer = [[KEEFISecurityObserver alloc] init];
    observer->owner = owner;
    observer->assertion = kIOPMNullAssertionID;
    [[NSDistributedNotificationCenter defaultCenter]
        addObserver:observer selector:@selector(screenLocked:) name:@"com.apple.screenIsLocked" object:nil
        suspensionBehavior:NSNotificationSuspensionBehaviorDeliverImmediately];
    auto* center = [[NSWorkspace sharedWorkspace] notificationCenter];
    [center addObserver:observer selector:@selector(sessionInactive:)
                  name:NSWorkspaceSessionDidResignActiveNotification object:nil];
    [center addObserver:observer selector:@selector(systemSleep:) name:NSWorkspaceWillSleepNotification object:nil];
    [center addObserver:observer selector:@selector(systemSleep:) name:NSWorkspaceDidWakeNotification object:nil];
    return observer;
}

bool setMacKeepAwake(void* native, bool enabled) {
    auto* observer = static_cast<KEEFISecurityObserver*>(native);
    if (!observer) return false;
    if (!enabled) {
        if (observer->assertion == kIOPMNullAssertionID) return true;
        const auto result = IOPMAssertionRelease(observer->assertion);
        if (result != kIOReturnSuccess) return false;
        observer->assertion = kIOPMNullAssertionID;
        return true;
    }
    if (observer->assertion != kIOPMNullAssertionID) return true;
    return IOPMAssertionCreateWithName(kIOPMAssertionTypeNoIdleSleep, kIOPMAssertionLevelOn,
                                      CFSTR("KEEFI user-enabled background operation"),
                                      &observer->assertion) == kIOReturnSuccess;
}

void removeMacSecurityObservers(void* native) {
    auto* observer = static_cast<KEEFISecurityObserver*>(native);
    if (!observer) return;
    setMacKeepAwake(native, false);
    [[NSDistributedNotificationCenter defaultCenter] removeObserver:observer];
    [[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:observer];
    [observer release];
}
} // namespace keefi
