import Foundation
import WebKit
import Zebkit

/// zebkit's viewer on iOS (docs/VIEWER.md): a `WKWebView` whose pages have the
/// global `zebkit` they have in zebkit's own views on Linux, macOS and
/// Windows: `zebkit.app.<name>()` for the functions bound here, `zebkit.pdf`,
/// `zebkit.compress`, `crypto`, `image`, `url.parse`, `fs`, `fetch`.
/// libzebkit answers the calls (PDFium and the rest, without the engine);
/// WebKit draws the pages, as the App Store asks (guideline 2.5.6).
///
/// ```swift
/// let viewer = ZebkitViewer()
/// viewer.grant(origin: "app://reader", allow: [.app, .pdf])
/// viewer.bind("save") { args in try await save(args) }     // zebkit.app.save(...)
/// viewer.webView.load(URLRequest(url: URL(string: "app://reader/index.html")!))
/// ```
///
/// The configuration given to `init` may carry the application's own scheme
/// handlers and scripts; the viewer adds its script and message handler.
@MainActor
public final class ZebkitViewer: NSObject {
    /// What a page may use (zebkit_allow in `<zebkit/zebkit.h>`).
    public struct Allow: OptionSet, Sendable {
        public let rawValue: UInt32
        public init(rawValue: UInt32) { self.rawValue = rawValue }
        public static let app = Allow(rawValue: 1 << 0)
        public static let fetch = Allow(rawValue: 1 << 1)
        public static let files = Allow(rawValue: 1 << 2)
        public static let compress = Allow(rawValue: 1 << 3)
        public static let crypto = Allow(rawValue: 1 << 4)
        public static let image = Allow(rawValue: 1 << 5)
        public static let url = Allow(rawValue: 1 << 6)
        public static let pdf = Allow(rawValue: 1 << 7)
        public static let all = Allow(rawValue: 0xff)
    }

    /// A function a page calls as `zebkit.app.<name>(...args)`: its arguments as
    /// the page gave them (JSON, bytes as {"$bytes": base64}); returns what the
    /// page's promise resolves to, as a JSON-able value (`nil` for undefined).
    /// A thrown error rejects it with its description.
    public typealias AppFunction = @MainActor ([Any]) async throws -> Any?

    public let webView: WKWebView

    /// The application's navigation delegate, which gets every message the
    /// viewer does not need for itself (it uses `didCommit`, then passes it on).
    public weak var navigationDelegate: WKNavigationDelegate?

    private let viewer: OpaquePointer
    private var functions: [String: AppFunction] = [:]
    private static let handlerName = "zebkit"

    /// Loads ICU's data (URLs' international names, text's line breaks) from
    /// the Zebkit framework's resources, once. Optional: zebkit_icu_init()
    /// looks in the application's main bundle otherwise.
    @discardableResult
    public static func initICU() -> Bool {
        let bundles = [Bundle.main] + Bundle.allFrameworks
        for bundle in bundles {
            if let path = bundle.path(forResource: "icudtl", ofType: "dat") {
                return zebkit_icu_init_from_file(path)
            }
        }
        return zebkit_icu_init()
    }

    /// A viewer with a web view of its own, made with `configuration` (which
    /// may carry the application's scheme handlers and scripts).
    public convenience init(configuration: WKWebViewConfiguration = WKWebViewConfiguration()) {
        self.init(webView: WKWebView(frame: .zero, configuration: configuration))
    }

    /// A viewer for `webView`, the application's (a subclass of its own, say):
    /// its script and message handler go into the web view's content
    /// controller, for the documents it loads from now on, and the web view's
    /// navigation delegate becomes `navigationDelegate`, which the viewer
    /// passes every navigation message.
    public init(webView: WKWebView) {
        guard let made = zebkit_viewer_new() else { fatalError("zebkit_viewer_new failed") }
        viewer = made
        self.webView = webView
        super.init()
        let post = "(m) => window.webkit.messageHandlers.\(Self.handlerName).postMessage(m)"
        let source = Self.take(zebkit_viewer_script(post)) ?? ""
        let controller = webView.configuration.userContentController
        controller.addUserScript(WKUserScript(source: source, injectionTime: .atDocumentStart, forMainFrameOnly: false))
        controller.add(MessageProxy(self), name: Self.handlerName)
        navigationDelegate = webView.navigationDelegate
        webView.navigationDelegate = self
    }

    deinit {
        zebkit_viewer_free(viewer)
    }

    /// Gives pages of `origin` ("app://reader", "https://app.example", or "*"
    /// for pages the application wrote itself) what `allow` says; `[]` takes it
    /// back. `filesRoot` is zebkit.fs's directory for `.files`.
    public func grant(origin: String, allow: Allow, filesRoot: URL? = nil) {
        zebkit_viewer_grant(viewer, origin, allow.rawValue, filesRoot?.path)
    }

    /// `zebkit.app.<name>`; `nil` removes it.
    public func bind(_ name: String, _ function: AppFunction?) {
        functions[name] = function
    }

    /// Stops answering: what pages opened is closed. Call before letting the
    /// web view go if it outlives the viewer.
    public func detach() {
        webView.configuration.userContentController.removeScriptMessageHandler(forName: Self.handlerName)
        zebkit_viewer_reset(viewer)
    }

    // MARK: The page's calls

    fileprivate func receive(_ message: WKScriptMessage) {
        guard let text = message.body as? String else { return }
        let origin = Self.origin(of: message.frameInfo.securityOrigin)
        let frame = message.frameInfo
        let viewer = UnsafeSendable(viewer)
        Task.detached(priority: .userInitiated) {
            // libzebkit's work (a PDF's page, a decompression) off the main thread.
            var appCall: UnsafeMutablePointer<CChar>?
            let script = Self.take(zebkit_viewer_handle(viewer.value, origin, text, &appCall))
            let call = Self.take(appCall)
            await MainActor.run {
                if let script {
                    self.run(script, in: frame)
                } else if let call {
                    self.answerApplication(call, in: frame)
                }
            }
        }
    }

    private func answerApplication(_ text: String, in frame: WKFrameInfo) {
        guard let data = text.data(using: .utf8),
              let call = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              let id = (call["id"] as? NSNumber)?.int64Value,
              let module = call["module"] as? String,
              let method = call["method"] as? String
        else { return }
        let args = call["args"] as? [Any] ?? []
        Task { @MainActor in
            do {
                let result: Any?
                if module == "fetch" {
                    result = try await Self.fetch(args)
                } else if let function = functions[method] {
                    result = try await function(args)
                } else {
                    throw ViewerError("zebkit.app has no \(method)")
                }
                self.run(Self.answer(id, ok: true, result: try Self.json(result)), in: frame)
            } catch {
                self.run(Self.answer(id, ok: false, result: "\(error)"), in: frame)
            }
        }
    }

    private func run(_ script: String, in frame: WKFrameInfo) {
        webView.evaluateJavaScript(script, in: frame, in: .page) { _ in }
    }

    // MARK: zebkit.fetch, over URLSession

    /// {status, url, headers, body}, body as bytes, as zebkit's views give it.
    private static func fetch(_ args: [Any]) async throws -> Any? {
        guard let text = args.first as? String, let url = URL(string: text) else {
            throw ViewerError("not a valid URL")
        }
        let options = args.count > 1 ? args[1] as? [String: Any] ?? [:] : [:]
        var request = URLRequest(url: url)
        request.httpMethod = options["method"] as? String ?? "GET"
        for (name, value) in options["headers"] as? [String: String] ?? [:] {
            request.setValue(value, forHTTPHeaderField: name)
        }
        if let body = options["body"] {
            if let bytes = (body as? [String: Any])?["$bytes"] as? String {
                request.httpBody = Data(base64Encoded: bytes)
            } else if let string = body as? String {
                request.httpBody = Data(string.utf8)
            }
        }
        let (data, response) = try await URLSession.shared.data(for: request)
        let http = response as? HTTPURLResponse
        let headers = (http?.allHeaderFields ?? [:]).map { "\($0.key): \($0.value)\r\n" }.joined()
        return [
            "status": http?.statusCode ?? 200,
            "url": response.url?.absoluteString ?? text,
            "headers": headers,
            "body": ["$bytes": data.base64EncodedString()],
        ] as [String: Any]
    }

    // MARK: Helpers

    private static func answer(_ id: Int64, ok: Bool, result: String?) -> String {
        take(zebkit_viewer_answer(id, ok, result)) ?? ""
    }

    /// A result as JSON text; nil for undefined.
    private static func json(_ value: Any?) throws -> String? {
        guard let value else { return nil }
        if value is NSNull { return "null" }
        let data = try JSONSerialization.data(withJSONObject: value, options: [.fragmentsAllowed])
        return String(decoding: data, as: UTF8.self)
    }

    nonisolated private static func take(_ pointer: UnsafeMutablePointer<CChar>?) -> String? {
        guard let pointer else { return nil }
        defer { zebkit_free(pointer) }
        return String(cString: pointer)
    }

    /// An origin as a browser serializes it: "app://reader", "https://a.example:8443".
    private static func origin(of origin: WKSecurityOrigin) -> String {
        let port = origin.port
        let defaultPort = (origin.protocol == "https" && port == 443) || (origin.protocol == "http" && port == 80)
        return "\(origin.protocol)://\(origin.host)" + (port == 0 || defaultPort ? "" : ":\(port)")
    }
}

extension ZebkitViewer: WKNavigationDelegate {
    public func webView(_ webView: WKWebView, didCommit navigation: WKNavigation!) {
        // A new document: the last one's documents close.
        zebkit_viewer_reset(viewer)
        navigationDelegate?.webView?(webView, didCommit: navigation)
    }

    // Everything else is the application's.
    override public func responds(to selector: Selector!) -> Bool {
        super.responds(to: selector) || (navigationDelegate?.responds(to: selector) ?? false)
    }

    override public func forwardingTarget(for selector: Selector!) -> Any? {
        if let delegate = navigationDelegate, delegate.responds(to: selector) { return delegate }
        return super.forwardingTarget(for: selector)
    }
}

/// The message handler, which the content controller keeps strongly: weak to
/// the viewer, so that the viewer can go.
private final class MessageProxy: NSObject, WKScriptMessageHandler {
    weak var viewer: ZebkitViewer?
    init(_ viewer: ZebkitViewer) { self.viewer = viewer }

    func userContentController(_ controller: WKUserContentController, didReceive message: WKScriptMessage) {
        MainActor.assumeIsolated { viewer?.receive(message) }
    }
}

private struct UnsafeSendable<T>: @unchecked Sendable {
    let value: T
    init(_ value: T) { self.value = value }
}

struct ViewerError: Error, CustomStringConvertible {
    let description: String
    init(_ description: String) { self.description = description }
}
