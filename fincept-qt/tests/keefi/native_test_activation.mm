#import <AppKit/AppKit.h>

// A QTest executable started by a shell has no LaunchServices foreground
// handoff. Request actual macOS activation for its synthetic windows only;
// Qt's native activation events and the caller's waits remain authoritative.
void keefiActivateNativeTestApplication() {
    @autoreleasepool {
        [[NSApplication sharedApplication] activateIgnoringOtherApps:YES];
    }
}
