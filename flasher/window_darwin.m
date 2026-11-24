//go:build darwin && cgo

// The Mac window for window_darwin.go: NSWindow + WKWebView, an app menu (Quit) and an Edit menu (so Cmd+C/V/X/A
// work in the Wi-Fi fields), alerts from the page, and no closing while a card is being written.

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

extern int flasherBusy(void);
extern void flasherWillQuit(void);

@interface FlasherDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, WKUIDelegate, WKNavigationDelegate>
@property(strong) NSWindow *window;
@property(strong) WKWebView *web;
@property(copy) NSString *url;
@end

static BOOL busyAlert(NSWindow *window) {
    if (!flasherBusy()) return NO;
    NSAlert *alert = [NSAlert new];
    alert.messageText = @"A card is being written";
    alert.informativeText = @"RG DS Flasher closes once it's done. Leave the card in until then.";
    [alert addButtonWithTitle:@"OK"];
    [alert beginSheetModalForWindow:window completionHandler:nil];
    return YES;
}

@implementation FlasherDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)note {
    NSRect frame = NSMakeRect(0, 0, 560, 800);
    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                              NSWindowStyleMaskResizable;
    self.window = [[NSWindow alloc] initWithContentRect:frame styleMask:style backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"RG DS Flasher";
    self.window.minSize = NSMakeSize(420, 560);
    self.window.delegate = self;
    self.window.releasedWhenClosed = NO;
    WKWebViewConfiguration *config = [WKWebViewConfiguration new];
    config.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore]; // nothing kept on disk
    self.web = [[WKWebView alloc] initWithFrame:frame configuration:config];
    self.web.UIDelegate = self;
    self.web.navigationDelegate = self;
    self.web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    @try {
        [self.web setValue:@NO forKey:@"drawsBackground"]; // no white flash before the page's own background
    } @catch (NSException *e) {
    }
    self.window.contentView = self.web;
    [self.web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:self.url]]];
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return YES; }
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
    return busyAlert(self.window) ? NSTerminateCancel : NSTerminateNow;
}
- (void)applicationWillTerminate:(NSNotification *)note { flasherWillQuit(); }
- (BOOL)windowShouldClose:(NSWindow *)window { return !busyAlert(window); }
// alert() from the page
- (void)webView:(WKWebView *)web runJavaScriptAlertPanelWithMessage:(NSString *)message
    initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(void))done {
    NSAlert *alert = [NSAlert new];
    alert.messageText = message;
    [alert addButtonWithTitle:@"OK"];
    [alert beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) { done(); }];
}
// window.close() from the page (Close, or the flasher went away): the app ends
- (void)webViewDidClose:(WKWebView *)web { [NSApp terminate:nil]; }
// links that leave the flasher's own page open in the default browser
- (void)webView:(WKWebView *)web decidePolicyForNavigationAction:(WKNavigationAction *)action
    decisionHandler:(void (^)(WKNavigationActionPolicy))decide {
    NSURL *to = action.request.URL;
    NSString *host = to.host ?: @"";
    if (action.navigationType == WKNavigationTypeLinkActivated && ![host isEqualToString:@"127.0.0.1"]) {
        [[NSWorkspace sharedWorkspace] openURL:to];
        decide(WKNavigationActionPolicyCancel);
        return;
    }
    decide(WKNavigationActionPolicyAllow);
}
- (WKWebView *)webView:(WKWebView *)web createWebViewWithConfiguration:(WKWebViewConfiguration *)config
    forNavigationAction:(WKNavigationAction *)action windowFeatures:(WKWindowFeatures *)features {
    if (action.request.URL) [[NSWorkspace sharedWorkspace] openURL:action.request.URL];
    return nil;
}
@end

static void addItem(NSMenu *menu, NSString *title, SEL action, NSString *key, NSEventModifierFlags mods) {
    NSMenuItem *item = [menu addItemWithTitle:title action:action keyEquivalent:key];
    item.keyEquivalentModifierMask = mods;
}

static NSMenu *mainMenu(void) {
    NSMenu *bar = [NSMenu new];
    NSMenu *app = [[NSMenu alloc] initWithTitle:@"RG DS Flasher"];
    addItem(app, @"Hide RG DS Flasher", @selector(hide:), @"h", NSEventModifierFlagCommand);
    [app addItem:[NSMenuItem separatorItem]];
    addItem(app, @"Quit RG DS Flasher", @selector(terminate:), @"q", NSEventModifierFlagCommand);
    NSMenu *edit = [[NSMenu alloc] initWithTitle:@"Edit"];
    addItem(edit, @"Undo", @selector(undo:), @"z", NSEventModifierFlagCommand);
    addItem(edit, @"Redo", @selector(redo:), @"z", NSEventModifierFlagCommand | NSEventModifierFlagShift);
    [edit addItem:[NSMenuItem separatorItem]];
    addItem(edit, @"Cut", @selector(cut:), @"x", NSEventModifierFlagCommand);
    addItem(edit, @"Copy", @selector(copy:), @"c", NSEventModifierFlagCommand);
    addItem(edit, @"Paste", @selector(paste:), @"v", NSEventModifierFlagCommand);
    addItem(edit, @"Select All", @selector(selectAll:), @"a", NSEventModifierFlagCommand);
    NSMenu *window = [[NSMenu alloc] initWithTitle:@"Window"];
    addItem(window, @"Minimize", @selector(performMiniaturize:), @"m", NSEventModifierFlagCommand);
    addItem(window, @"Close", @selector(performClose:), @"w", NSEventModifierFlagCommand);
    for (NSMenu *m in @[ app, edit, window ]) {
        NSMenuItem *holder = [bar addItemWithTitle:m.title action:nil keyEquivalent:@""];
        holder.submenu = m;
    }
    NSApp.windowsMenu = window;
    return bar;
}

static FlasherDelegate *delegate;

void flasherRun(const char *url) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSApp.activationPolicy = NSApplicationActivationPolicyRegular;
        NSApp.mainMenu = mainMenu();
        delegate = [FlasherDelegate new];
        delegate.url = [NSString stringWithUTF8String:url];
        NSApp.delegate = delegate;
        [NSApp run];
    }
}

// From any thread: the program is ending (Close, or the page went away while idle).
void flasherTerminate(void) {
    dispatch_async(dispatch_get_main_queue(), ^{ exit(0); });
}
