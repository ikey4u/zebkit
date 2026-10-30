// The application's delegate, menu, window and run loop belong to this host.
#import <Cocoa/Cocoa.h>
#include "model.h"
#include <chrono>
#include <cstdio>

using HostClock = std::chrono::steady_clock;
static int host_argc;
static char** host_argv;

static unsigned Modifiers(NSEvent* event) {
  unsigned flags = 0;
  const auto keys = event.modifierFlags;
  if (keys & NSEventModifierFlagShift) flags |= ZEBKIT_MODIFIER_SHIFT;
  if (keys & NSEventModifierFlagControl) flags |= ZEBKIT_MODIFIER_CONTROL;
  if (keys & NSEventModifierFlagOption) flags |= ZEBKIT_MODIFIER_ALT;
  if (keys & NSEventModifierFlagCommand) flags |= ZEBKIT_MODIFIER_META;
  if (keys & NSEventModifierFlagCapsLock) flags |= ZEBKIT_MODIFIER_CAPS_LOCK;
  const auto buttons = NSEvent.pressedMouseButtons;
  if (buttons & 1) flags |= ZEBKIT_MODIFIER_LEFT_BUTTON;
  if (buttons & 2) flags |= ZEBKIT_MODIFIER_RIGHT_BUTTON;
  if (buttons & 4) flags |= ZEBKIT_MODIFIER_MIDDLE_BUTTON;
  return flags;
}

@interface HostPane : NSView <NSTextInputClient> {
 @public
  native_host::Model* model;
  BOOL document;
  NSRect caret;
 @private
  NSString* _marked;
  NSRange _selection;
  NSTrackingArea* _tracking;
  BOOL _collecting;
  std::vector<std::pair<std::string, std::string>> _commands;
  NSMutableArray<NSDictionary*>* _textActions;
}
@property(strong) NSCursor* pointer;
@end
@implementation HostPane
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return !document; }
- (BOOL)becomeFirstResponder { if (model->view) zebkit_view_set_focus(model->view, true); return YES; }
- (BOOL)resignFirstResponder { if (model->view) zebkit_view_set_focus(model->view, false); return YES; }
- (void)setFrameSize:(NSSize)size {
  [super setFrameSize:size];
  if (!document && model && model->view)
    zebkit_view_set_size(model->view, std::max(1, (int)size.width), std::max(1, (int)size.height), self.window.backingScaleFactor ?: 1);
}
- (void)viewDidChangeBackingProperties { [self setFrameSize:self.frame.size]; }
- (void)updateTrackingAreas {
  if (_tracking) [self removeTrackingArea:_tracking];
  _tracking = [[NSTrackingArea alloc] initWithRect:NSZeroRect options:NSTrackingMouseMoved |
    NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect owner:self userInfo:nil];
  [self addTrackingArea:_tracking]; [super updateTrackingAreas];
}
- (void)drawImage:(const native_host::Image&)image fit:(BOOL)fit {
  if (image.bgra.empty()) return;
  NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:image.width
    pixelsHigh:image.height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
    colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaFirst | NSBitmapFormatThirtyTwoBitLittleEndian
    bytesPerRow:image.width * 4 bitsPerPixel:32];
  std::memcpy(bitmap.bitmapData, image.bgra.data(), image.bgra.size());
  NSImage* picture = [[NSImage alloc] initWithSize:NSMakeSize(image.width, image.height)];
  [picture addRepresentation:bitmap];
  NSRect target = NSMakeRect(image.bounds.x, image.bounds.y, image.width / image.scale, image.height / image.scale);
  if (fit) {
    const double scale = std::max(0., std::min((self.bounds.size.width - 20) / image.width,
                                               (self.bounds.size.height - 20) / image.height));
    target = NSMakeRect((self.bounds.size.width - image.width * scale) / 2,
      (self.bounds.size.height - image.height * scale) / 2, image.width * scale, image.height * scale);
  }
  [picture drawInRect:target fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1
       respectFlipped:YES hints:nil];
}
- (void)resetCursorRects { [self addCursorRect:self.bounds cursor:self.pointer ?: NSCursor.arrowCursor]; }
- (void)drawRect:(NSRect)dirty {
  [(document ? NSColor.lightGrayColor : NSColor.whiteColor) setFill]; NSRectFill(dirty);
  if (!model) return;
  if (document) [self drawImage:model->document fit:YES];
  else { [self drawImage:model->web fit:NO]; if (model->popup_shown) [self drawImage:model->popup fit:NO]; }
}
- (void)mouse:(NSEvent*)event type:(zebkit_mouse_type)type button:(zebkit_mouse_button)button {
  if (document || !model->view) return;
  if (type == ZEBKIT_MOUSE_DOWN) [self.window makeFirstResponder:self];
  const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
  zebkit_view_send_mouse(model->view, type, point.x, point.y, button,
    type == ZEBKIT_MOUSE_DOWN || type == ZEBKIT_MOUSE_UP ? (int)event.clickCount : 0, Modifiers(event));
}
- (void)mouseDown:(NSEvent*)event { [self mouse:event type:ZEBKIT_MOUSE_DOWN button:ZEBKIT_BUTTON_LEFT]; }
- (void)mouseUp:(NSEvent*)event { [self mouse:event type:ZEBKIT_MOUSE_UP button:ZEBKIT_BUTTON_LEFT]; }
- (void)rightMouseDown:(NSEvent*)event { [self mouse:event type:ZEBKIT_MOUSE_DOWN button:ZEBKIT_BUTTON_RIGHT]; }
- (void)rightMouseUp:(NSEvent*)event { [self mouse:event type:ZEBKIT_MOUSE_UP button:ZEBKIT_BUTTON_RIGHT]; }
- (void)otherMouseDown:(NSEvent*)event { [self mouse:event type:ZEBKIT_MOUSE_DOWN button:ZEBKIT_BUTTON_MIDDLE]; }
- (void)otherMouseUp:(NSEvent*)event { [self mouse:event type:ZEBKIT_MOUSE_UP button:ZEBKIT_BUTTON_MIDDLE]; }
- (void)mouseMoved:(NSEvent*)event { [self mouse:event type:ZEBKIT_MOUSE_MOVE button:ZEBKIT_BUTTON_NONE]; }
- (void)mouseDragged:(NSEvent*)event { [self mouseMoved:event]; }
- (void)rightMouseDragged:(NSEvent*)event { [self mouseMoved:event]; }
- (void)otherMouseDragged:(NSEvent*)event { [self mouseMoved:event]; }
- (void)mouseExited:(NSEvent*)event { [self mouse:event type:ZEBKIT_MOUSE_LEAVE button:ZEBKIT_BUTTON_NONE]; }
- (void)scrollWheel:(NSEvent*)event {
  if (document) { if (event.scrollingDeltaY) model->Turn(event.scrollingDeltaY < 0 ? 1 : -1); return; }
  if (!model->view) return;
  NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
  const double factor = event.hasPreciseScrollingDeltas ? 1 : 40;
  zebkit_wheel_phase phase = ZEBKIT_WHEEL_PHASE_NONE;
  if (event.momentumPhase != NSEventPhaseNone) phase = ZEBKIT_WHEEL_PHASE_MOMENTUM;
  else if (event.phase & NSEventPhaseBegan) phase = ZEBKIT_WHEEL_PHASE_BEGAN;
  else if (event.phase & NSEventPhaseChanged) phase = ZEBKIT_WHEEL_PHASE_CHANGED;
  else if (event.phase & (NSEventPhaseEnded | NSEventPhaseCancelled)) phase = ZEBKIT_WHEEL_PHASE_ENDED;
  zebkit_view_send_wheel(model->view, point.x, point.y, -event.scrollingDeltaX * factor,
    -event.scrollingDeltaY * factor, event.hasPreciseScrollingDeltas, phase, Modifiers(event));
}
- (zebkit_key_event)key:(NSEvent*)event type:(zebkit_key_type)type {
  zebkit_key_event key{}; key.type = type; key.modifiers = Modifiers(event); key.native_key_code = event.keyCode;
  NSString* text = event.charactersIgnoringModifiers;
  if (text.length) {
    const unichar character = [text characterAtIndex:0];
    key.key_code = character >= 'a' && character <= 'z' ? character - 'a' + 'A' : character < 128 ? character : 0;
    switch (character) {
      case NSDeleteCharacter: key.key_code = 8; break; case NSCarriageReturnCharacter: key.key_code = 13; break;
      case NSLeftArrowFunctionKey: key.key_code = 37; break; case NSUpArrowFunctionKey: key.key_code = 38; break;
      case NSRightArrowFunctionKey: key.key_code = 39; break; case NSDownArrowFunctionKey: key.key_code = 40; break;
      case NSDeleteFunctionKey: key.key_code = 46; break; case NSHomeFunctionKey: key.key_code = 36; break;
      case NSEndFunctionKey: key.key_code = 35; break; case NSPageUpFunctionKey: key.key_code = 33; break;
      case NSPageDownFunctionKey: key.key_code = 34; break;
    }
  }
  if (event.isARepeat) key.modifiers |= ZEBKIT_MODIFIER_AUTO_REPEAT;
  return key;
}
- (void)keyDown:(NSEvent*)event {
  if (!model->view || document) return;
  _commands.clear(); _textActions = [NSMutableArray array]; _collecting = YES;
  [self interpretKeyEvents:@[event]];
  _collecting = NO;
  std::vector<zebkit_edit_command> commands;
  for (const auto& command : _commands) commands.push_back({command.first.c_str(), command.second.c_str()});
  const auto key = [self key:event type:ZEBKIT_KEY_DOWN];
  zebkit_view_send_key_with_commands(model->view, &key, commands.data(), commands.size());
  for (NSDictionary* action in _textActions) {
    NSString* text = action[@"text"];
    if ([action[@"commit"] boolValue]) zebkit_view_ime_commit(model->view, text.UTF8String);
    else if (text.length) zebkit_view_ime_set_composition(model->view, text.UTF8String,
      [action[@"start"] intValue], [action[@"end"] intValue]);
    else zebkit_view_ime_cancel(model->view);
  }
  _textActions = nil;
}
- (void)keyUp:(NSEvent*)event { if (model->view && !document) { const auto key = [self key:event type:ZEBKIT_KEY_UP]; zebkit_view_send_key(model->view, &key); } }
- (void)doCommandBySelector:(SEL)selector {
  NSString* name = NSStringFromSelector(selector);
  if ([name hasSuffix:@":"]) name = [name substringToIndex:name.length - 1];
  if (!name.length || !model->view) return;
  NSDictionary<NSString*, NSString*>* aliases = @{
    @"insertParagraphSeparator":@"InsertNewline", @"insertNewlineIgnoringFieldEditor":@"InsertNewline",
    @"insertTabIgnoringFieldEditor":@"InsertTab", @"pageDown":@"MovePageDown",
    @"pageDownAndModifySelection":@"MovePageDownAndModifySelection", @"pageUp":@"MovePageUp",
    @"pageUpAndModifySelection":@"MovePageUpAndModifySelection", @"showGuessPanel":@"ToggleSpellPanel"};
  name = aliases[name] ?: name;
  name = [[[name substringToIndex:1] uppercaseString] stringByAppendingString:[name substringFromIndex:1]];
  if (_collecting) _commands.emplace_back(name.UTF8String, "");
  else {
    const zebkit_key_event key{};
    const zebkit_edit_command command{name.UTF8String, nullptr};
    zebkit_view_send_key_with_commands(model->view, &key, &command, 1);
  }
}
- (void)insertText:(id)value replacementRange:(NSRange)range {
  (void)range;
  NSString* text = [value isKindOfClass:NSAttributedString.class] ? [value string] : value;
  _marked = nil;
  if (_collecting) [_textActions addObject:@{@"text":text, @"commit":@YES}];
  else if (model->view) zebkit_view_ime_commit(model->view, text.UTF8String);
}
- (void)setMarkedText:(id)value selectedRange:(NSRange)selection replacementRange:(NSRange)replacement {
  (void)replacement;
  _marked = [value isKindOfClass:NSAttributedString.class] ? [value string] : value; _selection = selection;
  if (_collecting) [_textActions addObject:@{@"text":_marked, @"commit":@NO,
    @"start":@(selection.location), @"end":@(selection.location + selection.length)}];
  else if (model->view) zebkit_view_ime_set_composition(model->view, _marked.UTF8String,
    (int)selection.location, (int)(selection.location + selection.length));
}
- (void)unmarkText {
  _marked = nil;
  if (_collecting) [_textActions addObject:@{@"text":@"", @"commit":@NO}];
  else if (model->view) zebkit_view_ime_cancel(model->view);
}
- (BOOL)hasMarkedText { return _marked.length > 0; }
- (NSRange)markedRange { return self.hasMarkedText ? NSMakeRange(0, _marked.length) : NSMakeRange(NSNotFound, 0); }
- (NSRange)selectedRange { return _selection; }
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText { return @[]; }
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actual {
  (void)range; if (actual) *actual = NSMakeRange(NSNotFound, 0); return nil;
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point { (void)point; return NSNotFound; }
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actual {
  if (actual) *actual = range;
  return [self.window convertRectToScreen:[self convertRect:caret toView:nil]];
}
@end

@interface HostController : NSWindowController <NSWindowDelegate> {
  native_host::Model _model;
  HostPane *_web, *_pdf;
  NSTimer* _timer;
  HostClock::time_point _deadline;
  BOOL _started;
  int _pumps;
  std::string _smoke;
}
- (void)start;
- (void)scheduleAt:(HostClock::time_point)due;
- (void)stopEngine;
@end
static void Wake(int64_t delay, void* data) {
  HostController* host = (__bridge HostController*)data;
  const auto due = HostClock::now() + std::chrono::milliseconds(delay);
  dispatch_async(dispatch_get_main_queue(), ^{ [host scheduleAt:due]; });
}
@implementation HostController
- (instancetype)init {
  NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1200, 740)
    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
    backing:NSBackingStoreBuffered defer:NO];
  if ((self = [super initWithWindow:window])) {
    _deadline = HostClock::time_point::max();
    window.title = @"zebkit — AppKit host window"; window.delegate = self;
    NSSplitView* split = [[NSSplitView alloc] initWithFrame:window.contentView.bounds];
    split.vertical = YES; split.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _web = [[HostPane alloc] initWithFrame:NSMakeRect(0, 0, 600, 740)]; _web->model = &_model;
    _pdf = [[HostPane alloc] initWithFrame:NSMakeRect(601, 0, 599, 740)]; _pdf->model = &_model; _pdf->document = YES;
    [split addArrangedSubview:_web]; [split addArrangedSubview:_pdf];
    [window.contentView addSubview:split];
    __weak HostController* weak = self;
    _model.redraw = [weak] { HostController* host = weak; if (host) { [host->_web setNeedsDisplay:YES]; [host->_pdf setNeedsDisplay:YES]; } };
  }
  return self;
}
- (void)start {
  std::string url, pdf;
  for (int i = 1; i < host_argc; ++i) {
    const std::string arg = host_argv[i];
    if (arg.rfind("--url=", 0) == 0) url = arg.substr(6);
    if (arg.rfind("--pdf=", 0) == 0) pdf = arg.substr(6);
    if (arg.rfind("--smoke-test=", 0) == 0) _smoke = arg.substr(13);
  }
  zebkit_set_wakeup(Wake, (__bridge void*)self);
  char* error = nullptr;
  if (zebkit_init_with_settings(host_argc, host_argv, "{\"headless\":true}", &error)) {
    std::fprintf(stderr, "%s\n", error ? error : "Engine startup failed"); zebkit_free(error); [NSApp stop:nil]; return;
  }
  _started = YES;
  try { _model.Open(url, pdf); }
  catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); [self stopEngine]; [NSApp stop:nil]; return; }
  zebkit_view_on_caret(_model.view, [](zebkit_view*, const zebkit_rect* caret, void* data) {
    HostPane* pane = (__bridge HostPane*)data;
    if (caret) pane->caret = NSMakeRect(caret->x, caret->y, caret->width, caret->height);
    [pane.inputContext invalidateCharacterCoordinates];
  }, (__bridge void*)_web);
  zebkit_view_on_cursor(_model.view, [](zebkit_view*, const char* name, const zebkit_cursor_image* image, void* data) {
    HostPane* pane = (__bridge HostPane*)data;
    const std::string cursor(name);
    NSCursor* pointer = NSCursor.arrowCursor;
    if (cursor == "pointer") pointer = NSCursor.pointingHandCursor;
    else if (cursor == "text") pointer = NSCursor.IBeamCursor;
    else if (cursor == "crosshair") pointer = NSCursor.crosshairCursor;
    else if (cursor == "grab") pointer = NSCursor.openHandCursor;
    else if (cursor == "grabbing") pointer = NSCursor.closedHandCursor;
    else if (cursor == "ew-resize") pointer = NSCursor.resizeLeftRightCursor;
    else if (cursor == "ns-resize") pointer = NSCursor.resizeUpDownCursor;
    else if (cursor == "custom" && image && image->pixels) {
      NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:image->width
        pixelsHigh:image->height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
        bitmapFormat:NSBitmapFormatAlphaFirst | NSBitmapFormatThirtyTwoBitLittleEndian bytesPerRow:image->width * 4 bitsPerPixel:32];
      std::memcpy(bitmap.bitmapData, image->pixels, (size_t)image->width * image->height * 4);
      const float scale = image->scale > 0 ? image->scale : 1;
      NSImage* picture = [[NSImage alloc] initWithSize:NSMakeSize(image->width / scale, image->height / scale)];
      [picture addRepresentation:bitmap];
      pointer = [[NSCursor alloc] initWithImage:picture hotSpot:NSMakePoint(image->hot_x / scale, image->hot_y / scale)];
    }
    pane.pointer = pointer; [pane.window invalidateCursorRectsForView:pane];
  }, (__bridge void*)_web);
  [self showWindow:nil]; [self.window center]; [self.window makeKeyAndOrderFront:nil];
  [self.window makeFirstResponder:_web]; [_web setFrameSize:_web.frame.size];
  [self scheduleAt:HostClock::now()];
  if (!_smoke.empty()) {
    __weak HostController* weak = self;
    [NSTimer scheduledTimerWithTimeInterval:1.5 repeats:YES block:^(NSTimer* timer) {
      HostController* host = weak;
      if (!host || !host->_started) { [timer invalidate]; return; }
      if (!host->_model.frames) return;
      NSView* view = host.window.contentView;
      NSBitmapImageRep* image = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
      [view cacheDisplayInRect:view.bounds toBitmapImageRep:image];
      NSData* png = [image representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
      [png writeToFile:[NSString stringWithUTF8String:host->_smoke.c_str()] atomically:YES];
      [timer invalidate]; [host stopEngine]; [NSApp stop:nil];
      [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
        modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:NO];
    }];
  }
}
- (void)scheduleAt:(HostClock::time_point)due {
  if (!_started || due >= _deadline) return;
  _deadline = due; [_timer invalidate];
  const auto delay = std::max(0., std::chrono::duration<double>(due - HostClock::now()).count());
  __weak HostController* weak = self;
  _timer = [NSTimer timerWithTimeInterval:delay repeats:NO block:^(NSTimer*) {
    HostController* host = weak;
    if (!host || !host->_started) return;
    host->_timer = nil; host->_deadline = HostClock::time_point::max(); ++host->_pumps; zebkit_pump();
  }];
  [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
}
- (void)stopEngine {
  if (!_started) return;
  _started = NO; [_timer invalidate]; _timer = nil;
  _model.Close(); const bool stopped = zebkit_shutdown();
  std::printf("AppKit host: %d frames, %d event-driven pumps, shutdown=%s\n", _model.frames, _pumps, stopped ? "ok" : "failed");
}
- (void)windowWillClose:(NSNotification*)notification { (void)notification; [self stopEngine]; [NSApp stop:nil]; }
@end

@interface HostDelegate : NSObject <NSApplicationDelegate>
@property(strong) HostController* host;
@end
@implementation HostDelegate
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
  (void)notification; self.host = [[HostController alloc] init]; [self.host start];
}
- (void)applicationWillTerminate:(NSNotification*)notification { (void)notification; [self.host stopEngine]; }
@end
int main(int argc, char** argv) {
  host_argc = argc; host_argv = argv;
  @autoreleasepool {
    [NSApplication sharedApplication];
    HostDelegate* delegate = [[HostDelegate alloc] init]; NSApp.delegate = delegate;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSMenu* menu = [[NSMenu alloc] init]; NSMenuItem* application = [[NSMenuItem alloc] init];
    [menu addItem:application]; NSMenu* appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
    application.submenu = appMenu; NSApp.mainMenu = menu;
    [NSApp run]; [delegate.host stopEngine];
  }
  return 0;
}
