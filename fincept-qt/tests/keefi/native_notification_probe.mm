#import <AppKit/AppKit.h>

// NSWorkspace's notification center is process-local. Do not post a fake
// distributed screen-lock notification: other running apps could react to it.
void keefiPostNativeSessionInactiveForTest() {
    [[[NSWorkspace sharedWorkspace] notificationCenter]
        postNotificationName:NSWorkspaceSessionDidResignActiveNotification object:nil];
}

void keefiPostNativeSleepForTest() {
    [[[NSWorkspace sharedWorkspace] notificationCenter]
        postNotificationName:NSWorkspaceWillSleepNotification object:nil];
}
