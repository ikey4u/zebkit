#!/usr/bin/env python3
"""Attribute an engine binary's code and data to subsystems by symbol name.

Reads the *unstripped* library of a profile with `nm`, classifies every sized
symbol, and writes docs/reports/<profile>-code.md. There is no debug info in our
builds, so attribution is by C++ namespace and class-name prefix rather than
by source directory. Template helpers that live in WTF:: or std:: are charged
to the first engine namespace mentioned in their arguments, since that is the
code that caused them to be instantiated.

    scripts/codesize.py <profile>
"""

import collections
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Symbol types from nm: code, read-only data, writable data.
KIND = {"t": "code", "w": "code", "r": "rodata", "d": "data", "b": None, "v": "data", "u": "rodata", "n": None}

OWNER = re.compile(r"\b(WebCore|WebKit|JSC|Inspector|PAL|IPC|API|Messages|bmalloc|Gigacage|WTF)::")
SKIA = re.compile(r"^(?:\(anonymous namespace\)::)?(Sk|Gr|sk_|skgpu|sktext|skia|SkSL|skcms|skvx|skif|sknonstd|SKSL)|\b(Sk[A-Z]\w+|Gr[A-Z]\w+|skgpu|SkSL|sktext|skif)::")
C_PREFIXES = [
    ("bmalloc / libpas", re.compile(r"^(pas_|bmalloc_|jit_heap_|iso_|tagged_bmalloc|hotbit_|minalign32_|inline_medium|medium_|inline_non_committable|utility_)")),
    ("WPE platform + GLib API", re.compile(r"^(wpe_|webkit_|jsc_|WPE|_?wpe|_webkit|_jsc|xdg_|_xdg)")),
    ("simdutf", re.compile(r"\bsimdutf::")),
    ("ICU glue", re.compile(r"\bicu(_\d+)?::|^u[a-z]+_\w+")),
]

# Second-level buckets inside WebCore, matched against the class name that
# follows "WebCore::". Order matters: first match wins.
WEBCORE = [
    ("JS bindings", r"JS[A-Z]|js[A-Z]|IDL|Converter<|convert|toJS|WebCoreBuiltin|WebCoreJS|.*BuiltinFunctions|.*Builtins|DOMPromise|JSDOM|ScriptController|DeferredPromise|SerializedScriptValue|Clone(Serializer|Deserializer)"),
    ("CSS + style", r"CSS|Style|style|Css|MQ::|Calculation|MediaQuer|Length|PropertyAllowlist|Font(Face|Cascade|Selection|Feature|Palette|Variant)|RuleSet|RuleFeature|Selector|ElementRuleCollector|PseudoElement|DocumentRuleSets|ComputedStyle|Blending|Interpolation|TransformOperation|TransformFunction|BasicShape|Color|GridTrack|GridPosition|ShapeValue|ContentData|FillLayer|NinePiece|RenderStyle|Animation|KeyframeEffect|Keyframe|WebAnimation|DocumentTimeline|ScrollTimeline|ViewTimeline|Acceler"),
    ("Layout + rendering", r"Render|Layout|InlineIterator|Inline|LegacyInline|LineLayout|Display::|Paint|Border|Grid|Flex|Table|LegacyRoot|RootInline|Line(Box|Width|Break)|TextBox|Text(Painter|Decoration|Run|Util|Auto)|Float|HitTest|Scroll|Sticky|Clip|Repaint|Overlap|LayerCompos|BackgroundPainter|CollapsedBorder|Autosiz|Caret|ImageQuality|Marquee|FrameView|LocalFrameView|Region|ContentVisib|BidiRun|BidiResolver"),
    ("SVG", r"SVG|Legacy(Render)?SVG|SMIL"),
    ("HTML + DOM", r"HTML|Document|Element|Node|Attr|Event|DOM|Range|Text(?!ure)|CharacterData|Shadow|Slot|Mutation|Custom|TreeScope|Container|Named|Qualified|Position|Visible|Selection|Form|Input|Validat|Label|Radio|Checkbox|Date|Time|Month|Week|Option|Select|Button|Popover|Dialog|Tree(Walker|Scope)|Collection|Live|ClassList|Dataset|Space|Parser|Tokenizer|Markup|Entity|XML|XPath|XSL|Pointer|Mouse|Keyboard|Wheel|Touch|Drag|Focus|UserGesture|Gesture|Clipboard|DataTransfer|Paste|Intersection|Resize|Performance|Navigation|History|Location|Window|Frame|Page|Chrome|Settings|Quirks|Viewport|Screen|Fullscreen|Visibility|Idle|Abort|Message|Broadcast|Custom|Observable|Subscriber|Trusted|Sanitiz|ViewTransition|Lazy"),
    ("Editing", r"Edit|Composite|Apply|Insert|Delete|Replace|Typing|Indent|Spell|Text(Check|Iterator|Manip|Insertion|Granular)|Alternative|Undo|Markers?|WordAwareIterator|SimplifiedBackwards|Break|ReplaceSelection|FrameSelection|Pasteboard|Dictation|WritingTools"),
    ("Loader + network + fetch", r"Loader|Load|Resource|Cache|Fetch|Network|Request|Response|Header|HTTP|URL|Cookie|Credential|Authentication|Certificate|Soup|CORS|CrossOrigin|ContentSecurity|Security|Mixed|Referrer|Origin|Site|Link|Preload|Speculation|Download|Blob|FormData|Stream|Readable|Writable|Transform|Substitute|Archive|MIME|Sniff|Data(URL|Detector)|WebSocket|ThreadableWebSocket|EventSource|XMLHttp|Beacon|Ping|Report|COOP|COEP|Permissions?Policy|Integrity|Sandbox|PrivateClick|Ad(Click)?"),
    ("Storage + IndexedDB + workers", r"IDB|Indexed|SQLite|Storage|Database|Worker|Worklet|ServiceWorker|SW[A-Z]|Shared|BackgroundFetch|Push|Notification|FileSystem|File|Lock|Permission|CacheStorage|Cookie[SC]|WebLock"),
    ("Graphics + fonts + images", r"Graphics|Font|Glyph|Image|Bitmap|Pixel|Texture|Coordinated|Composit|Nicosia|GL|Skia|Path|Gradient|Pattern|Filter|FE[A-Z]|Shadow|Affine|Transformation|Float(Rect|Point|Size|Quad|Rounded)|Int(Rect|Point|Size)|Rect|Rounded|Complex|Harf|WidthIterator|Text(Shap|Spac)|Shape|Canvas|Offscreen|Display(List|Refresh)|ANGLE|PlatformDisplay|Damage|Tile|Backing|Surface|Decoder|Encoder|PNG|JPEG|WebP|GIF|ICO|BMP|Cursor|Icon|Theme|Adwaita|Scrollbar|Platform(Layer|Screen|CA)|DrawGlyphs|DecomposedGlyphs|Destination|ColorSpace|DynamicRange|Opentype|OpenType|WOFF"),
    ("Crypto", r"Crypto|Subtle|GCrypt|JsonWeb|Rsa|Ec[A-Z]|Hmac|Aes|Pbkdf|Hkdf|SHA|X25519|Ed25519"),
    ("Accessibility", r"Accessib|AX[A-Z]|AccessibilityObject"),
    ("Media leftovers", r"Media|Audio|Video|Track|TextTrack|VTT|Cue|Capture|Playback|RTC|WebCodecs|MSE|Source(Buffer)?|Remote(Playback)?|Picture"),
    ("Inspector hooks", r"Inspector|Instrumenting|PageDebugger|WebConsoleAgent|Page(Console|Runtime|Heap|DOM|Network|Canvas|Timeline|Audit)|Worker(Inspector|Debugger)|CommandLineAPI|WebInjectedScript|InstrumentingAgents"),
    ("Intl + text + platform", r"Locale|Text(Encoding|Codec|Break|Boundar)|Unicode|Localized|Platform|Timer|Thread|RunLoop|Log|Memory|Process|Glib|GLib|G[A-Z]|Shared(Buffer|Memory|String)|Fragmented|Decimal|Language|LowPower|Battery|Geolocation|DeviceOrientation|Gamepad|UserAgent|System"),
]
WEBCORE = [(name, re.compile(r"(?:" + pat + r")")) for name, pat in WEBCORE]

JSC = [
    ("DFG JIT", r"DFG::"),
    ("FTL + B3 + Air", r"FTL::|B3::"),
    ("Baseline JIT + ICs + assembler", r"JIT|LOL::|PolymorphicAccess|InlineCache|StructureStubInfo|AccessCase|Repatch|MacroAssembler|X86|ARM64|AssemblyHelpers|CCallHelpers|LinkBuffer|Thunk|operation[A-Z]|CallLinkInfo|Disassembl|GPRInfo|RegisterSet|BinarySwitch|ExecutableAllocator|Probe::|Snippet|DOMJIT"),
    ("WebAssembly", r"Wasm|WebAssembly|JSToWasm|WasmToJS"),
    ("Yarr (regexp)", r"Yarr::|RegExp"),
    ("Intl", r"Intl|intl[A-Z]|Temporal|temporal|ISO8601|TemporalCore"),
    ("Parser + bytecode", r"Parser|Lexer|ASTBuilder|SyntaxChecker|BytecodeGenerator|[A-Z]\w*Node::|Unlinked|CodeBlock|Bytecode|Op[A-Z]\w+::|LLInt|Interpreter|CodeCache|SourceProvider|Builtin|ModuleAnalyzer|ModuleScope|VariableEnvironment|Scope|CachedTypes|Cached[A-Z]|Decoder|Encoder"),
    ("GC + heap", r"Heap|Marked|Slot|Subspace|IsoSubspace|GCClient|Collector|Visitor|Weak|Handle|Allocator|BlockDirectory|Sweep|Conservative|MachineThreads|Verifier|FreeList|PreciseAllocation|CompleteSubspace|AlignedMemory|IncrementalSweeper|Full?GC"),
    ("Debugger + inspector + profiler", r"Debugger|Inspector|Profiler|Sampling|ScriptCallStack|Console|TypeProfiler|ControlFlowProfiler|HeapSnapshot|Breakpoint|ScriptDebug|TypeSet|FunctionHasExecuted|BasicBlockLocation"),
    ("Typed arrays + atomics", r"TypedArray|typedArray|ArrayBuffer|DataView|dataView|Atomics|atomics|Float(16|32|64)Array|Int(8|16|32)Array|Uint(8|16|32)|BigInt64|GenericTypedArray|JSArrayBufferView"),
]
JSC = [(name, re.compile(pat)) for name, pat in JSC]

WEBKIT = [
    ("IPC message glue", r"Messages::|IPC::|ArgumentCoder|handleMessage|didReceive(Sync)?Message|Coder<|MessageReceiver|MessageSender|StreamConnection|Connection::"),
    ("Network process", r"Network|NetworkCache|WebSocketTask|PrivateClickMeasurement|PCM::|WebResourceLoadStatistics|ResourceLoadStatistics|ITP|Cookie|WebSWServer|WebSharedWorkerServer|ServiceWorkerFetch|Download|CacheStorage|WebStorage|NetworkStorage|LocalStorage|SessionStorage|IDBStorage|OriginStorage|BackgroundFetch|WebPush|Storage"),
    ("UI process + GLib API", r"WebPageProxy|WebProcessProxy|WebProcessPool|Proxy|UIProcess|API::|WebKit(?:WebView|Settings|WebContext|Download|Cookie|Website|UserContent|Navigation|Policy|URI|Network|Hit|Find|Back|Input|Option|Web|Script|Authentication|Geolocation|Notification|Permission|Security|Favicon|Feature|Memory|Color|Rectangle|File|Form|Print|Editor|Context|Automation|Application|Clipboard|Media|Device|User|ITP|Credential|Certificate|TLS|Error|Plug|Mime|Custom|Window)|webkit[A-Z_]|WebPreferences|WebBackForward|WebInspector|WebAutomation|ViewGestureController|DrawingAreaProxy|WebsiteData|WebFrameProxy|Provisional|SuspendedPage|ProcessLauncher|AuxiliaryProcessProxy|WebContextMenu|WebOpenPanel|WebPopup|WebFullScreen|PageClient|VisitedLink|WebUserContent|WebScriptMessage|WebURLScheme|ProcessThrottler|ProcessAssertion|BackgroundProcessResponsiveness|ResponsivenessTimer|Acceler|WPE"),
    ("Web process", r"WebPage|WebFrame|WebProcess|WebLoaderStrategy|WebResourceLoader|InjectedBundle|WebChromeClient|WebLocalFrameLoaderClient|WebFrameLoaderClient|WebEditorClient|DrawingArea|LayerTree|WebDocumentLoader|WebFoundTextRange|FindController|WebBackForwardListProxy|EventDispatcher|ViewUpdateDispatcher|Remote(Layer|Scrolling|Render|Graphics|Image|Display|Resource|Web|Media|Audio|Video|GPU|Legacy|Sample|CDM|Source|Text|Barcode|Face|Shape|WC|Compositor|Queue|Adapter|Device|Buffer|Bind|Pipeline|Sampler|Shader|Texture|Command|Compute|External|Present|Query|XR)|UserData|WebSW|WebSharedWorker|WebIDB|WebStorage|WebCookieJar|WebPlatformStrategies|WebBroadcastChannel|WebMessagePort|WebNotification|WebPermission|WebGeolocation|WebInspector|WebAutomationSession|Coordinated|ThreadedCompositor|AcceleratedSurface|NonCompositedFrameRenderer|WebContextMenu|WebPopupMenu|WebColorChooser|WebDataListSuggestionPicker|WebDateTimeChooser|WebSearchPopupMenu|WebOpenPanelResultListener|WebDragClient|WebValidationMessageClient|WebURLSchemeHandlerProxy|WebFullScreenManager|WebPaymentCoordinator|WebProgressTrackerClient|WebAlternativeTextClient|WebSpeech|WebBadgeClient|WebFileSystemStorageConnection|WebLockRegistryProxy|WebMDNS|LibWebRTC|WebRTC|WebMockContentFilterManager|WebScreenOrientation|WebAttachment|WebCacheStorage|WebSocketChannel|WebTransport|StorageArea|StorageNamespace|VisitedLinkTable"),
]
WEBKIT = [(name, re.compile(pat)) for name, pat in WEBKIT]


def classify(name):
    """Return (group, subgroup) for a demangled symbol name."""
    skia, owner = SKIA.search(name), OWNER.search(name)
    if skia and (not owner or skia.start() < owner.start()):
        return "Skia", ""
    for label, pattern in C_PREFIXES:
        if pattern.search(name) and not OWNER.search(name):
            return label, ""

    owners = [m.group(1) for m in OWNER.finditer(name)]
    if not owners:
        if name.startswith(("std::", "void std::", "__", "operator")) or "std::" in name:
            return "C++ runtime + std templates", ""
        return "Unattributed (C symbols, thunks)", ""

    # Charge WTF/IPC/API helpers to the engine namespace that instantiated them.
    primary = next((o for o in owners if o in ("WebCore", "WebKit", "JSC", "Inspector", "PAL")), owners[0])
    if primary in ("IPC", "Messages", "API"):
        primary = "WebKit"
    if primary in ("bmalloc", "Gigacage"):
        return "bmalloc / libpas", ""
    if primary == "PAL":
        return "WebCore", "Intl + text + platform"
    if primary == "Inspector":
        return "JavaScriptCore", "Debugger + inspector + profiler"
    if primary == "WTF":
        return "WTF", ""

    if primary in ("WebCore", "JSC"):
        tail = name[name.index(primary + "::") + len(primary) + 2:]
    if primary == "WebCore":
        for label, pattern in WEBCORE:
            if pattern.match(tail):
                return "WebCore", label
        return "WebCore", "Other"
    if primary == "JSC":
        for label, pattern in JSC:
            if pattern.search(tail[:160]):
                return "JavaScriptCore", label
        return "JavaScriptCore", "Runtime + everything else"
    for label, pattern in WEBKIT:
        if pattern.search(name[:400]):
            return "WebKit", label
    return "WebKit", "Other"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip().splitlines()[-1].strip())
    profile = sys.argv[1]
    libs = sorted((ROOT / "out" / profile / "lib").glob("libzebkit.so.*.*.*"))
    if not libs:
        sys.exit(f"error: profile '{profile}' is not built")
    lib = libs[0]

    out = subprocess.run(["nm", "-S", "--size-sort", "-C", str(lib)], check=True, capture_output=True, text=True).stdout
    groups = collections.defaultdict(lambda: collections.Counter())
    subgroups = collections.defaultdict(lambda: collections.Counter())
    biggest = []
    for line in out.splitlines():
        parts = line.split(" ", 3)
        if len(parts) < 4:
            continue
        kind = KIND.get(parts[2].lower())
        if kind is None:
            continue
        size = int(parts[1], 16)
        group, sub = classify(parts[3])
        groups[group][kind] += size
        if sub:
            subgroups[group][sub] += size
        if kind == "code":
            biggest.append((size, parts[3]))

    mib = lambda n: f"{n / 1048576:.1f}"
    total = collections.Counter()
    for counter in groups.values():
        total.update(counter)
    everything = sum(total.values())

    lines = [
        f"# Code size by subsystem: `{profile}`",
        "",
        f"- Binary: `{lib.name}` (unstripped), symbols classified by `scripts/codesize.py`",
        f"- Sized symbols: {mib(total['code'])} MiB code, {mib(total['rodata'])} MiB read-only data, {mib(total['data'])} MiB data",
        "- Attribution is by namespace and class-name prefix, so sub-buckets are approximate.",
        "- Functions the linker folded together are counted once per name, so totals can exceed the section sizes.",
        "",
        "## By component",
        "",
        "| Component | Code | Read-only data | Data | Total | Share |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for group, counter in sorted(groups.items(), key=lambda kv: -sum(kv[1].values())):
        size = sum(counter.values())
        lines.append(f"| {group} | {mib(counter['code'])} | {mib(counter['rodata'])} | {mib(counter['data'])} | {mib(size)} | {100 * size / everything:.0f}% |")

    for group in ("WebCore", "JavaScriptCore", "WebKit"):
        if group not in subgroups:
            continue
        lines += ["", f"## Inside {group}", "", "| Area | MiB |", "|---|---:|"]
        for sub, size in subgroups[group].most_common():
            lines.append(f"| {sub} | {mib(size)} |")

    lines += ["", "## Largest 25 functions", "", "| KiB | Symbol |", "|---:|---|"]
    for size, name in sorted(biggest, reverse=True)[:25]:
        shown = name if len(name) <= 110 else name[:107] + "..."
        lines.append(f"| {size / 1024:.0f} | `{shown.replace('|', '/')}` |")

    report = ROOT / "docs" / "reports" / f"{profile}-code.md"
    report.write_text("\n".join(lines) + "\n")
    print(f"wrote {report.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
