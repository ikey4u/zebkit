/*
 * pageprobe for macOS: load one URL in a WKWebView that is never shown and
 * print what happened as a single JSON object on stdout. It is pageprobe.c
 * for the Mac engine: the same options, the same questions asked of the
 * page (probe-scripts.h) and the same report, so that scripts/compat.sh,
 * media.sh, smoke.sh and bench.sh use either.
 *
 *   pageprobe [--settle SECONDS] [--timeout SECONDS] [--snapshot FILE] [--eval SCRIPT]
 *             [--no-web-security] [--ignore-tls-errors] URL
 *
 * --snapshot writes the viewport as raw pixels: a text line "width height
 * stride" followed by the bytes, four per pixel, blue first.
 * --eval runs SCRIPT in the settled page instead of the standard
 * measurements and reports its string result as "page".
 * --no-web-security turns off the same-origin policy, for investigation.
 * --ignore-tls-errors accepts any certificate, for a test server's own.
 *
 * Build against a built profile:
 *
 *   clang -fobjc-arc pageprobe.m -o pageprobe -F out/<profile>/Release \
 *       -framework WebKit -framework Cocoa
 */

#import "probe-scripts.h"
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

@interface WKPreferences (Probe)
- (void)_setWebSecurityEnabled:(BOOL)enabled;
@end

@interface WKWebView (Probe)
- (void)_setWindowOcclusionDetectionEnabled:(BOOL)enabled;
@end

/* A page runs at full speed, and plays media, only in a window that is on
 * screen and key. One nobody sees can still say that it is. */
@interface HiddenWindow : NSWindow
@end

@implementation HiddenWindow
- (BOOL)isKeyWindow
{
    return YES;
}
- (BOOL)canBecomeKeyWindow
{
    return YES;
}
@end

static NSString *jsonString(NSString *value)
{
    if (!value)
        return @"null";
    NSData *data = [NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingFragmentsAllowed error:nil];
    return data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : @"null";
}

@interface Probe : NSObject <WKNavigationDelegate>
@property (strong) WKWebView *view;
@property (copy) NSString *url;
@property (copy) NSString *snapshotPath;
@property (copy) NSString *evalScript;
@property double settleSeconds;
@property BOOL ignoreTLSErrors;
@property NSDate *startTime;
@property NSDate *finishTime;
@property BOOL finished;
@property BOOL measuring;
@property (copy) NSString *failure;
@property (copy) NSString *crash;
@property (copy) NSString *page;
@property (copy) NSString *paint;
@end

@implementation Probe

- (void)report
{
    NSMutableString *out = [NSMutableString stringWithFormat:@"{\"requested\":%@", jsonString(self.url)];
    [out appendFormat:@",\"loadFinished\":%s", self.finished ? "true" : "false"];
    if (self.finished)
        [out appendFormat:@",\"loadMs\":%lld", (long long)([self.finishTime timeIntervalSinceDate:self.startTime] * 1000)];
    [out appendFormat:@",\"loadFailed\":%@,\"crashed\":%@", jsonString(self.failure), jsonString(self.crash)];
    [out appendFormat:@",\"paint\":%@,\"page\":%@}", self.paint ?: @"null", self.page ?: @"null"];
    puts(out.UTF8String);
    fflush(stdout);
    exit(0);
}

/* How much of the viewport differs from its most common colour, and how many
 * distinct colours a coarse sample sees. A blank or single-colour viewport is
 * the signature of a page that loaded but did not render. */
- (void)snapshotReady:(NSImage *)image
{
    CGImageRef picture = [image CGImageForProposedRect:NULL context:nil hints:nil];
    if (!picture) {
        [self report];
        return;
    }
    /* Drawn at the size of the view, whatever the screen's scale, with the
     * bytes in the order the Linux probe has them. */
    int width = VIEW_WIDTH, height = VIEW_HEIGHT;
    size_t stride = (size_t)width * 4;
    NSMutableData *pixels = [NSMutableData dataWithLength:stride * height];
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(pixels.mutableBytes, width, height, 8, stride, space,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), picture);
    CGContextRelease(context);
    CGColorSpaceRelease(space);

    NSCountedSet *colours = [NSCountedSet new];
    NSUInteger samples = 0, mostCommon = 0;
    const uint8_t *bytes = pixels.bytes;
    for (int y = 0; y < height; y += 4) {
        for (int x = 0; x < width; x += 4) {
            uint32_t pixel;
            memcpy(&pixel, bytes + (size_t)y * stride + (size_t)x * 4, 4);
            /* Drop the low bits so antialiasing noise does not count as colour. */
            NSNumber *key = @(pixel & 0x00f0f0f0);
            [colours addObject:key];
            mostCommon = MAX(mostCommon, [colours countForObject:key]);
            samples++;
        }
    }
    self.paint = [NSString stringWithFormat:@"{\"width\":%d,\"height\":%d,\"colours\":%lu,\"paintedFraction\":%.3f}",
        width, height, (unsigned long)colours.count, samples ? 1.0 - (double)mostCommon / samples : 0.0];

    if (self.snapshotPath) {
        FILE *file = fopen(self.snapshotPath.fileSystemRepresentation, "wb");
        if (file) {
            fprintf(file, "%d %d %zu\n", width, height, stride);
            fwrite(pixels.bytes, 1, pixels.length, file);
            fclose(file);
        }
    }
    [self report];
}

- (void)measure
{
    if (self.measuring)
        return;
    self.measuring = YES;
    if (self.crash) {
        [self report];
        return;
    }
    [self.view evaluateJavaScript:self.evalScript ?: @(measureScript) completionHandler:^(id result, NSError *error) {
        if ([result isKindOfClass:NSString.class]) {
            /* The measurements are JSON already; anything else is quoted. */
            self.page = self.evalScript ? jsonString(result) : result;
        }
        if (self.crash) {
            [self report];
            return;
        }
        WKSnapshotConfiguration *configuration = [WKSnapshotConfiguration new];
        configuration.afterScreenUpdates = NO;
        [self.view takeSnapshotWithConfiguration:configuration completionHandler:^(NSImage *image, NSError *snapshotError) {
            [self snapshotReady:image];
        }];
    }];
}

- (void)after:(double)seconds do:(void (^)(void))block
{
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)), dispatch_get_main_queue(), block);
}

- (void)webView:(WKWebView *)view didFinishNavigation:(WKNavigation *)navigation
{
    if (self.finished)
        return;
    self.finished = YES;
    self.finishTime = [NSDate date];
    /* Give scripts time to build the page after the load event. */
    [self after:self.settleSeconds do:^{
        [self measure];
    }];
}

- (void)failed:(NSError *)error
{
    /* A cancelled load is a redirect or a script navigating away, not a failure. */
    BOOL cancelled = ([error.domain isEqualToString:NSURLErrorDomain] && error.code == NSURLErrorCancelled)
        || ([error.domain isEqualToString:@"WebKitErrorDomain"] && error.code == 102);
    if (cancelled)
        return;
    if (!self.failure)
        self.failure = [NSString stringWithFormat:@"%@ (%@)", error.localizedDescription, error.userInfo[NSURLErrorFailingURLStringErrorKey] ?: self.url];
    /* The load is over, and nothing else will say so. */
    [self webView:self.view didFinishNavigation:nil];
}

- (void)webView:(WKWebView *)view didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error
{
    [self failed:error];
}

- (void)webView:(WKWebView *)view didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error
{
    [self failed:error];
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView *)view
{
    if (!self.crash)
        self.crash = @"crashed";
    if (!self.measuring) {
        self.measuring = YES;
        [self report];
    }
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
        probe.settleSeconds = 6;
        double timeoutSeconds = 60;
        BOOL noWebSecurity = NO;
        for (int i = 1; i < argc; i++) {
            NSString *argument = @(argv[i]);
            BOOL hasValue = i + 1 < argc;
            if ([argument isEqual:@"--settle"] && hasValue)
                probe.settleSeconds = atof(argv[++i]);
            else if ([argument isEqual:@"--timeout"] && hasValue)
                timeoutSeconds = atof(argv[++i]);
            else if ([argument isEqual:@"--snapshot"] && hasValue)
                probe.snapshotPath = @(argv[++i]);
            else if ([argument isEqual:@"--eval"] && hasValue)
                probe.evalScript = @(argv[++i]);
            else if ([argument isEqual:@"--no-web-security"])
                noWebSecurity = YES;
            else if ([argument isEqual:@"--ignore-tls-errors"])
                probe.ignoreTLSErrors = YES;
            else
                probe.url = argument;
        }
        if (!probe.url) {
            fprintf(stderr, "usage: %s [--settle SECONDS] [--timeout SECONDS] [--snapshot FILE] [--eval SCRIPT] [--no-web-security] [--ignore-tls-errors] URL\n", argv[0]);
            return 2;
        }

        /* An application without a Dock icon or a menu bar. */
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        /* Cookies and caches in memory: nothing is kept between runs or
         * written to the user's Library. */
        WKWebViewConfiguration *configuration = [WKWebViewConfiguration new];
        configuration.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
        /* Media plays without a click, as it does in the Linux engine's
         * test: the page is muted. */
        configuration.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;
        [configuration.userContentController addUserScript:[[WKUserScript alloc] initWithSource:@(errorHookScript)
            injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES]];
        if (noWebSecurity && [configuration.preferences respondsToSelector:@selector(_setWebSecurityEnabled:)])
            [configuration.preferences _setWebSecurityEnabled:NO];

        NSRect frame = NSMakeRect(0, 0, VIEW_WIDTH, VIEW_HEIGHT);
        probe.view = [[WKWebView alloc] initWithFrame:frame configuration:configuration];
        probe.view.navigationDelegate = probe;
        /* A window that is never on any screen: the view needs one to lay
         * out and paint, and what is covered or off screen is otherwise
         * not painted. */
        NSWindow *window = [[HiddenWindow alloc] initWithContentRect:NSOffsetRect(frame, -20000, -20000)
            styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
        if ([probe.view respondsToSelector:@selector(_setWindowOcclusionDetectionEnabled:)])
            [probe.view _setWindowOcclusionDetectionEnabled:NO];
        window.contentView = probe.view;
        [window orderBack:nil];

        probe.startTime = [NSDate date];
        /* Measure whatever is there; "loadFinished" stays false in the report. */
        [probe after:timeoutSeconds do:^{
            [probe measure];
        }];
        /* The page never answered the measurement: its main thread is stuck. */
        [probe after:timeoutSeconds + probe.settleSeconds + 20 do:^{
            if (!probe.failure)
                probe.failure = @"page stopped responding";
            [probe report];
        }];
        [probe.view loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:probe.url]]];
        [NSApp run];
    }
    return 0;
}
