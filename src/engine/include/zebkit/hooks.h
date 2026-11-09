/* Host policy for a view. Defaults apply until explicitly replaced. */
#ifndef ZEBKIT_HOOKS_H
#define ZEBKIT_HOOKS_H

#include "zebkit.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Merge validated preferences into this view; null removes an override.
 * {} is a no-op. Invalid input changes nothing. Applies to every frame,
 * existing documents and future navigations through Chromium WebPreferences.
 * Getter returns explicit overrides (not temporary CDP emulation), caller frees.
 * See docs/ROADMAP_HOOKS.md for the supported fields. Engine thread only.
 */
ZEBKIT_API bool zebkit_view_set_settings(zebkit_view *view, const char *settings_json, char **error);
ZEBKIT_API char *zebkit_view_get_settings(zebkit_view *view);

/* What zebkit decides itself, changed while it runs: batches of JSON, merged by key.
 * null removes a value, {} does nothing; an unknown key, a bad value or a wrong type
 * is an error, and nothing of the batch changes. Each has today's default until set.
 * The values:
 *   "thirdPartyCookies": "block" | "allow". --zebkit-allow-third-party-cookies is the
 *     default. The engine's goes to all contexts, now and later, over what a view
 *     asked; a view's goes to its context, all the views in it. For requests from when
 *     the network has it (asynchronously); a document keeps the cookies it has read.
 *   "dialogs": "default" | "hold" | "accept" | "dismiss", <zebkit/page.h>'s policies.
 *     zebkit_view_set_dialog_policy sets the same value of the view.
 *   "downloads": "save" | "deny". With no function (zebkit_view_on_download): saved
 *     or cancelled. A function still decides.
 *   "newWindows": "block-without-gesture" | "allow" | "block", zebkit_popup_policy.
 *     zebkit_view_set_popup_policy sets the same value of the view.
 *   "spareRenderer": true | false. The engine's alone; --zebkit-spare-renderer is the
 *     default. True makes a renderer ready when a page has finished loading, which
 *     the next navigation to another site takes; a frame committing before then drops
 *     it. False takes it away, within a load when the engine started with the switch.
 *     Content's own keeping of one is fixed at startup: only the switch gives that.
 * A view's value goes before the engine's, which goes before the default; a popup
 * starts with its opener's newWindows and downloads.
 * Getters: {"explicit": what was set, "effective": what holds}, JSON the caller frees;
 * the view's "downloads" is also "ask" when a function decides, "deny" for a popup
 * whose opener has closed, and the engine's has "spareRendererRunning". NULL outside
 * the running engine's thread or for a closed view. A page never has these.
 * Engine thread only; they are separate from the web preferences above.
 */
ZEBKIT_API bool zebkit_set_engine_settings(const char *settings_json, char **error);
ZEBKIT_API char *zebkit_get_engine_settings(void);
ZEBKIT_API bool zebkit_view_set_runtime_settings(zebkit_view *view, const char *settings_json, char **error);
ZEBKIT_API char *zebkit_view_get_runtime_settings(zebkit_view *view);

typedef enum zebkit_permission_state {
    ZEBKIT_PERMISSION_INVALID = -1,
    ZEBKIT_PERMISSION_PROMPT = 0,
    ZEBKIT_PERMISSION_GRANTED = 1,
    ZEBKIT_PERMISSION_DENIED = 2
} zebkit_permission_state;

/* Origin is a non-opaque URL (canonicalized to its origin), or "*" for a
 * view-wide default; NULL uses the current non-opaque document origin.
 * Permission names include geolocation, notifications,
 * camera, microphone, clipboard-read/write and screen-wake-lock. Invalid
 * input changes nothing. Runtime changes notify navigator.permissions.
 * Later CDP/context changes take precedence in their actual Chromium scope;
 * later explicit view changes replace that decision for this view.
 */
ZEBKIT_API bool zebkit_view_set_permission(zebkit_view *view, const char *origin, const char *permission,
                                           zebkit_permission_state state, char **error);
/* Getter needs a concrete origin; NULL uses the current document origin.
 * It reports policy; document security/permissions-policy checks still apply.
 */
ZEBKIT_API zebkit_permission_state zebkit_view_get_permission(zebkit_view *view, const char *origin,
                                                              const char *permission);
/* NULL origin clears all view rules; "*" clears defaults only. Other
 * origins clear that origin's rules. Context/CDP overrides remain in force.
 */
ZEBKIT_API bool zebkit_view_reset_permissions(zebkit_view *view, const char *origin, char **error);
/* New requests use this deadline (1..300000 ms); default is 30000 ms. */
ZEBKIT_API bool zebkit_view_set_permission_timeout(zebkit_view *view, uint32_t milliseconds);

typedef struct zebkit_permission_request zebkit_permission_request;
/* JSON has id, permission, requestingOrigin, topOrigin, userGesture,
 * timeoutMs, deadlineUnixMs and state (pending). JSON is borrowed during callback;
 * the opaque request token may be retained and answered later. The engine
 * owns it: no free call is needed. Call on the engine thread (post from any
 * other thread). Replacing a handler cancels its outstanding requests.
 */
typedef void (*zebkit_permission_func)(zebkit_view *view, zebkit_permission_request *request,
                                      const char *json, void *data);
ZEBKIT_API void zebkit_view_on_permission(zebkit_view *view, zebkit_permission_func func, void *data);
/* Owned JSON while pending, NULL after settlement/cancellation. Caller frees. */
ZEBKIT_API char *zebkit_permission_request_info(zebkit_permission_request *request);
/* Exactly one reply. Prompt dismisses. Remember stores an origin decision;
 * false grants/denies only this request. Late/invalid replies return false.
 * Opaque origins cannot be remembered. Approximate-only geolocation cannot
 * be stored independently; use geolocation for a persistent decision.
 * Navigation, closing, handler replacement and deadlines settle requests.
 */
ZEBKIT_API bool zebkit_permission_respond(zebkit_permission_request *request, zebkit_permission_state state,
                                         bool remember);
/* Outcomes of host-handled requests, asynchronously: metadata plus state,
 * reason (answered/timeout/cancelled/handler-changed/policy-changed), remembered.
 * Closing the view cancels pending reports. NULL removes the observer.
 */
typedef void (*zebkit_permission_result_func)(zebkit_view *view, const char *json, void *data);
ZEBKIT_API void zebkit_view_on_permission_result(zebkit_view *view, zebkit_permission_result_func func, void *data);

/* Document navigations: start, redirect, response, failure, commit for
 * navigations without a URL loader, or activation before BFCache/prerender
 * becomes the active page. Renderer-only same-document changes remain outside
 * this policy. activationType is "back-forward-cache" or "prerender".
 * Activation runs before startup sanitizes the referrer; referrer is empty
 * and referrerAvailable is false for these stages.
 * Metadata includes id/navigationId/frameId, phase, url, method, mainFrame,
 * userGesture, rendererInitiated, sameDocument, referrer, redirectChain,
 * responseStatus/responseHeaders, netError, timeoutMs and deadlineUnixMs.
 * On redirect, url is the destination and headers belong to the redirect.
 * Header pairs preserve duplicates; headersTruncated reports bounded output.
 * Callback JSON is borrowed. Tokens may be retained, never freed by the host.
 * All methods run on the engine thread; post replies from other threads.
 */
typedef struct zebkit_navigation_request zebkit_navigation_request;
typedef enum zebkit_navigation_action {
    ZEBKIT_NAVIGATION_CONTINUE = 0,
    ZEBKIT_NAVIGATION_CANCEL = 1,
    ZEBKIT_NAVIGATION_REDIRECT = 2
} zebkit_navigation_action;
typedef void (*zebkit_navigation_func)(zebkit_view *view, zebkit_navigation_request *request,
                                      const char *json, void *data);
ZEBKIT_API void zebkit_view_on_navigation(zebkit_view *view, zebkit_navigation_func func, void *data);
/* New requests: 1..300000 milliseconds, default 30000. Unanswered requests
 * continue. Replacing/removing a handler continues its pending requests.
 * Closing, superseding or cancelling a navigation invalidates its tokens.
 *
 * More than 1024 requests pending on a view are cancelled, not continued. A
 * popup is decided for by the handlers of the view that opened it, called with
 * the popup as the view, until it has its own; result events are only sent to
 * a handler on the popup itself. Once the opener has closed, a popup without
 * its own handlers is refused navigations, permissions, certificates and
 * credentials.
 */
ZEBKIT_API bool zebkit_view_set_navigation_timeout(zebkit_view *view, uint32_t milliseconds);
/* True: an unanswered request (a timeout or a removed handler) is cancelled
 * instead of continued, for a host that filters navigations for safety. A
 * popup takes its opener's choice when it opens.
 */
ZEBKIT_API bool zebkit_view_set_navigation_fail_closed(zebkit_view *view, bool fail_closed);
/* Owned JSON while pending; NULL after settlement. Caller frees. */
ZEBKIT_API char *zebkit_navigation_request_info(zebkit_navigation_request *request);
/* Reply once. Redirect cancels the original navigation and starts a GET of
 * replacement_url in the same frame. Other actions require NULL replacement.
 * Invalid input leaves the request pending. Late/stale replies return false.
 */
ZEBKIT_API bool zebkit_navigation_respond(zebkit_navigation_request *request, zebkit_navigation_action action,
                                        const char *replacement_url, char **error);
/* Outcomes: request metadata plus action (continue/cancel/redirect), reason
 * (answered/timeout/cancelled/handler-changed/shutdown), and replacementUrl
 * for redirects. Destruction cancels queued reports. NULL removes observer.
 */
typedef void (*zebkit_navigation_result_func)(zebkit_view *view, const char *json, void *data);
ZEBKIT_API void zebkit_view_on_navigation_result(zebkit_view *view, zebkit_navigation_result_func func, void *data);

typedef enum zebkit_popup_policy {
    ZEBKIT_POPUP_POLICY_BLOCK_WITHOUT_GESTURE = 0,
    ZEBKIT_POPUP_POLICY_ALLOW = 1,
    ZEBKIT_POPUP_POLICY_BLOCK = 2
} zebkit_popup_policy;

/* Replaces the policy for future window.open calls, immediately. Explicit
 * per-view policy takes precedence over --disable-popup-blocking. Invalid
 * values leave the current policy unchanged and return false.
 */
ZEBKIT_API bool zebkit_view_set_popup_policy(zebkit_view *view, zebkit_popup_policy policy);
ZEBKIT_API zebkit_popup_policy zebkit_view_get_popup_policy(zebkit_view *view);

/* A blocked popup reported after the decision, on the engine thread.
 * event_json: {url, frameName, userGesture}. Valid during the callback.
 * Changing policy here affects future attempts, not this one. NULL removes
 * the callback. Closing the view cancels pending reports. Unlike a decision
 * callback, this may safely close the view or navigate it.
 */
typedef void (*zebkit_popup_blocked_func)(zebkit_view *view, const char *event_json, void *data);
ZEBKIT_API void zebkit_view_on_popup_blocked(zebkit_view *view, zebkit_popup_blocked_func func, void *data);

/* All new-window attempts, asynchronously. phase is decision or created.
 * Decision events include allowed, url, frameName, userGesture, disposition,
 * openerUrl/topUrl/sourceOrigin/referrer, openerSuppressed, containerType
 * (0 normal, 1 background, 2 persistent) and features.
 * Created events include url, targetId, frameName, disposition, userGesture,
 * features, hasOpener, openerOrigin (null without an opener), and topOrigin.
 * features contains bounds, explicit coordinate/size flags, popup and alwaysOnTop.
 * These are Chromium-supplied window parameters, not measured toolkit bounds.
 * Observing does not adopt or redirect a window: use on_window_open for its view.
 * JSON is borrowed; NULL removes the observer. Closing or replacing/removing
 * the observer cancels its queued reports.
 */
typedef void (*zebkit_window_open_event_func)(zebkit_view *view, const char *json, void *data);
ZEBKIT_API void zebkit_view_on_window_open_event(zebkit_view *view, zebkit_window_open_event_func func, void *data);
/* Owned creation metadata for a popup view; {} for an ordinary view.
 * Available inside on_window_open and retained across navigation. Caller frees.
 */
ZEBKIT_API char *zebkit_view_get_window_open_info(zebkit_view *view);

/* Native document-console messages; this does not evaluate page objects.
 * JSON: level (debug/info/warning/error), message, source, line, frameId,
 * mainFrame, origin, stackTrace (null when unavailable), truncated, dropped.
 * stackTrace is Chromium's optional, untrusted exception text, not structured
 * JavaScript frames. Worker/DevTools-only console events are outside this hook.
 * Delivery is asynchronous on the engine thread. Closing or replacing/removing
 * the observer cancels queued reports. NULL removes it. Per view, at most 256
 * reports wait; dropped is a decimal string counting skipped reports since
 * the previous delivery.
 * Text/source/stack are bounded to 65536/16384/65536 UTF-16 code units.
 */
typedef void (*zebkit_console_func)(zebkit_view *view, const char *json, void *data);
ZEBKIT_API void zebkit_view_on_console(zebkit_view *view, zebkit_console_func func, void *data);

/* Document certificate errors. No handler means deny. Chromium/CDP may handle
 * an error before the embedder hook. Replies are engine-thread-only and single
 * use; strictEnforcement errors cannot be continued. There is no remembered
 * exception or global ignore-certificate-errors switch in this API.
 * Requests expire after 30000 ms by default (range 1..300000). Top-level navigation,
 * closure and handler replacement deny pending requests. metadata includes
 * the URL, error, mainFrame, strictEnforcement/canIgnore, certificate subject,
 * issuer, validity, certStatus, knownRoot, a bounded PEM certificateChain and
 * chainTruncated. It does not claim to be a certificate trust verdict.
 */
typedef struct zebkit_certificate_request zebkit_certificate_request;
typedef void (*zebkit_certificate_func)(zebkit_view*, zebkit_certificate_request*, const char *json, void *data);
typedef void (*zebkit_certificate_result_func)(zebkit_view*, const char *json, void *data);
ZEBKIT_API void zebkit_view_on_certificate(zebkit_view*, zebkit_certificate_func, void *data);
ZEBKIT_API void zebkit_view_on_certificate_result(zebkit_view*, zebkit_certificate_result_func, void *data);
ZEBKIT_API bool zebkit_view_set_certificate_timeout(zebkit_view*, uint32_t milliseconds);
ZEBKIT_API char *zebkit_certificate_request_info(zebkit_certificate_request*);
ZEBKIT_API bool zebkit_certificate_respond(zebkit_certificate_request*, bool allow);

/* HTTP authentication challenges, including proxies. Installed handlers take
 * precedence over automation credentials at the browser login delegate. With
 * no handler, existing automation/shell behavior remains. Chromium's profile
 * auth cache and earlier CDP interception keep their native scopes.
 * Credentials supplied in a reply are not added to metadata or outcome JSON;
 * URL userinfo is redacted. mainFrame describes a primary-main-frame navigation,
 * not an inferred initiator for fetch. Request handles are engine-owned tokens:
 * do not free or dereference them. All methods require the engine thread.
 */
typedef struct zebkit_http_auth_request zebkit_http_auth_request;
typedef void (*zebkit_http_auth_func)(zebkit_view*, zebkit_http_auth_request*, const char *json, void *data);
typedef void (*zebkit_http_auth_result_func)(zebkit_view*, const char *json, void *data);
ZEBKIT_API void zebkit_view_on_http_auth(zebkit_view*, zebkit_http_auth_func, void *data);
ZEBKIT_API void zebkit_view_on_http_auth_result(zebkit_view*, zebkit_http_auth_result_func, void *data);
/* Default 30000 ms; accepted range 1..300000. */
ZEBKIT_API bool zebkit_view_set_http_auth_timeout(zebkit_view*, uint32_t milliseconds);
/* Owned JSON while pending; free with zebkit_free(). NULL for stale tokens. */
ZEBKIT_API char *zebkit_http_auth_request_info(zebkit_http_auth_request*);
/* Both NULL cancel; both non-NULL provide credentials (empty strings allowed).
 * Each field is valid UTF-8, at most 65536 bytes. Invalid replies leave the
 * challenge pending. A request can be answered once before its deadline.
 */
ZEBKIT_API bool zebkit_http_auth_respond(zebkit_http_auth_request*, const char *username, const char *password);

/* Links to other programs: a navigation to a scheme the engine does not load
 * (mailto:, zoommtg:). The engine opens nothing, with or without a function; the
 * page stays where it is and the application decides. Told asynchronously:
 * url, scheme, mainFrame, userGesture, initiatorOrigin (null when none), frameId.
 * As Chrome, fenced frames and sandboxed frames without
 * allow-top-navigation-to-custom-protocols are not told. At most 256 reports of
 * this and the other told hooks wait per view; more are dropped. NULL removes it.
 */
typedef void (*zebkit_external_protocol_func)(zebkit_view*, const char *json, void *data);
ZEBKIT_API void zebkit_view_on_external_protocol(zebkit_view*, zebkit_external_protocol_func, void *data);

/* Downloads. Without a function they are saved in Downloads in the profile's
 * directory, a file already there kept ("report (1).bin"). With one, each is
 * asked about: downloadId, url, suggestedName, mimeType, totalBytes (null unknown),
 * userGesture, origin, timeoutMs, deadlineUnixMs. Reply once: save at an absolute
 * path, whose directory exists and whose file is replaced, or cancel. Unanswered,
 * replaced handlers and closed views cancel. A popup's opener decides until it has
 * its own function; once that opener has closed, its downloads are cancelled, and
 * so is one whose view has gone once any view has set a function.
 * CDP's setDownloadBehavior keeps its own scope, ahead of this hook.
 */
typedef struct zebkit_download_request zebkit_download_request;
typedef enum zebkit_download_action {
    ZEBKIT_DOWNLOAD_SAVE = 0,
    ZEBKIT_DOWNLOAD_CANCEL = 1
} zebkit_download_action;
typedef void (*zebkit_download_func)(zebkit_view*, zebkit_download_request*, const char *json, void *data);
ZEBKIT_API void zebkit_view_on_download(zebkit_view*, zebkit_download_func, void *data);
/* Default 30000 ms; accepted range 1..300000. */
ZEBKIT_API bool zebkit_view_set_download_timeout(zebkit_view*, uint32_t milliseconds);
ZEBKIT_API char *zebkit_download_request_info(zebkit_download_request*);
/* Path only with SAVE. Invalid input leaves the request pending. */
ZEBKIT_API bool zebkit_download_respond(zebkit_download_request*, zebkit_download_action action,
                                        const char *path, char **error);
/* Told for this view's downloads, asked or not: the decision (state save or
 * cancelled with its reason), then downloadId, url, state (in-progress, complete,
 * cancelled, interrupted), path, receivedBytes, totalBytes, reason, done. A download
 * outlives its view, without reports. NULL removes it.
 */
typedef void (*zebkit_download_progress_func)(zebkit_view*, const char *json, void *data);
ZEBKIT_API void zebkit_view_on_download_progress(zebkit_view*, zebkit_download_progress_func, void *data);
/* Cancels a download of this view by its downloadId; false when it has ended. */
ZEBKIT_API bool zebkit_view_cancel_download(zebkit_view*, const char *download_id);

/* Client certificates a TLS server asks for. Without a function, none is sent.
 * Metadata: host, port, proxy, certificateAuthorities (base64 DER names),
 * timeoutMs, deadlineUnixMs. NONE continues without one; PROVIDE sends a PEM
 * chain, leaf first, with its PEM private key (RSA or EC); CANCEL fails the
 * request. Unanswered, a replaced handler and a closed view cancel. Chromium
 * remembers NONE and PROVIDE for that host and port in the whole context, so that
 * other views are not asked again, and CANCEL is not remembered.
 *
 * The operating system's certificate stores are offered only when the engine
 * setting clientCertificateStores is true (zebkit_set_engine_settings): then
 * metadata.identities lists the store's identities matching the server's
 * authorities ({index, subject, issuer, validFromUnixMs, validUntilUnixMs,
 * sha256, certificate}), from the user's NSS database on Linux, Keychain on
 * macOS, the user's MY store on Windows, and zebkit_client_certificate_choose()
 * answers with one: its key stays in the store, which signs; the engine never
 * reads it. An NSS key behind the database's password is not offered, and
 * Keychain may show its own access prompt the first time, which a host without
 * a screen cannot answer.
 */
typedef struct zebkit_client_certificate_request zebkit_client_certificate_request;
typedef enum zebkit_client_certificate_action {
    ZEBKIT_CLIENT_CERTIFICATE_NONE = 0,
    ZEBKIT_CLIENT_CERTIFICATE_PROVIDE = 1,
    ZEBKIT_CLIENT_CERTIFICATE_CANCEL = 2
} zebkit_client_certificate_action;
typedef void (*zebkit_client_certificate_func)(zebkit_view*, zebkit_client_certificate_request*, const char *json, void *data);
typedef void (*zebkit_client_certificate_result_func)(zebkit_view*, const char *json, void *data);
ZEBKIT_API void zebkit_view_on_client_certificate(zebkit_view*, zebkit_client_certificate_func, void *data);
ZEBKIT_API void zebkit_view_on_client_certificate_result(zebkit_view*, zebkit_client_certificate_result_func, void *data);
ZEBKIT_API bool zebkit_view_set_client_certificate_timeout(zebkit_view*, uint32_t milliseconds);
ZEBKIT_API char *zebkit_client_certificate_request_info(zebkit_client_certificate_request*);
/* PEM only with PROVIDE. A key not matching the leaf, or anything invalid, leaves
 * the request pending and returns false with the reason in *error.
 */
ZEBKIT_API bool zebkit_client_certificate_respond(zebkit_client_certificate_request*,
                                                  zebkit_client_certificate_action action,
                                                  const char *certificate_pem, const char *key_pem, char **error);
/* Answers with metadata.identities[index], the system store's: true when taken;
 * the request then ends once the store has given its key (PROVIDE), or, when it
 * gives none, as CANCEL. False with the reason in *error otherwise.
 */
ZEBKIT_API bool zebkit_client_certificate_choose(zebkit_client_certificate_request*, size_t index,
                                                 char **error);

/* A script before the page's own in every new document, and at once in those there
 * are. An empty or NULL world is the page's; a name is an isolated world, one per
 * name, sharing the DOM and not the globals. Chromium gives these worlds universal
 * access, as CDP's worldName has it: the world can read other origins.
 * main_frame_only false runs it in every frame, other sites' included; true wraps
 * it in a block, so that its top-level let, const and class are not global.
 * False with a reason in *error on invalid input.
 */
ZEBKIT_API bool zebkit_view_add_world_script(zebkit_view*, const char *world, const char *source,
                                             bool main_frame_only, char **error);

/* File choosers of a view: <input type=file> and File System Access's pickers
 * (showOpenFilePicker, showSaveFilePicker, showDirectoryPicker). Without a function,
 * the engine's: the system's dialog, or cancelled without a display. Ahead of
 * automation's file chooser events; CDP's interception stays ahead of this hook.
 * Metadata: mode (open, open-multiple, upload-folder, directory, save), api ("input"
 * or "file-system-access"), title, defaultFileName, acceptTypes, capture, frameId,
 * mainFrame, origin. Reply once with absolute paths: none cancels, one for open,
 * save, directory and upload-folder (a directory, whose files the page gets).
 * Unanswered, replaced, closed or a frame that has gone: cancelled. What a File
 * System Access page may then do: it reads what was picked and writes where it saves;
 * writing to a file it opened, or reading or writing beyond what was picked, is the
 * "file-system-write" or "file-system-read" permission (zebkit_view_on_permission).
 */
typedef struct zebkit_file_chooser_request zebkit_file_chooser_request;
typedef void (*zebkit_file_chooser_func)(zebkit_view*, zebkit_file_chooser_request*, const char *json, void *data);
typedef void (*zebkit_file_chooser_result_func)(zebkit_view*, const char *json, void *data);
ZEBKIT_API void zebkit_view_on_file_chooser(zebkit_view*, zebkit_file_chooser_func, void *data);
ZEBKIT_API void zebkit_view_on_file_chooser_result(zebkit_view*, zebkit_file_chooser_result_func, void *data);
ZEBKIT_API bool zebkit_view_set_file_chooser_timeout(zebkit_view*, uint32_t milliseconds);
ZEBKIT_API char *zebkit_file_chooser_request_info(zebkit_file_chooser_request*);
ZEBKIT_API bool zebkit_file_chooser_respond(zebkit_file_chooser_request*, const char *const *paths, size_t count,
                                            char **error);

/* The page's EyeDropper.open(): a colour from the screen. Without a function, cancelled,
 * as there is no eye dropper of the engine's (Chrome's is its browser's). Metadata:
 * origin, frameId, mainFrame. Reply once with "#rrggbb", or NULL to cancel. Unanswered,
 * replaced, closed or a frame that has gone: cancelled.
 */
typedef struct zebkit_eye_dropper_request zebkit_eye_dropper_request;
typedef void (*zebkit_eye_dropper_func)(zebkit_view*, zebkit_eye_dropper_request*, const char *json, void *data);
typedef void (*zebkit_eye_dropper_result_func)(zebkit_view*, const char *json, void *data);
ZEBKIT_API void zebkit_view_on_eye_dropper(zebkit_view*, zebkit_eye_dropper_func, void *data);
ZEBKIT_API void zebkit_view_on_eye_dropper_result(zebkit_view*, zebkit_eye_dropper_result_func, void *data);
ZEBKIT_API bool zebkit_view_set_eye_dropper_timeout(zebkit_view*, uint32_t milliseconds);
ZEBKIT_API char *zebkit_eye_dropper_request_info(zebkit_eye_dropper_request*);
ZEBKIT_API bool zebkit_eye_dropper_respond(zebkit_eye_dropper_request*, const char *color);

/*
 * A page's device chooser (docs/SYSTEM_INTEGRATION.md, 10):
 * navigator.usb.requestDevice(), navigator.hid.requestDevice() and
 * navigator.serial.requestPort(). The request's JSON has "kind" ("usb", "hid",
 * "serial"), "origin", "frameId", the page's "filters", and "devices", those
 * of the system's that match, each with an "id", a "name" and what the kind
 * has (vendorId, productId, serialNumber, path, ...). The list follows devices
 * as they come and go: zebkit_device_chooser_request_info() returns it as it
 * is now. Answered with a device's id, which the origin may then use for the
 * engine's run (and forget() ends), or NULL for none. Without a function the
 * page gets none, as before; there is no chooser of the engine's own.
 */
typedef struct zebkit_device_chooser_request zebkit_device_chooser_request;
typedef void (*zebkit_device_chooser_func)(zebkit_view*, zebkit_device_chooser_request*, const char *json, void *data);
typedef void (*zebkit_device_chooser_result_func)(zebkit_view*, const char *json, void *data);
ZEBKIT_API void zebkit_view_on_device_chooser(zebkit_view*, zebkit_device_chooser_func, void *data);
ZEBKIT_API void zebkit_view_on_device_chooser_result(zebkit_view*, zebkit_device_chooser_result_func, void *data);
ZEBKIT_API bool zebkit_view_set_device_chooser_timeout(zebkit_view*, uint32_t milliseconds);
ZEBKIT_API char *zebkit_device_chooser_request_info(zebkit_device_chooser_request*);
ZEBKIT_API bool zebkit_device_chooser_respond(zebkit_device_chooser_request*, const char *device_id);

/* The page's navigator.share(): the application answers it shared or not. Without a
 * function, the system's share sheet where there is one (macOS); otherwise cancelled
 * (AbortError), as Linux has none. Metadata: title, text, url, files (their names),
 * origin. Unanswered, replaced or closed: cancelled.
 */
typedef struct zebkit_share_request zebkit_share_request;
typedef void (*zebkit_share_func)(zebkit_view*, zebkit_share_request*, const char *json, void *data);
typedef void (*zebkit_share_result_func)(zebkit_view*, const char *json, void *data);
ZEBKIT_API void zebkit_view_on_share(zebkit_view*, zebkit_share_func, void *data);
ZEBKIT_API void zebkit_view_on_share_result(zebkit_view*, zebkit_share_result_func, void *data);
ZEBKIT_API bool zebkit_view_set_share_timeout(zebkit_view*, uint32_t milliseconds);
ZEBKIT_API char *zebkit_share_request_info(zebkit_share_request*);
ZEBKIT_API bool zebkit_share_respond(zebkit_share_request*, bool shared);

/* Camera and microphone (getUserMedia). Without a function, Chromium's permission
 * controller decides (zebkit_view_on_permission). With one it replaces that decision
 * for this request; Permissions Policy still refuses first. Screen and tab capture
 * stay refused. Metadata: audio, video, origin, frameId, mainFrame, userGesture,
 * requestedAudioDevices, requestedVideoDevices, audioDevices and videoDevices ({id,
 * name}). A device id NULL takes what the page asked for, else the first; a device
 * that does not exist fails as without hardware. Unanswered: refused.
 */
typedef struct zebkit_media_access_request zebkit_media_access_request;
typedef void (*zebkit_media_access_func)(zebkit_view*, zebkit_media_access_request*, const char *json, void *data);
typedef void (*zebkit_media_access_result_func)(zebkit_view*, const char *json, void *data);
ZEBKIT_API void zebkit_view_on_media_access(zebkit_view*, zebkit_media_access_func, void *data);
ZEBKIT_API void zebkit_view_on_media_access_result(zebkit_view*, zebkit_media_access_result_func, void *data);
ZEBKIT_API bool zebkit_view_set_media_access_timeout(zebkit_view*, uint32_t milliseconds);
ZEBKIT_API char *zebkit_media_access_request_info(zebkit_media_access_request*);
ZEBKIT_API bool zebkit_media_access_respond(zebkit_media_access_request*, bool allow, const char *audio_device_id,
                                            const char *video_device_id);

/* Keys the page does not get: a JSON array of "Ctrl+Shift+KeyI", modifiers Ctrl, Alt,
 * Shift, Meta and a KeyboardEvent.code. [] clears. At most 256; invalid input changes
 * nothing. As Chrome's accelerators, the page still sees the key's keyup. Then told
 * asynchronously: combo, key, code, autoRepeat. A popup takes its opener's keys until
 * it has its own.
 */
ZEBKIT_API bool zebkit_view_set_reserved_keys(zebkit_view*, const char *keys_json, char **error);
typedef void (*zebkit_reserved_key_func)(zebkit_view*, const char *json, void *data);
ZEBKIT_API void zebkit_view_on_reserved_key(zebkit_view*, zebkit_reserved_key_func, void *data);

/* With a function, the engine shows no context menu and tells what was asked: x, y,
 * linkUrl, linkText, srcUrl, mediaType, selectionText, editable, pageUrl, frameUrl,
 * frameId, menuId and items (the default menu's: [{id, title, enabled}] or
 * {separator: true}). Without one, the engine's default menu, with the system's look,
 * where there is a window: Back, Forward, Reload; Cut, Copy, Paste, Select All in a
 * field; Copy Link Address, Open Link in New Window; Save Image As, Copy Image.
 *
 * zebkit_view_context_menu_run() runs a default item's command against what was
 * right-clicked, as captured then: a page cannot name what is saved or opened. The
 * last eight menus of a view are kept. zebkit_view_context_menu_show() shows a menu
 * there: @items_json NULL for the default items, or a list mixing default ids (their
 * own title and state unless given, never more enabled) with the application's own
 * {id, title, enabled}; a default item chosen runs, the application's goes to @func
 * with its id ("" when the menu closes with none). False without a window.
 */
typedef void (*zebkit_context_menu_func)(zebkit_view*, const char *json, void *data);
ZEBKIT_API void zebkit_view_on_context_menu(zebkit_view*, zebkit_context_menu_func, void *data);
ZEBKIT_API bool zebkit_view_context_menu_run(zebkit_view*, uint64_t menu_id, const char *command, char **error);
typedef void (*zebkit_context_menu_item_func)(zebkit_view*, const char *item_id, void *data);
ZEBKIT_API bool zebkit_view_context_menu_show(zebkit_view*, uint64_t menu_id, const char *items_json,
                                              zebkit_context_menu_item_func func, void *data, char **error);

/* Whether a page may go fullscreen; allowed by default. Denying leaves fullscreen if
 * the page is in it. A popup takes its opener's policy until it has its own, and is
 * denied once that opener has closed. Told asynchronously: entered. As in Chrome,
 * a page goes fullscreen only from a user gesture in a view with focus
 * (zebkit_view_focus(), or a person's click).
 */
typedef enum zebkit_fullscreen_policy {
    ZEBKIT_FULLSCREEN_ALLOW = 0,
    ZEBKIT_FULLSCREEN_DENY = 1
} zebkit_fullscreen_policy;
ZEBKIT_API bool zebkit_view_set_fullscreen_policy(zebkit_view*, zebkit_fullscreen_policy policy);
typedef void (*zebkit_fullscreen_func)(zebkit_view*, const char *json, void *data);
ZEBKIT_API void zebkit_view_on_fullscreen(zebkit_view*, zebkit_fullscreen_func, void *data);

/* Network requests and responses of a view's frames and of their dedicated workers:
 * a loader factory in front of the network's, and their WebSockets' handshakes.
 * Nothing costs anything until a filter is set, but one mojo hop per request of a
 * shared or service worker. A filter is a JSON array of rules, replacing the last, of
 * at most 256:
 * {url: Playwright's glob (default every URL), mode: "hold" (default) or "observe",
 * stage: "request" (default), "sendHeaders", "sent", "headersReceived" or "response" (below), resourceTypes: ["document", "stylesheet",
 * "image", "media", "font", "script", "manifest", "fetch", "websocket"], frames: "all",
 * "main" or "sub", body: true (zebkit_view_on_network_body; response stage)}. Invalid
 * input changes nothing. Frames there are from then on; a dedicated worker running
 * already is not covered. Requests answered from a service worker never reach this.
 * CDP's Fetch and Network stay behind it.
 * A WebSocket is its handshake, at the request stage: continue, with changed headers
 * (Cookie and User-Agent too, what else the network would leave out is refused), or
 * block; redirect and fulfill are refused. Its frames are not seen. A rule without
 * resourceTypes matches it, with a ws: or wss: URL. A dedicated worker's is told as
 * its frame's, with worker false.
 */
ZEBKIT_API bool zebkit_view_set_network_filter(zebkit_view*, const char *filter_json, char **error);
/* This view's filter also decides for the shared and service workers of its profile,
 * running ones too, their scripts, fetches and WebSockets (a shared worker's first
 * script through its first client's frame included). False without a filter set: []
 * is one. A later call of another view takes over. While a policy is set and its view
 * has gone, those requests fail with ERR_BLOCKED_BY_CLIENT, unasked, until another view
 * takes over or any view says off: closing a view does not end a policy, only off does,
 * which another view's still there refuses. Workers' metadata has frameId "", mainFrame
 * false, worker true, workerType "shared" or "service" ("dedicated" for those of a
 * frame). Their WebSockets say "shared-or-service": content gives them no frame and no
 * profile, so that they go to the one profile with a policy whichever they are from,
 * and with two such profiles they fail. Their requests answered from a service
 * worker's own cache never reach this, nor do background fetches.
 */
ZEBKIT_API bool zebkit_view_set_network_filter_for_workers(zebkit_view*, bool on);
/* What a hold rule holds is asked: stage, requestId (the same across redirects), url,
 * method, headers as the page gave them (at the request stage the network has yet to add Cookie,
 * Origin, Referer and more), resourceType, frameId, mainFrame, worker, workerType, navigation, initiator, hasBody,
 * bodySize, body (base64, up to 64 KiB), bodyComplete; for the response, status,
 * statusText, responseHeaders, mimeType; timeoutMs, deadlineUnixMs. Each redirect's
 * destination is a request again; a redirect response is told, not held.
 * Unanswered or with its function replaced, a request goes on. A view closed ends its
 * requests. At most 1024 of a view's requests are held: past that, a further request
 * or response a hold rule matches fails with ERR_BLOCKED_BY_CLIENT, unasked, rather
 * than getting past the rule. A popup whose opener closed, when that opener had set a
 * filter, has no policy left: its requests and their responses fail the same way.
 */
/* Inside the network, each hop, with what it adds; not for WebSockets. Values are secrets
 * the page's own stages do not have.
 * sendHeaders: headers with the Cookie, before the transaction. Continue, with any valid
 *   headers (Cookie too), or block.
 * sent: observe only (mode must say so), kind "sent": headers as sent, with Authorization,
 *   Proxy-Authorization and Host, which a transaction adds after sendHeaders; again for each
 *   attempt, as after a 401; none when the HTTP cache answers. As DevTools' ExtraInfo.
 * headersReceived: status, statusText, responseHeaders with Set-Cookie, before the cookie store.
 *   Continue, with headers (a Set-Cookie left out is not stored), or block.
 * A rule at these fails closed: without a function, no policy left, too many held, no answer by
 * the deadline, or the function replaced, the request ends with ERR_BLOCKED_BY_CLIENT and
 * its outcome is told as state refused. A CORS preflight has its own requestId, preflight
 * true, resourceType "preflight", and no body. A rule must be set before a request starts to reach it.
 */
typedef struct zebkit_network_request zebkit_network_request;
typedef void (*zebkit_network_func)(zebkit_view*, zebkit_network_request*, const char *json, void *data);
ZEBKIT_API void zebkit_view_on_network(zebkit_view*, zebkit_network_func, void *data);
/* Reply once, JSON:
 * {action: "continue", headers: [{name, value}]}: all of them when given. Headers
 *   the network refuses a page are refused here. A response's change does not undo
 *   a cookie already stored, nor CORS, which the network has checked.
 * {action: "block", error: Playwright's name, default "blockedbyclient"}
 * {action: "redirect", url}: http or https. As with CDP's Fetch, a fetch's redirect:
 *   "manual" sees it as an opaque redirect and "error" fails, on the page's side.
 * {action: "fulfill", status, headers}, with @body of at most 64 MiB. As CDP's
 *   Fetch.fulfillRequest, CORS is not applied and Set-Cookie is not stored here.
 * Invalid replies return false with the reason and leave the request pending.
 * The original response's body cannot be changed in place; it can be read as it
 * passes (zebkit_view_on_network_body).
 */
ZEBKIT_API bool zebkit_network_respond(zebkit_network_request*, const char *reply_json, const void *body,
                                       size_t body_size, char **error);
/* Fulfills from a file: a reply of {status, headers}, and an absolute path, which is
 * the application's decision and never a page's. Read off the engine's thread, through
 * the pipe as the page reads, of any size; Range is not applied. Content-Type is
 * sniffed when absent and the page wants that. A file that cannot be read fails the
 * request with ERR_FILE_NOT_FOUND afterwards: this has answered by then, and
 * zebkit_view_on_network_event tells failed without a rule asking.
 */
ZEBKIT_API bool zebkit_network_respond_file(zebkit_network_request*, const char *reply_json, const char *path,
                                            char **error);
/* Fulfills with what the application writes, the response going at once: {status,
 * headers}; give a Content-Type, since no bytes are there to sniff. NULL with the
 * reason when invalid, and the request stays pending. No deadline applies from here: a
 * stream that is not ended leaves the page waiting. Engine thread only.
 */
typedef struct zebkit_network_stream zebkit_network_stream;
ZEBKIT_API zebkit_network_stream *zebkit_network_respond_stream(zebkit_network_request*, const char *reply_json,
                                                                char **error);
/* Takes all the bytes or none: false when they do not fit in what waits, at most 1 MiB
 * (a larger write never fits; write parts), or when the page has gone or stopped
 * reading. */
ZEBKIT_API bool zebkit_network_stream_write(zebkit_network_stream*, const void *bytes, size_t size);
/* Called once after a write that returned false, when that write fits, or when
 * nothing will (zebkit_network_stream_is_open says which). */
typedef void (*zebkit_network_stream_writable_func)(zebkit_network_stream*, void *data);
ZEBKIT_API void zebkit_network_stream_on_writable(zebkit_network_stream*, zebkit_network_stream_writable_func,
                                                  void *data);
ZEBKIT_API bool zebkit_network_stream_is_open(zebkit_network_stream*);
/* Ends the response after what waits, or as failed at once. Frees the stream, whose
 * pointer then does nothing; to be called also when the page has gone. */
ZEBKIT_API void zebkit_network_stream_end(zebkit_network_stream*, bool failed);
/* Default 30000 ms; accepted range 1..300000. */
ZEBKIT_API bool zebkit_view_set_network_timeout(zebkit_view*, uint32_t milliseconds);
ZEBKIT_API char *zebkit_network_request_info(zebkit_network_request*);
/* What an observe rule matches, holding nothing: kind request, response, redirect,
 * finished or failed (with error), and the fields above; dropped counts reports over
 * 4096 waiting. Outcomes of held requests too: state, reason. A WebSocket is told
 * request only. Bodies are not read.
 */
typedef void (*zebkit_network_event_func)(zebkit_view*, const char *json, void *data);
ZEBKIT_API void zebkit_view_on_network_event(zebkit_view*, zebkit_network_event_func, void *data);
/* The body of a response a rule with body: true matches, told as it passes to the page
 * and never kept: @bytes for this call only, at most 64 KiB at a time, with @request_id
 * the requestId of its events; such a rule tells them as an observe rule at the
 * response stage does, and holds nothing. The next chunk is read when the page has
 * taken this one, so that a page that does not read waits as it would without this.
 * Each response ends with one call of end true: truncated true once 64 MiB have been
 * told, after which the page's stream goes on untold; truncated false at the end of the
 * body, or when the network failed or the page went, which the following failed event
 * (with error) tells from finished. Not told to a function set after the response
 * started, nor after this function is replaced or the view has closed. Without a
 * function, nothing passes through here. NULL removes it.
 */
typedef void (*zebkit_network_body_func)(zebkit_view*, const char *request_id, const void *bytes, size_t size,
                                         bool end, bool truncated, void *data);
ZEBKIT_API void zebkit_view_on_network_body(zebkit_view*, zebkit_network_body_func, void *data);

#ifdef __cplusplus
}
#endif
#endif
