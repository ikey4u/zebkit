/*
 * cdphost for macOS: the smallest application that embeds the engine and
 * can be driven over the Chrome DevTools Protocol. It opens one page and
 * serves CDP until it is killed. scripts/cdp.sh uses it as the browser
 * under test, and it shows what an embedding application has to do: create
 * a view and call zebkit_cdp_listen().
 *
 *   cdphost [--port N] [--address IP] [--private] [--headless] [--size WxH] [URL]
 *
 * --private serves no discovery pages: a client needs the address printed
 * on standard error, which only this process and whoever reads its output
 * know.
 * --headless shows nothing: no Dock icon, and a window that is off every
 * screen.
 *
 * Build against a built profile:
 *
 *   clang -fobjc-arc cdphost.m -o cdphost -I src/cdp -F out/<profile>/Release \
 *       -framework WebKit -framework Cocoa
 */

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <zebkit-cdp.h>

/* A page takes input and runs at full speed only in the key window. A
 * window nobody sees can still say that it is. */
@interface HeadlessWindow : NSWindow
@end

@implementation HeadlessWindow
- (BOOL)isKeyWindow
{
    return YES;
}
- (BOOL)canBecomeKeyWindow
{
    return YES;
}
@end

@interface WKWebView (ZebkitHeadless)
- (void)_setWindowOcclusionDetectionEnabled:(BOOL)enabled;
@end

int main(int argc, char **argv)
{
    @autoreleasepool {
        NSString *url = @"about:blank";
        const char *address = NULL;
        unsigned port = 9222;
        BOOL headless = NO;
        BOOL private = NO;
        int width = 1280, height = 720;

        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--port") && i + 1 < argc)
                port = (unsigned)strtoul(argv[++i], NULL, 10);
            else if (!strcmp(argv[i], "--address") && i + 1 < argc)
                address = argv[++i];
            else if (!strcmp(argv[i], "--headless"))
                headless = YES;
            else if (!strcmp(argv[i], "--private"))
                private = YES;
            else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
                if (sscanf(argv[++i], "%dx%d", &width, &height) != 2) {
                    fprintf(stderr, "cdphost: --size takes WIDTHxHEIGHT\n");
                    return 2;
                }
            } else if (argv[i][0] == '-') {
                fprintf(stderr, "usage: %s [--port N] [--address IP] [--private] [--headless] [--size WxH] [URL]\n", argv[0]);
                return 2;
            } else
                url = @(argv[i]);
        }

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:headless ? NSApplicationActivationPolicyAccessory : NSApplicationActivationPolicyRegular];

        NSRect frame = NSMakeRect(0, 0, width, height);
        /* Nothing is kept from one run to the next, and nothing is written
         * to the user's Library: cookies and caches are in memory. */
        WKWebViewConfiguration *configuration = [WKWebViewConfiguration new];
        configuration.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
        WKWebView *view = [[WKWebView alloc] initWithFrame:frame configuration:configuration];
        NSWindow *window;
        if (headless) {
            window = [[HeadlessWindow alloc] initWithContentRect:NSOffsetRect(frame, -20000, -20000) styleMask:NSWindowStyleMaskBorderless
                backing:NSBackingStoreBuffered defer:NO];
            /* What is covered or off screen is otherwise not painted. */
            if ([view respondsToSelector:@selector(_setWindowOcclusionDetectionEnabled:)])
                [view _setWindowOcclusionDetectionEnabled:NO];
            window.contentView = view;
            [window orderBack:nil];
        } else {
            window = [[NSWindow alloc] initWithContentRect:frame
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                backing:NSBackingStoreBuffered defer:NO];
            window.title = @"zebkit";
            window.contentView = view;
            [window center];
            [window makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
        }
        [window makeFirstResponder:view];

        if (private)
            zebkit_cdp_set_discovery_enabled(false);
        char *error = NULL;
        if (!zebkit_cdp_listen(address, port, &error)) {
            fprintf(stderr, "cdphost: %s\n", error);
            free(error);
            return 1;
        }

        [view loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:url]]];

        /* Killed politely, the application ends the way it would by itself. */
        for (NSNumber *number in @[ @(SIGINT), @(SIGTERM) ]) {
            signal(number.intValue, SIG_IGN);
            dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, number.intValue, 0, dispatch_get_main_queue());
            dispatch_source_set_event_handler(source, ^{
                exit(0);
            });
            dispatch_resume(source);
            CFBridgingRetain(source);
        }
        [NSApp run];
    }
    return 0;
}
