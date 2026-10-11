/*
 * What goes through the view and not through the inspector, as far as it is
 * the same on every platform: the bookkeeping of a navigation, what a load
 * event or a crash means for the clients, dialogs, and the order of the
 * answers to input. The platform files (CDPView*.{cpp,mm}) hook up the
 * view's notifications and call in here, and do what this asks of a view
 * through the functions in CDPPlatform.h.
 */

#include "config.h"
#include "CDPInternal.h"

#include "WebPageProxy.h"
#include <wtf/URL.h>
#include <wtf/text/MakeString.h>

namespace Zebkit::CDP {

// MARK: Loads

void PageTarget::confirmNavigationCommitted(unsigned generation, unsigned attemptsLeft)
{
    auto params = JSON::Object::create();
    params->setString("objectGroup"_s, "zebkit-sync"_s);
    wip("Runtime.releaseObjectGroup"_s, WTF::move(params), [this, weakThis = WeakPtr { *this }, generation, attemptsLeft](JSON::Object*, const String& error) {
        if (!weakThis || generation != m_navigationGeneration || !m_pendingNavigation)
            return;
        if (!error.isNull()) {
            // Asked of a process the page was just leaving; ask the new one.
            if (attemptsLeft && !m_crashed && m_inspectorConnected)
                confirmNavigationCommitted(generation, attemptsLeft - 1);
            return;
        }
        if (!m_pendingErrorText.isNull()) {
            m_errorPageExpected = false;
            finishNavigation({ }, std::exchange(m_pendingErrorText, String()));
            return;
        }
        auto frame = m_frames.find(mainFrameID());
        finishNavigation(frame == m_frames.end() ? String() : uniqueID(frame->value.loaderID), { });
    });
}

void PageTarget::infoChanged()
{
    if (m_view)
        Server::singleton().targetInfoChanged(*this);
}

// The address of the main frame changed. If no new document came with it,
// the document changed its own address: a fragment, or the history API.
// WebKit's protocol has no event for that, so the document is asked. A new
// document has reported its address by the time it answers; one that is
// still on its way leaves the old one answering with the old address.
void PageTarget::addressChanged()
{
    if (!m_view || !m_inspectorConnected || m_crashed)
        return;
    evaluateValue("location.href"_s, [this, weakThis = WeakPtr { *this }](RefPtr<JSON::Value>&& value) {
        if (weakThis && value)
            navigatedWithinDocument(value->asString());
    });
}

void PageTarget::navigatedWithinDocument(const String& url)
{
    auto frame = m_frames.find(mainFrameID());
    if (frame == m_frames.end() || url.isEmpty() || frame->value.url == url)
        return;
    auto withoutFragment = [](const String& address) {
        auto hash = address.find('#');
        return hash == notFound ? address : address.left(hash);
    };
    bool isFragment = withoutFragment(frame->value.url) == withoutFragment(url);
    frame->value.url = url;
    forEachSession([&](Session& session) {
        if (!session.pageEnabled)
            return;
        auto event = JSON::Object::create();
        event->setString("frameId"_s, m_targetID);
        event->setString("url"_s, url);
        event->setString("navigationType"_s, isFragment ? "fragment"_s : "historyApi"_s);
        session.event("Page.navigatedWithinDocument"_s, WTF::move(event));
    });
}

void PageTarget::loadChanged(LoadEvent loadEvent)
{
    if (loadEvent == LoadEvent::Started && std::exchange(m_crashed, false)) {
        // The load starts a new process, which knows nothing of this client.
        m_reconnectAttempts = 0;
        reconnectInspector();
    }
    if (loadEvent == LoadEvent::Committed && m_pendingNavigation && m_inspectorConnected) {
        // The page reports the commit itself, with the loader's id, unless
        // it committed before its process had been told to report anything.
        // Once the page has answered and the navigation is still waiting,
        // that is what happened.
        confirmNavigationCommitted(m_navigationGeneration, 5);
    }
    if (loadEvent != LoadEvent::Started && loadEvent != LoadEvent::Finished)
        return;
    bool started = loadEvent == LoadEvent::Started;
    forEachSession([&](Session& session) {
        if (!session.pageEnabled)
            return;
        auto event = JSON::Object::create();
        event->setString("frameId"_s, m_targetID);
        session.event(started ? "Page.frameStartedLoading"_s : "Page.frameStoppedLoading"_s, WTF::move(event));
    });
}

// A load failed, and not because a newer one replaced it. errorText is
// Chrome's name for the reason.
void PageTarget::loadFailed(ASCIILiteral errorText)
{
    if (!engineShowsErrorPages) {
        finishNavigation({ }, errorText);
        return;
    }
    m_errorPageExpected = true;
    if (!m_pendingNavigation)
        return;
    // WebKit now loads its error page. The client is told of the failure
    // once that page has committed (handleInnerEvent), so that the commit
    // is not taken for part of whatever the client does next. An
    // application may suppress the error page; do not wait for long.
    m_pendingErrorText = errorText;
    unsigned generation = ++m_navigationGeneration;
    runAfter(500, [this, weakThis = WeakPtr { *this }, generation] {
        if (weakThis && generation == m_navigationGeneration && !m_pendingErrorText.isNull())
            finishNavigation({ }, std::exchange(m_pendingErrorText, String()));
    });
}

// A load that was replaced by a newer one is not the newer one failing.
void PageTarget::loadCancelled()
{
    m_errorPageExpected = false;
}

void PageTarget::processTerminated()
{
    // WebKit keeps the same inspector target for the page and gives it a new
    // process on the next load, so nothing here says the old one is gone.
    // Whatever was waiting for it will never be answered.
    m_crashed = true;
    m_queuedCommands.clear();
    failPendingCommands("Target crashed"_s);
    resetDocumentState();
    dismissDialog();
    forEachSession([](Session& session) {
        session.event("Inspector.targetCrashed"_s, JSON::Object::create());
    });
    finishNavigation({ }, "net::ERR_ABORTED"_s);
}

void PageTarget::pageCrash(Session&, JSON::Object&, Reply&& reply)
{
    replyOK(reply);
    terminateWebProcess(m_view);
}

// MARK: Navigation

void PageTarget::finishNavigation(const String& loaderID, const String& errorText)
{
    if (!m_pendingNavigation)
        return;
    auto reply = std::exchange(m_pendingNavigation, nullptr);
    auto result = JSON::Object::create();
    result->setString("frameId"_s, m_targetID);
    if (!loaderID.isEmpty())
        result->setString("loaderId"_s, loaderID);
    if (!errorText.isNull())
        result->setString("errorText"_s, errorText);
    reply(WTF::move(result), { });
}

void PageTarget::pageNavigate(Session&, JSON::Object& params, Reply&& reply)
{
    auto url = params.getString("url"_s);
    if (auto frameID = params.getString("frameId"_s); !frameID.isEmpty() && frameID != m_targetID) {
        replyError(reply, "Only the main frame can be navigated"_s);
        return;
    }
    // What cannot be an address is refused, as in Chrome, and not loaded.
    if (url.isEmpty() || !URL { url }.isValid()) {
        replyError(reply, "Cannot navigate to invalid URL"_s);
        return;
    }
    finishNavigation({ }, "net::ERR_ABORTED"_s);
    pendingInitialURL = String();
    m_pendingErrorText = String();
    ++m_navigationGeneration;

    auto withoutFragment = [](const String& address) {
        auto hash = address.find('#');
        return hash == notFound ? address : address.left(hash);
    };
    auto current = viewURI(m_view);
    bool sameDocument = url.contains('#') && !current.isEmpty() && withoutFragment(url) == withoutFragment(current);
    if (sameDocument || url.startsWith("javascript:"_s)) {
        // Neither commits a new document, so there is nothing to wait for.
        loadURI(m_view, url);
        auto result = JSON::Object::create();
        result->setString("frameId"_s, m_targetID);
        reply(WTF::move(result), { });
        if (sameDocument)
            navigatedWithinDocument(url);
        return;
    }

    // Answered when the document commits (handleInnerEvent) or the load
    // fails (loadFailed), as Chrome does.
    m_pendingNavigation = WTF::move(reply);
    loadURI(m_view, url);
}

void PageTarget::pageStopLoading(Session&, JSON::Object&, Reply&& reply)
{
    stopLoading(m_view);
    // WebKit reports this like a load a newer one replaced, which is not
    // passed on; the navigation the client is waiting for is over all the same.
    ++m_navigationGeneration;
    m_pendingErrorText = String();
    finishNavigation({ }, "net::ERR_ABORTED"_s);
    replyOK(reply);
}

void PageTarget::pageClose(Session&, JSON::Object&, Reply&& reply)
{
    replyOK(reply);
    close();
}

// MARK: Dialogs

void PageTarget::dismissDialog()
{
    for (auto& session : m_sessions)
        session->sawDialog = false;
    closeDialog();
}

bool PageTarget::acceptsDialog() const
{
    bool hasListener = false;
    for (auto& session : m_sessions)
        hasListener |= session->pageEnabled;
    return hasListener && !hasOpenDialog();
}

// The platform holds a dialog open; tell the clients. defaultPrompt is null
// unless the dialog is a prompt.
void PageTarget::announceDialog(ASCIILiteral type, const String& message, const String& defaultPrompt)
{
    m_dialogDefaultPrompt = defaultPrompt;
    forEachSession([&](Session& session) {
        if (!session.pageEnabled)
            return;
        session.sawDialog = true;
        auto event = JSON::Object::create();
        event->setString("url"_s, viewURI(m_view));
        // WebKit does not say which frame opened it.
        event->setString("frameId"_s, m_targetID);
        event->setString("message"_s, message);
        event->setString("type"_s, type);
        event->setBoolean("hasBrowserHandler"_s, false);
        if (!defaultPrompt.isNull())
            event->setString("defaultPrompt"_s, defaultPrompt);
        session.event("Page.javascriptDialogOpening"_s, WTF::move(event));
    });
}

void PageTarget::pageHandleJavaScriptDialog(Session&, JSON::Object& params, Reply&& reply)
{
    if (!hasOpenDialog()) {
        replyInvalid(reply, "No dialog is showing"_s);
        return;
    }
    bool accept = params.getBoolean("accept"_s).value_or(false);
    auto promptText = params.getString("promptText"_s);
    // Accepting a prompt without saying what to answer gives what it proposed.
    if (accept && promptText.isNull())
        promptText = std::exchange(m_dialogDefaultPrompt, String());
    answerDialog(accept, promptText);
    for (auto& session : m_sessions)
        session->sawDialog = false;

    forEachSession([&](Session& session) {
        if (!session.pageEnabled)
            return;
        auto event = JSON::Object::create();
        event->setString("frameId"_s, m_targetID);
        event->setBoolean("result"_s, accept);
        event->setString("userInput"_s, promptText.isNull() ? emptyString() : promptText);
        session.event("Page.javascriptDialogClosed"_s, WTF::move(event));
    });
    replyOK(reply);
}

// MARK: Window and input

void PageTarget::waitForViewportWidth(int width, unsigned attemptsLeft, Reply&& reply)
{
    evaluateValue("innerWidth"_s, [this, weakThis = WeakPtr { *this }, width, attemptsLeft, reply = WTF::move(reply)](RefPtr<JSON::Value>&& value) mutable {
        if (!weakThis || !attemptsLeft || (value && value->asInteger().value_or(0) == width)) {
            replyOK(reply);
            return;
        }
        runAfter(25, [this, weakThis, width, attemptsLeft, reply = WTF::move(reply)]() mutable {
            if (weakThis)
                waitForViewportWidth(width, attemptsLeft - 1, WTF::move(reply));
            else
                replyOK(reply);
        });
    });
}

// Events are handed to the page's process one at a time, the next when the
// previous one has been handled. A command is answered when nothing is left
// in that queue, so that script the client runs next sees its effect.
//
// The answers go out in the order the commands came in. Clients send a
// press and its release without waiting in between and keep track of which
// buttons are down from the order of the answers.
void PageTarget::whenInputProcessed(Reply&& reply)
{
    m_inputReplies.append(WTF::move(reply));
    if (!std::exchange(m_inputPollActive, true))
        pollInput(1500);
}

void PageTarget::pollInput(unsigned attemptsLeft)
{
    if (!m_view) {
        m_inputPollActive = false;
        for (auto& reply : std::exchange(m_inputReplies, { }))
            replyError(reply, "Target closed"_s);
        return;
    }
    auto& page = pageProxy(m_view);
    bool busy = page.isProcessingMouseEvents() || page.isProcessingKeyboardEvents() || page.isProcessingWheelEvents();
    if (!busy || !attemptsLeft) {
        // What the events set off in the page (a click handler, a focus
        // change) has run by the time the page answers anything else.
        m_inputPollActive = false;
        afterPageCatchesUp([replies = std::exchange(m_inputReplies, { })]() mutable {
            for (auto& reply : replies)
                replyOK(reply);
        });
        return;
    }
    runAfter(2, [this, protectedThis = Ref { *this }, attemptsLeft] {
        pollInput(attemptsLeft - 1);
    });
}

char32_t singleCodePoint(const String& text)
{
    if (text.isEmpty())
        return 0;
    char32_t first = text[0];
    if (U16_IS_LEAD(first) && text.length() > 1 && U16_IS_TRAIL(text[1]))
        first = U16_GET_SUPPLEMENTARY(first, text[1]);
    unsigned length = first > 0xffff ? 2 : 1;
    return text.length() == length ? first : 0;
}

} // namespace Zebkit::CDP
