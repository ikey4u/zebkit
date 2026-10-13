/*
 * window: a small browser window, using zebkit. This is window.c for
 * macOS: the same program, with the engine's API there.
 *
 *   window [URL]
 *
 * A window with a web page in it and, across the top, a back button, a
 * forward button and a box to type an address into.
 *
 * What a host has to do is all here: create a view, give its window a
 * size and a title, load something, and run the application. The engine
 * chooses the display itself, as on Linux (<Zebkit/zebkit-view.h>): a window on the
 * screen if the machine has one, else no window at all (the page still
 * loads and can be driven over CDP).
 *
 * zebkit draws web pages and nothing else. An application for macOS has
 * AppKit and would draw its controls with that, in a window of its own
 * with a WKWebView in it; this one keeps the bar window.c has, so that the
 * two are the same program: a script the engine runs in every document, in
 * a world of its own where the page's scripts cannot reach it, and which
 * talks to this program through a message handler. toolbar_script in
 * window-bar.h is that script; this file is the host's side of the
 * conversation.
 *
 * Build against a packaged zebkit (see examples/README.md):
 *
 *   clang -fobjc-arc window.m -o window -F<package> -framework Zebkit -framework Cocoa
 */

#import "window-bar.h"
#import <Cocoa/Cocoa.h>
#import <Zebkit/Zebkit.h>

/* What was typed is an address if it looks like one; "example.com" means
 * "https://example.com". */
static NSString *addressFromInput(NSString *text)
{
    if ([text containsString:@"://"] || [text hasPrefix:@"about:"] || [text hasPrefix:@"data:"])
        return text;
    return [@"https://" stringByAppendingString:text];
}

@interface Host : NSObject <NSWindowDelegate, WKScriptMessageHandler>
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) WKWebView *view;
@property (nonatomic, strong) WKContentWorld *barWorld;
@end

@implementation Host

/* Tells the bar where the page is and which buttons can do something. */
- (void)updateToolbar
{
    NSString *address = self.view.URL.absoluteString ?: @"";
    NSString *escaped = [address stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.alphanumericCharacterSet];
    NSString *script = [NSString stringWithFormat:@"globalThis.zebkitBar && zebkitBar.update(decodeURIComponent('%@'), %s, %s)",
        escaped, self.view.canGoBack ? "true" : "false", self.view.canGoForward ? "true" : "false"];
    [self.view evaluateJavaScript:script inFrame:nil inContentWorld:self.barWorld completionHandler:nil];
}

- (void)userContentController:(WKUserContentController *)controller didReceiveScriptMessage:(WKScriptMessage *)message
{
    NSString *text = [message.body isKindOfClass:NSString.class] ? message.body : @"";
    if ([text isEqualToString:@"back"])
        [self.view goBack];
    else if ([text isEqualToString:@"forward"])
        [self.view goForward];
    else if ([text hasPrefix:@"go "])
        [self.view loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:addressFromInput([text substringFromIndex:3])]]];
    else if ([text isEqualToString:@"ready"])
        [self updateToolbar];
}

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary *)change context:(void *)context
{
    if ([keyPath isEqualToString:@"title"])
        self.window.title = self.view.title.length ? self.view.title : @"zebkit";
    else
        [self updateToolbar];
}

- (void)windowWillClose:(NSNotification *)notification
{
    [NSApp terminate:nil];
}

@end

int main(int argc, char **argv)
{
    @autoreleasepool {
        NSString *url = argc > 1 ? @(argv[1]) : @"https://example.com/";
        Host *host = [Host new];

        /* The bar: a script for every document, and the channel it reports on. */
        host.barWorld = [WKContentWorld worldWithName:@BAR_WORLD];
        WKWebViewConfiguration *configuration = [WKWebViewConfiguration new];
        WKUserContentController *content = configuration.userContentController;
        [content addUserScript:[[WKUserScript alloc] initWithSource:@(toolbar_script)
            injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES inContentWorld:host.barWorld]];
        [content addScriptMessageHandler:host contentWorld:host.barWorld name:@BAR_CHANNEL];

        /* The view is the page. */
        host.view = zebkit_view_new(configuration);
        for (NSString *key in @[@"URL", @"loading", @"canGoBack", @"canGoForward", @"title"])
            [host.view addObserver:host forKeyPath:key options:0 context:NULL];

        /* The window the view is shown in. On a machine with no display it
         * is never on any screen. */
        host.window = host.view.window;
        [host.window setContentSize:NSMakeSize(1024, 768)];
        host.window.title = @"zebkit";
        host.window.delegate = host;

        [host.view loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:url]]];
        [NSApp run];
    }
    return 0;
}
