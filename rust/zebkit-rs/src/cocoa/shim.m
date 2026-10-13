/*
 * The engine's API on macOS is Objective-C: a WKWebView in an NSWindow,
 * told of changes through delegates and key-value observing, in an
 * application whose loop AppKit runs. This file puts the part of it the
 * zebkit crate uses behind C functions, which is what Rust can declare.
 * The build script compiles it; src/lib.rs has the declarations, and says
 * what each function does.
 *
 * The window of a view is the engine's business, as it is on Linux
 * (zebkit-view.h): it is on the screen when the machine has a display and
 * nowhere otherwise.
 *
 * Everything here is for the main thread.
 */

#import <Cocoa/Cocoa.h>
#import <Zebkit/Zebkit.h>

enum { HandlerTitle, HandlerURI, HandlerLoad, HandlerClose };
enum { LoadStarted, LoadRedirected, LoadCommitted, LoadFinished };

typedef void (*ZebkitShimHandler)(int event, const char *text, void *data);
typedef void (*ZebkitShimFunction)(void *data);

/* A closure of the caller's: what to call, with what, and how to free it. */
@interface ZebkitShimClosure : NSObject
@property (nonatomic) int kind;
@property (nonatomic) ZebkitShimHandler handler;
@property (nonatomic) void *data;
@property (nonatomic) ZebkitShimFunction destroy;
@end

@implementation ZebkitShimClosure
- (void)dealloc
{
    if (_destroy)
        _destroy(_data);
}
@end

/* A view, its window, and who wants to hear about them. */
@interface ZebkitShimView : NSObject <WKNavigationDelegate, NSWindowDelegate>
@property (nonatomic, strong) WKWebView *webView;
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) NSMutableArray<ZebkitShimClosure *> *closures;
@property (nonatomic, copy) NSString *reportedTitle;
@property (nonatomic, copy) NSString *reportedURI;
@end

@implementation ZebkitShimView

- (void)tell:(int)kind event:(int)event text:(NSString *)text
{
    /* A handler may add another, or drop the view. */
    for (ZebkitShimClosure *closure in [self.closures copy]) {
        if (closure.kind == kind)
            closure.handler(event, text.UTF8String, closure.data);
    }
}

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary *)change context:(void *)context
{
    /* Told once for each change, as on Linux, however often AppKit says so. */
    if ([keyPath isEqualToString:@"title"]) {
        NSString *title = self.webView.title ?: @"";
        if (![title isEqualToString:self.reportedTitle ?: @""]) {
            self.reportedTitle = title;
            [self tell:HandlerTitle event:0 text:title];
        }
    } else {
        NSString *address = self.webView.URL.absoluteString ?: @"";
        if (![address isEqualToString:self.reportedURI ?: @""]) {
            self.reportedURI = address;
            [self tell:HandlerURI event:0 text:address];
        }
    }
}

- (void)webView:(WKWebView *)webView didStartProvisionalNavigation:(WKNavigation *)navigation
{
    [self tell:HandlerLoad event:LoadStarted text:nil];
}

- (void)webView:(WKWebView *)webView didReceiveServerRedirectForProvisionalNavigation:(WKNavigation *)navigation
{
    [self tell:HandlerLoad event:LoadRedirected text:nil];
}

- (void)webView:(WKWebView *)webView didCommitNavigation:(WKNavigation *)navigation
{
    [self tell:HandlerLoad event:LoadCommitted text:nil];
}

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation
{
    [self tell:HandlerLoad event:LoadFinished text:nil];
}

- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error
{
    [self tell:HandlerLoad event:LoadFinished text:nil];
}

- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error
{
    [self tell:HandlerLoad event:LoadFinished text:nil];
}

- (void)windowWillClose:(NSNotification *)notification
{
    [self tell:HandlerClose event:0 text:nil];
}

- (void)dealloc
{
    [_webView removeObserver:self forKeyPath:@"title"];
    [_webView removeObserver:self forKeyPath:@"URL"];
    _webView.navigationDelegate = nil;
    _window.delegate = nil;
    if (_webView)
        zebkit_view_destroy(_webView);
}

@end

static NSString *string(const char *text)
{
    return text ? @(text) : nil;
}

static char *copyOf(NSString *text)
{
    return text ? strdup(text.UTF8String) : NULL;
}

static ZebkitShimView *viewOf(void *view)
{
    return (__bridge ZebkitShimView *)view;
}

void *zebkit_shim_view_new(int headless)
{
    ZebkitShimView *view = [ZebkitShimView new];
    view.closures = [NSMutableArray new];
    view.webView = headless ? zebkit_view_new_headless(nil) : zebkit_view_new(nil);
    view.window = view.webView.window;
    view.webView.navigationDelegate = view;
    [view.webView addObserver:view forKeyPath:@"title" options:0 context:NULL];
    [view.webView addObserver:view forKeyPath:@"URL" options:0 context:NULL];
    view.window.delegate = view;
    return (__bridge_retained void *)view;
}

void *zebkit_shim_view_retain(void *view)
{
    return (__bridge_retained void *)viewOf(view);
}

void zebkit_shim_view_release(void *view)
{
    (void)(__bridge_transfer ZebkitShimView *)view;
}

void *zebkit_shim_view_get_web_view(void *view)
{
    return (__bridge void *)viewOf(view).webView;
}

void zebkit_shim_view_load_uri(void *view, const char *uri)
{
    NSURL *url = [NSURL URLWithString:string(uri)];
    if (url)
        [viewOf(view).webView loadRequest:[NSURLRequest requestWithURL:url]];
}

void zebkit_shim_view_load_html(void *view, const char *html, const char *baseURI)
{
    [viewOf(view).webView loadHTMLString:string(html) baseURL:baseURI ? [NSURL URLWithString:string(baseURI)] : nil];
}

char *zebkit_shim_view_copy_uri(void *view)
{
    return copyOf(viewOf(view).webView.URL.absoluteString);
}

char *zebkit_shim_view_copy_title(void *view)
{
    return copyOf(viewOf(view).webView.title);
}

int zebkit_shim_view_is_loading(void *view)
{
    return viewOf(view).webView.loading;
}

void zebkit_shim_view_reload(void *view)
{
    [viewOf(view).webView reload];
}

void zebkit_shim_view_stop_loading(void *view)
{
    [viewOf(view).webView stopLoading];
}

int zebkit_shim_view_can_go_back(void *view)
{
    return viewOf(view).webView.canGoBack;
}

void zebkit_shim_view_go_back(void *view)
{
    [viewOf(view).webView goBack];
}

int zebkit_shim_view_can_go_forward(void *view)
{
    return viewOf(view).webView.canGoForward;
}

void zebkit_shim_view_go_forward(void *view)
{
    [viewOf(view).webView goForward];
}

void zebkit_shim_view_add_handler(void *view, int kind, ZebkitShimHandler handler, void *data, ZebkitShimFunction destroy)
{
    ZebkitShimClosure *closure = [ZebkitShimClosure new];
    closure.kind = kind;
    closure.handler = handler;
    closure.data = data;
    closure.destroy = destroy;
    [viewOf(view).closures addObject:closure];
}

void zebkit_shim_window_resize(void *view, int width, int height)
{
    [viewOf(view).window setContentSize:NSMakeSize(width, height)];
}

void zebkit_shim_window_set_title(void *view, const char *title)
{
    viewOf(view).window.title = string(title) ?: @"";
}

static BOOL running;

void zebkit_shim_run(void)
{
    [NSApplication sharedApplication];
    running = YES;
    [NSApp run];
    running = NO;
}

void zebkit_shim_quit(void)
{
    if (!running)
        return;
    [NSApp stop:nil];
    /* The loop looks at the request only when an event comes by. */
    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0
        windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:YES];
}

void zebkit_shim_timeout(unsigned milliseconds, ZebkitShimFunction function, void *data, ZebkitShimFunction destroy)
{
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)milliseconds * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        function(data);
        if (destroy)
            destroy(data);
    });
}
