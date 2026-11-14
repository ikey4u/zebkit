import WebKit
import XCTest
import Zebkit
@testable import ZebkitViewer

/// The viewer in a real WKWebView: the page's zebkit, answered by libzebkit.
@MainActor
final class ZebkitViewerTests: XCTestCase {
    /// A page with "Hello zebkit" in Helvetica, as src/engine/mobile_test.c has it.
    private static let pdf = """
        %PDF-1.4
        1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj
        2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj
        3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >> endobj
        4 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj
        5 0 obj << /Length 43 >> stream
        BT /F1 18 Tf 20 40 Td (Hello zebkit) Tj ET
        endstream endobj
        trailer << /Root 1 0 R >>
        %%EOF

        """

    private func loadedViewer(origin: String = "*") async throws -> ZebkitViewer {
        XCTAssertTrue(ZebkitViewer.initICU(), "icudtl.dat in Zebkit.framework")
        let viewer = ZebkitViewer()
        viewer.grant(origin: origin, allow: .all)
        viewer.bind("echo") { args in args }
        viewer.bind("fail") { _ in throw ViewerError("refused on purpose") }
        viewer.webView.loadHTMLString("<!doctype html><title>t</title><p>page</p>",
                                      baseURL: URL(string: "https://viewer.test/"))
        for _ in 0..<100 {
            let kind = try? await viewer.webView.evaluateJavaScript("typeof zebkit") as? String
            if kind == "object" { return viewer }
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        XCTFail("the page never had zebkit")
        return viewer
    }

    private func run(_ viewer: ZebkitViewer, _ body: String, _ arguments: [String: Any] = [:]) async throws -> [String: Any] {
        let value = try await viewer.webView.callAsyncJavaScript(
            "return JSON.stringify(await (async () => { \(body) })())", arguments: arguments, contentWorld: .page)
        let text = try XCTUnwrap(value as? String)
        return try XCTUnwrap(JSONSerialization.jsonObject(with: Data(text.utf8)) as? [String: Any])
    }

    func testServices() async throws {
        let viewer = try await loadedViewer()
        let result = try await run(viewer, """
            const packed = await zebkit.compress('zstd', new TextEncoder().encode('hello hello hello'), 3);
            const back = new TextDecoder().decode(await zebkit.decompress('zstd', packed));
            const digest = await zebkit.crypto.digest('SHA-256', 'abc');
            const url = await zebkit.url.parse('../b?q=1', 'https://bücher.example/a/c');
            let refused = '';
            try { await zebkit.url.registrableDomain('a.example.co.uk'); } catch (e) { refused = e.message; }
            return {back, digest: Array.from(digest.slice(0, 4)), href: url.href, refused};
            """)
        XCTAssertEqual(result["back"] as? String, "hello hello hello")
        XCTAssertEqual(result["digest"] as? [Int], [0xba, 0x78, 0x16, 0xbf])
        XCTAssertEqual(result["href"] as? String, "https://xn--bcher-kva.example/b?q=1")
        XCTAssertEqual(result["refused"] as? String, "zebkit.url.registrableDomain needs zebkit's engine")
    }

    func testApplicationFunctions() async throws {
        let viewer = try await loadedViewer()
        let result = try await run(viewer, """
            const echo = await zebkit.app.echo('hi', new Uint8Array([1, 2, 3]));
            let failed = '';
            try { await zebkit.app.fail(); } catch (e) { failed = e.message; }
            let missing = '';
            try { await zebkit.app.nothing(); } catch (e) { missing = e.message; }
            return {text: echo[0], bytes: Array.from(echo[1]), failed, missing};
            """)
        XCTAssertEqual(result["text"] as? String, "hi")
        XCTAssertEqual(result["bytes"] as? [Int], [1, 2, 3])
        XCTAssertEqual(result["failed"] as? String, "refused on purpose")
        XCTAssertEqual(result["missing"] as? String, "zebkit.app has no nothing")
    }

    func testPdf() async throws {
        let viewer = try await loadedViewer()
        let result = try await run(viewer, """
            const doc = await zebkit.pdf.open(new TextEncoder().encode(source));
            const info = await doc.info();
            const text = await doc.text(0);
            const image = await doc.render(0, {scale: 2});
            let dark = 0;
            for (let i = 0; i < image.data.length; i += 4) if (image.data[i] < 128) dark++;
            const saved = await doc.save();
            await doc.close();
            return {pages: info.pages, text, width: image.width, height: image.height, dark, saved: saved.length};
            """, ["source": Self.pdf])
        XCTAssertEqual(result["pages"] as? Int, 1)
        XCTAssertEqual((result["text"] as? String)?.contains("Hello zebkit"), true)
        XCTAssertEqual(result["width"] as? Int, 400)
        XCTAssertEqual(result["height"] as? Int, 200)
        XCTAssertGreaterThan(result["dark"] as? Int ?? 0, 100)
        XCTAssertGreaterThan(result["saved"] as? Int ?? 0, 100)
    }

    func testOtherOriginsAreRefused() async throws {
        let viewer = try await loadedViewer(origin: "https://elsewhere.example")
        let result = try await run(viewer, """
            try { await zebkit.crypto.randomBytes(4); return {refused: ''}; }
            catch (e) { return {refused: e.message}; }
            """)
        XCTAssertEqual(result["refused"] as? String, "zebkit.crypto is not allowed for https://viewer.test")
    }
}
