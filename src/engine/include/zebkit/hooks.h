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

#ifdef __cplusplus
}
#endif
#endif
