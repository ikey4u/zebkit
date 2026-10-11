/*
 * pageprobe for macOS: load one URL in a WKWebView that is never shown, and
 * print what the page says about itself.
 *
 *   pageprobe [--settle SECONDS] [--timeout SECONDS] [--snapshot FILE.png]
 *             [--eval SCRIPT] [--ignore-tls-errors] URL
 *
 * It prints the framework the view came from, then the string result of
 * SCRIPT (by default the page's title), evaluated SECONDS after the load
 * finished. --snapshot saves a picture of the view. Exit status: 0 loaded,
 * 1 failed to load, 2 timed out, 3 the web process crashed.
 *
 * scripts/smoke.sh uses it to check an engine build. It is a small relative
 * of pageprobe.c, which does the same and more for the Linux engine.
 *
 * Build against a built profile:
 *
 *   clang -fobjc-arc pageprobe.m -o pageprobe -F out/<profile>/Release \
 *       -framework WebKit -framework Cocoa
 */

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

@interface Probe : NSObject <WKNavigationDelegate>
@property (copy) NSString *script;
@property (copy) NSString *snapshot;
@property double settle;
@property BOOL ignoreTLSErrors;
@end

@implementation Probe

- (void)webView:(WKWebView *)view didFinishNavigation:(WKNavigation *)navigation
{
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(self.settle * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [view evaluateJavaScript:self.script completionHandler:^(id result, NSError *error) {
            printf("page: %s\n", error ? error.description.UTF8String : [result description].UTF8String);
            if (!self.snapshot)
                exit(0);
            [view takeSnapshotWithConfiguration:nil completionHandler:^(NSImage *image, NSError *snapshotError) {
                NSBitmapImageRep *bitmap = image ? [[NSBitmapImageRep alloc] initWithData:image.TIFFRepresentation] : nil;
                if (bitmap && [[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:self.snapshot atomically:YES])
                    printf("snapshot: %ldx%ld\n", (long)bitmap.pixelsWide, (long)bitmap.pixelsHigh);
                else
                    printf("snapshot failed: %s\n", snapshotError.description.UTF8String ?: "could not write the file");
                exit(0);
            }];
        }];
    });
}

- (void)fail:(NSError *)error
{
    printf("load failed: %s\n", error.localizedDescription.UTF8String);
    exit(1);
}

- (void)webView:(WKWebView *)view didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error
{
    [self fail:error];
}

- (void)webView:(WKWebView *)view didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error
{
    [self fail:error];
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView *)view
{
    printf("the web process crashed\n");
    exit(3);
}

- (void)webView:(WKWebView *)view didReceiveAuthenticationChallenge:(NSURLAuthenticationChallenge *)challenge
    completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential *))completionHandler
{
    SecTrustRef trust = challenge.protectionSpace.serverTrust;
    if (self.ignoreTLSErrors && trust)
        completionHandler(NSURLSessionAuthChallengeUseCredential, [NSURLCredential credentialForTrust:trust]);
    else
        completionHandler(NSURLSessionAuthChallengePerformDefaultHandling, nil);
}

@end

int main(int argc, char **argv)
{
    @autoreleasepool {
        Probe *probe = [Probe new];
        probe.script = @"document.title";
        probe.settle = 1;
        double timeout = 30;
        NSString *url = nil;
        for (int i = 1; i < argc; i++) {
            NSString *argument = @(argv[i]);
            BOOL hasValue = i + 1 < argc;
            if ([argument isEqual:@"--settle"] && hasValue)
                probe.settle = atof(argv[++i]);
            else if ([argument isEqual:@"--timeout"] && hasValue)
                timeout = atof(argv[++i]);
            else if ([argument isEqual:@"--snapshot"] && hasValue)
                probe.snapshot = @(argv[++i]);
            else if ([argument isEqual:@"--eval"] && hasValue)
                probe.script = @(argv[++i]);
            else if ([argument isEqual:@"--ignore-tls-errors"])
                probe.ignoreTLSErrors = YES;
            else
                url = argument;
        }
        if (!url) {
            fprintf(stderr, "usage: pageprobe [--settle SECONDS] [--timeout SECONDS] [--snapshot FILE.png] [--eval SCRIPT] [--ignore-tls-errors] URL\n");
            return 64;
        }

        // An application without a Dock icon or a menu bar, and a window that
        // is never put on screen: the view needs one to lay out and paint.
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        NSRect frame = NSMakeRect(0, 0, 800, 600);
        WKWebView *view = [[WKWebView alloc] initWithFrame:frame configuration:[WKWebViewConfiguration new]];
        view.navigationDelegate = probe;
        NSWindow *window = [[NSWindow alloc] initWithContentRect:frame styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
        window.contentView = view;

        printf("framework: %s\n", [NSBundle bundleForClass:[WKWebView class]].bundlePath.UTF8String);
        [view loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:url]]];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeout * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            printf("timed out\n");
            exit(2);
        });
        [NSApp run];
    }
    return 0;
}
