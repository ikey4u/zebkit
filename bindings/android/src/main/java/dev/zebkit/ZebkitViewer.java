package dev.zebkit;

import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.Bitmap;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

import org.json.JSONArray;
import org.json.JSONObject;

/**
 * zebkit's viewer on Android (docs/VIEWER.md): a {@link WebView} whose pages
 * have the global {@code zebkit} they have in zebkit's own views on Linux,
 * macOS and Windows: {@code zebkit.app.<name>()} for the functions bound
 * here, {@code zebkit.pdf}, {@code zebkit.compress}, {@code crypto},
 * {@code image}, {@code url.parse}, {@code fs}, {@code fetch}. libzebkit
 * answers the calls (it has PDFium and the rest, without the engine);
 * Android's System WebView draws the pages.
 *
 * <pre>{@code
 * ZebkitViewer viewer = new ZebkitViewer(webView);
 * viewer.grant("https://app.local", ZebkitViewer.ALLOW_APP | ZebkitViewer.ALLOW_PDF, null);
 * viewer.setAppContent("app.local", path -> getAssets().open("web/" + path));
 * viewer.bind("save", args -> saveNote(args.getString(0)));   // zebkit.app.save(text)
 * viewer.loadUrl("https://app.local/index.html");
 * }</pre>
 *
 * Calls run off the main thread; bound functions are called on a worker
 * thread. Pages of app content get {@code zebkit} before their own script;
 * other pages get it as they start loading, which their first script may
 * run before.
 */
public final class ZebkitViewer {
    /** What a page may use (zebkit_allow in {@code <zebkit/zebkit.h>}). */
    public static final int ALLOW_APP = 1;
    public static final int ALLOW_FETCH = 1 << 1;
    public static final int ALLOW_FILES = 1 << 2;
    public static final int ALLOW_COMPRESS = 1 << 3;
    public static final int ALLOW_CRYPTO = 1 << 4;
    public static final int ALLOW_IMAGE = 1 << 5;
    public static final int ALLOW_URL = 1 << 6;
    public static final int ALLOW_PDF = 1 << 7;
    public static final int ALLOW_ALL = 0xff;

    /**
     * A function a page calls as {@code zebkit.app.<name>(...args)}: its
     * arguments as the page gave them (bytes as {"$bytes": base64}); returns
     * what the page's promise resolves to, a JSON value (a JSONObject,
     * JSONArray, String, Number, Boolean, or null). An exception rejects it.
     */
    public interface AppFunction {
        Object call(JSONArray args) throws Exception;
    }

    /** The files of app content: the stream of {@code path}, or null for none. */
    public interface ContentSource {
        InputStream open(String path) throws IOException;
    }

    private static final String INTERFACE = "__zebkitAndroid";

    private final WebView webView;
    private final long viewer;
    private final String script;
    private final Map<String, AppFunction> functions = new ConcurrentHashMap<>();
    private final Map<String, ContentSource> content = new ConcurrentHashMap<>();
    private final ExecutorService calls = Executors.newCachedThreadPool();
    private final Handler main = new Handler(Looper.getMainLooper());
    private volatile String origin = "";
    private WebViewClient client;
    private boolean destroyed;

    /** Copies the APK asset {@code asset} (an icudtl.dat) out and loads it, once. */
    public static boolean initIcu(Context context, String asset) {
        File file = new File(context.getNoBackupFilesDir(), "zebkit-icudtl.dat");
        if (!file.exists()) {
            try (InputStream in = context.getAssets().open(asset);
                 OutputStream out = new FileOutputStream(file)) {
                byte[] buffer = new byte[64 * 1024];
                for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
            } catch (IOException e) {
                file.delete();
                return false;
            }
        }
        return Native.icuInitFromFile(file.getPath());
    }

    @SuppressLint({"SetJavaScriptEnabled", "AddJavascriptInterface"})
    public ZebkitViewer(WebView webView) {
        this.webView = webView;
        this.viewer = Native.viewerNew();
        this.script = Native.viewerScript("(m) => " + INTERFACE + ".post(m)");
        webView.getSettings().setJavaScriptEnabled(true);
        webView.addJavascriptInterface(new Port(), INTERFACE);
        webView.setWebViewClient(new Client());
    }

    /** The WebView it drives. */
    public WebView getWebView() {
        return webView;
    }

    /**
     * Gives pages of {@code origin} ("https://app.local", or "*" for pages the
     * application wrote itself) what {@code allow} says; 0 takes it back.
     * {@code filesRoot} is zebkit.fs's directory for ALLOW_FILES, or null.
     */
    public void grant(String origin, int allow, String filesRoot) {
        Native.viewerGrant(viewer, origin, allow, filesRoot);
    }

    /** {@code zebkit.app.<name>}; null removes it. */
    public void bind(String name, AppFunction function) {
        if (function == null) functions.remove(name);
        else functions.put(name, function);
    }

    /**
     * Serves {@code https://<host>/<path>} from {@code source}: the
     * application's own pages, which get {@code zebkit} before their script.
     */
    public void setAppContent(String host, ContentSource source) {
        if (source == null) content.remove(host);
        else content.put(host, source);
    }

    /**
     * The application's own WebViewClient, which the viewer passes what it
     * does not handle itself (navigation, errors, requests).
     */
    public void setWebViewClient(WebViewClient client) {
        this.client = client;
    }

    public void loadUrl(String url) {
        webView.loadUrl(url);
    }

    /** Runs {@code source} in the page; {@code result} gets its value as JSON. */
    public void evaluate(String source, android.webkit.ValueCallback<String> result) {
        webView.evaluateJavascript(source, result);
    }

    /** Frees libzebkit's side: the documents pages opened. Call before the WebView goes. */
    public void destroy() {
        if (destroyed) return;
        destroyed = true;
        webView.removeJavascriptInterface(INTERFACE);
        calls.shutdown();
        Native.viewerFree(viewer);
    }

    // ---- the page's calls ----

    private final class Port {
        @JavascriptInterface
        public void post(String message) {
            // On WebView's own thread: the work goes to the viewer's.
            final String from = origin;
            calls.execute(() -> handle(from, message));
        }
    }

    private void handle(String from, String message) {
        if (destroyed) return;
        String[] answer = Native.viewerHandle(viewer, from, message);
        if (answer[0] != null) {
            run(answer[0]);
            return;
        }
        if (answer[1] == null) return;
        long id = 0;
        try {
            JSONObject call = new JSONObject(answer[1]);
            id = call.getLong("id");
            String module = call.getString("module");
            JSONArray args = call.getJSONArray("args");
            Object result = module.equals("fetch") ? fetch(args) : app(call.getString("method"), args);
            run(Native.viewerAnswer(id, true, toJson(result)));
        } catch (Exception e) {
            String reason = e.getMessage() != null ? e.getMessage() : e.toString();
            run(Native.viewerAnswer(id, false, reason));
        }
    }

    private Object app(String name, JSONArray args) throws Exception {
        AppFunction function = functions.get(name);
        if (function == null) throw new IllegalStateException("zebkit.app has no " + name);
        Object result = function.call(args);
        return result == null ? JSONObject.NULL : result;
    }

    /** zebkit.fetch: {status, url, headers, body}, body as bytes, as zebkit's views give it. */
    private Object fetch(JSONArray args) throws Exception {
        String url = args.getString(0);
        JSONObject options = args.optJSONObject(1);
        HttpURLConnection connection = (HttpURLConnection) new URL(url).openConnection();
        try {
            connection.setRequestMethod(options != null ? options.optString("method", "GET") : "GET");
            JSONObject headers = options != null ? options.optJSONObject("headers") : null;
            if (headers != null) {
                for (Iterator<String> it = headers.keys(); it.hasNext(); ) {
                    String key = it.next();
                    connection.setRequestProperty(key, headers.getString(key));
                }
            }
            byte[] body = options != null ? bytes(options.opt("body")) : null;
            if (body != null) {
                connection.setDoOutput(true);
                try (OutputStream out = connection.getOutputStream()) {
                    out.write(body);
                }
            }
            int status = connection.getResponseCode();
            InputStream in = status >= 400 ? connection.getErrorStream() : connection.getInputStream();
            byte[] data = in == null ? new byte[0] : readAll(in);
            StringBuilder lines = new StringBuilder();
            for (Map.Entry<String, List<String>> header : connection.getHeaderFields().entrySet()) {
                if (header.getKey() == null) continue;
                for (String value : header.getValue())
                    lines.append(header.getKey()).append(": ").append(value).append("\r\n");
            }
            JSONObject response = new JSONObject();
            response.put("status", status);
            response.put("url", connection.getURL().toString());
            response.put("headers", lines.toString());
            response.put("body", new JSONObject().put("$bytes",
                    android.util.Base64.encodeToString(data, android.util.Base64.NO_WRAP)));
            return response;
        } finally {
            connection.disconnect();
        }
    }

    /** A result as JSON text; null for undefined. */
    private static String toJson(Object value) {
        if (value == null) return null;
        Object wrapped = JSONObject.wrap(value);
        if (wrapped == null || wrapped == JSONObject.NULL) return "null";
        if (wrapped instanceof String) return JSONObject.quote((String) wrapped);
        return wrapped.toString();
    }

    /** Bytes the page sent: {$bytes: base64}, or a string's UTF-8. */
    private static byte[] bytes(Object value) {
        if (value instanceof JSONObject && ((JSONObject) value).has("$bytes"))
            return android.util.Base64.decode(((JSONObject) value).optString("$bytes"), android.util.Base64.DEFAULT);
        if (value instanceof String) return ((String) value).getBytes(StandardCharsets.UTF_8);
        return null;
    }

    private static byte[] readAll(InputStream in) throws IOException {
        try (InputStream stream = in; ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[64 * 1024];
            for (int n; (n = stream.read(buffer)) > 0; ) out.write(buffer, 0, n);
            return out.toByteArray();
        }
    }

    private void run(String javascript) {
        if (javascript == null) return;
        main.post(() -> {
            if (!destroyed) webView.evaluateJavascript(javascript, null);
        });
    }

    private static String originOf(String url) {
        if (url == null) return "";
        Uri uri = Uri.parse(url);
        if (uri.getScheme() == null || uri.getHost() == null) return "";
        int port = uri.getPort();
        return uri.getScheme() + "://" + uri.getHost() + (port > 0 ? ":" + port : "");
    }

    // ---- loading ----

    private final class Client extends WebViewClient {
        @Override
        public void onPageStarted(WebView view, String url, Bitmap favicon) {
            // A new document: the last one's documents close, and calls come
            // from this one's origin. (A plain WebView does not say which
            // frame called: frames of another origin than the page's are
            // answered as the page.)
            origin = originOf(url);
            Native.viewerReset(viewer);
            view.evaluateJavascript(script, null);
            if (client != null) client.onPageStarted(view, url, favicon);
        }

        @Override
        public void onPageFinished(WebView view, String url) {
            if (client != null) client.onPageFinished(view, url);
        }

        @Override
        public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
            return client != null && client.shouldOverrideUrlLoading(view, request);
        }

        @Override
        public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
            Uri url = request.getUrl();
            ContentSource source = "https".equals(url.getScheme()) && url.getHost() != null
                    ? content.get(url.getHost()) : null;
            if (source == null)
                return client != null ? client.shouldInterceptRequest(view, request) : null;
            String path = url.getPath() == null || url.getPath().equals("/") ? "index.html" : url.getPath().substring(1);
            try {
                InputStream in = source.open(path);
                if (in == null) return notFound();
                String type = mimeType(path);
                if (type.equals("text/html") && request.isForMainFrame()) {
                    // zebkit before the page's own script.
                    String html = new String(readAll(in), StandardCharsets.UTF_8);
                    in = new ByteArrayInputStream(withScript(html).getBytes(StandardCharsets.UTF_8));
                }
                WebResourceResponse response = new WebResourceResponse(type, "UTF-8", in);
                Map<String, String> headers = new HashMap<>();
                headers.put("Access-Control-Allow-Origin", "https://" + url.getHost());
                response.setResponseHeaders(headers);
                return response;
            } catch (IOException e) {
                return notFound();
            }
        }
    }

    private String withScript(String html) {
        String tag = "<script>" + script.replace("</script", "<\\/script") + "</script>";
        int head = html.toLowerCase(java.util.Locale.ROOT).indexOf("<head");
        if (head >= 0) {
            int end = html.indexOf('>', head);
            if (end > 0) return html.substring(0, end + 1) + tag + html.substring(end + 1);
        }
        return tag + html;
    }

    private static WebResourceResponse notFound() {
        WebResourceResponse response = new WebResourceResponse("text/plain", "UTF-8",
                new ByteArrayInputStream(new byte[0]));
        response.setStatusCodeAndReasonPhrase(404, "Not Found");
        return response;
    }

    private static String mimeType(String path) {
        String lower = path.toLowerCase(java.util.Locale.ROOT);
        if (lower.endsWith(".html") || lower.endsWith(".htm")) return "text/html";
        if (lower.endsWith(".js") || lower.endsWith(".mjs")) return "text/javascript";
        if (lower.endsWith(".css")) return "text/css";
        if (lower.endsWith(".json")) return "application/json";
        if (lower.endsWith(".svg")) return "image/svg+xml";
        if (lower.endsWith(".png")) return "image/png";
        if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) return "image/jpeg";
        if (lower.endsWith(".webp")) return "image/webp";
        if (lower.endsWith(".woff2")) return "font/woff2";
        if (lower.endsWith(".wasm")) return "application/wasm";
        if (lower.endsWith(".pdf")) return "application/pdf";
        return "application/octet-stream";
    }
}
