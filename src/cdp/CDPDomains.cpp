/*
 * CDP commands addressed to a page, translated to WebKit's protocol or
 * answered with script run in the page.
 */

#include "config.h"
#include "CDPInternal.h"

#include <wpe/webkit.h>
#include <wtf/CallbackAggregator.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace Zebkit::CDP {


// Ids this bridge hands out for DOM.describeNode are kept apart from the
// node ids of WebKit's DOM domain, which double as backendNodeId.
static constexpr int backendNodeBase = 1 << 28;

static String quote(const String& string)
{
    return JSON::Value::create(string)->toJSONString();
}

// Messages client libraries look for to tell "the page navigated under
// you" from a real failure.
static String errorToCDP(const String& error)
{
    if (error.containsIgnoringASCIICase("injected script"_s) || error.containsIgnoringASCIICase("injectedscript"_s) || error.containsIgnoringASCIICase("contextid"_s))
        return "Cannot find context with specified id"_s;
    if (error.contains("navigated or closed"_s))
        return "Execution context was destroyed."_s;
    return error;
}

// WebKit's object previews carry a few fields of their own and leave out
// one that CDP requires.
static void previewToCDP(JSON::Object& preview)
{
    preview.remove("lossless"_s);
    // "Array" in WebKit is "Array(2)" in Chrome.
    auto subtype = preview.getString("subtype"_s);
    if (auto size = preview.getInteger("size"_s); size && (subtype == "array"_s || subtype == "map"_s || subtype == "set"_s))
        preview.setString("description"_s, makeString(preview.getString("description"_s), '(', *size, ')'));
    preview.remove("size"_s);
    if (!preview.getValue("overflow"_s))
        preview.setBoolean("overflow"_s, false);
    RefPtr properties = preview.getArray("properties"_s);
    if (!properties)
        preview.setArray("properties"_s, JSON::Array::create());
    else {
        for (auto& item : *properties) {
            RefPtr property = item->asObject();
            if (!property)
                continue;
            property->remove("internal"_s);
            property->remove("isPrivate"_s);
            if (RefPtr nested = property->getObject("valuePreview"_s)) {
                previewToCDP(*nested);
                if (!property->getValue("value"_s))
                    property->setString("value"_s, nested->getString("description"_s));
            }
        }
    }
    if (RefPtr entries = preview.getArray("entries"_s)) {
        for (auto& item : *entries) {
            RefPtr entry = item->asObject();
            if (!entry)
                continue;
            for (auto key : { "key"_s, "value"_s }) {
                if (RefPtr nested = entry->getObject(key))
                    previewToCDP(*nested);
            }
        }
    }
}

// WebKit describes an element as its start tag and a text node with its
// text; Chrome as a selector ("div#id.class") and "#text".
static String nodeDescriptionToCDP(const String& description)
{
    if (description.startsWith('#')) {
        auto space = description.find(' ');
        return space == notFound ? description : description.left(space);
    }
    if (description.length() < 3 || !description.startsWith('<') || !description.endsWith('>'))
        return description;
    auto tag = description.substring(1, description.length() - 2);
    auto space = tag.find(' ');
    if (space == notFound)
        return tag;
    auto attribute = [&](ASCIILiteral name) -> String {
        auto start = tag.find(makeString(' ', name, "=\""_s));
        if (start == notFound)
            return { };
        start += name.length() + 3;
        auto end = tag.find('"', start);
        return end == notFound ? String() : tag.substring(start, end - start);
    };
    StringBuilder result;
    result.append(tag.left(space));
    if (auto id = attribute("id"_s); !id.isEmpty())
        result.append('#', id);
    for (auto name : attribute("class"_s).split(' '))
        result.append('.', name);
    return result.toString();
}

Ref<JSON::Object> remoteObjectToCDP(JSON::Object& object)
{
    auto type = object.getString("type"_s);
    auto subtype = object.getString("subtype"_s);
    auto description = object.getString("description"_s);
    RefPtr value = object.getValue("value"_s);
    // CDP has no JSON for these and carries them as text.
    bool isNegativeZero = type == "number"_s && description == "-0"_s;
    if (type == "bigint"_s || isNegativeZero || (type == "number"_s && (!value || value->isNull()) && !description.isNull())) {
        object.remove("value"_s);
        object.setString("unserializableValue"_s, description);
    }
    if (subtype == "class"_s)
        object.remove("subtype"_s);
    // Chrome describes what has no value to show; these have one.
    if (type == "string"_s || type == "boolean"_s || type == "undefined"_s)
        object.remove("description"_s);
    auto className = object.getString("className"_s);
    if (subtype == "node"_s)
        object.setString("description"_s, nodeDescriptionToCDP(description));
    // "Array" in WebKit is "Array(2)" in Chrome, and so on.
    if (subtype == "array"_s && className != "Array"_s && className.endsWith("Array"_s)) {
        subtype = "typedarray"_s;
        object.setString("subtype"_s, subtype);
    }
    if ((subtype == "array"_s || subtype == "typedarray"_s || subtype == "map"_s || subtype == "set"_s) && description == className) {
        if (auto size = object.getInteger("size"_s))
            object.setString("description"_s, makeString(className, '(', *size, ')'));
    }
    if (type == "object"_s && subtype.isNull() && className == "Promise"_s)
        object.setString("subtype"_s, "promise"_s);
    if (type == "function"_s)
        object.setString("className"_s, "Function"_s);
    if (type == "symbol"_s)
        object.remove("className"_s);
    object.remove("size"_s);
    object.remove("classPrototype"_s);
    if (RefPtr preview = object.getObject("preview"_s))
        previewToCDP(*preview);
    return object;
}

static Ref<JSON::Object> exceptionDetails(JSON::Object& exception, int id)
{
    auto details = JSON::Object::create();
    details->setInteger("exceptionId"_s, id);
    details->setString("text"_s, "Uncaught"_s);
    details->setInteger("lineNumber"_s, 0);
    details->setInteger("columnNumber"_s, 0);
    details->setObject("exception"_s, remoteObjectToCDP(exception));
    return details;
}

static int contextOfObject(const String& objectID);

void PageTarget::dispatch(Session& session, const String& method, JSON::Object& params, Reply&& reply)
{
    if (!m_view) {
        replyError(reply, "Target closed"_s);
        return;
    }
    // What WebKit would make of an object id that is not one of its own
    // depends on the command; Chrome has one answer.
    for (auto key : { "objectId"_s, "promiseObjectId"_s }) {
        if (auto objectID = params.getString(key); !objectID.isNull() && contextOfObject(objectID) <= 0) {
            replyError(reply, "Invalid remote object id"_s);
            return;
        }
    }
    auto& table = handlers();
    if (auto iterator = table.find(method); iterator != table.end()) {
        (this->*iterator->value)(session, params, WTF::move(reply));
        return;
    }
    if (method.startsWith("DOM."_s)) {
        domPassThrough(method, params, WTF::move(reply));
        return;
    }
    replyError(reply, makeString('\'', method, "' wasn't found"_s));
}

void PageTarget::acknowledge(Session&, JSON::Object&, Reply&& reply)
{
    replyOK(reply);
}

// MARK: Shared helpers

// Runs when everything already sent to the page has been processed and its
// events have arrived here.
void PageTarget::afterPageCatchesUp(Function<void()>&& function)
{
    auto params = JSON::Object::create();
    params->setString("objectGroup"_s, "zebkit-sync"_s);
    wip("Runtime.releaseObjectGroup"_s, WTF::move(params), [function = WTF::move(function)](JSON::Object*, const String&) {
        function();
    });
}

void PageTarget::globalObjectFor(int innerContextID, Function<void(const String&, const String&)>&& completion)
{
    if (auto iterator = m_globalObjects.find(innerContextID); iterator != m_globalObjects.end()) {
        completion(iterator->value, { });
        return;
    }
    auto params = JSON::Object::create();
    params->setString("expression"_s, "globalThis"_s);
    if (innerContextID)
        params->setInteger("contextId"_s, innerContextID);
    wip("Runtime.evaluate"_s, WTF::move(params), [this, weakThis = WeakPtr { *this }, innerContextID, completion = WTF::move(completion)](JSON::Object* result, const String& error) {
        RefPtr object = result ? result->getObject("result"_s) : nullptr;
        auto objectID = object ? object->getString("objectId"_s) : String();
        if (!weakThis || objectID.isNull()) {
            completion({ }, error.isNull() ? "Cannot find context with specified id"_s : errorToCDP(error));
            return;
        }
        if (m_contexts.contains(innerContextID))
            m_globalObjects.set(innerContextID, objectID);
        completion(objectID, { });
    });
}

void PageTarget::callOn(const String& objectID, const String& function, RefPtr<JSON::Array>&& arguments, bool returnByValue, WIPReply&& reply, bool userGesture)
{
    auto params = JSON::Object::create();
    params->setString("objectId"_s, objectID);
    params->setString("functionDeclaration"_s, function);
    if (arguments)
        params->setArray("arguments"_s, arguments.releaseNonNull());
    params->setBoolean("returnByValue"_s, returnByValue);
    if (userGesture)
        params->setBoolean("emulateUserGesture"_s, true);
    wip("Runtime.callFunctionOn"_s, WTF::move(params), WTF::move(reply));
}

// Calls a function on a node and hands back its JSON result. A thrown
// exception, or a null result, becomes the given error.
static constexpr auto notANodeMarker = "zebkit-not-a-node"_s;

void PageTarget::callOnNode(JSON::Object& params, const String& function, ASCIILiteral failure, Function<void(RefPtr<JSON::Value>&&, const String& objectID, const String& error)>&& completion)
{
    resolveNode(params, [this, weakThis = WeakPtr { *this }, function, failure, completion = WTF::move(completion)](const String& objectID, const String& error) mutable {
        if (!weakThis || objectID.isNull()) {
            completion(nullptr, { }, error.isNull() ? "Could not find node with given id"_s : error);
            return;
        }
        auto guarded = makeString("function(){if(!(this instanceof Node))return'"_s, notANodeMarker, "';return("_s, function, ").call(this)}"_s);
        callOn(objectID, guarded, nullptr, true, [objectID, failure, completion = WTF::move(completion)](JSON::Object* result, const String& error) {
            RefPtr object = result ? result->getObject("result"_s) : nullptr;
            RefPtr value = object ? object->getValue("value"_s) : nullptr;
            if (!error.isNull()) {
                completion(nullptr, objectID, errorToCDP(error));
                return;
            }
            if (value && value->asString() == notANodeMarker) {
                completion(nullptr, objectID, "Object id doesn't reference a Node"_s);
                return;
            }
            if (result->getBoolean("wasThrown"_s).value_or(false) || !value || value->isNull()) {
                completion(nullptr, objectID, failure);
                return;
            }
            completion(WTF::move(value), objectID, { });
        });
    });
}

void PageTarget::evaluateValue(const String& expression, Function<void(RefPtr<JSON::Value>&&)>&& completion)
{
    auto params = JSON::Object::create();
    params->setString("expression"_s, expression);
    params->setBoolean("returnByValue"_s, true);
    wip("Runtime.evaluate"_s, WTF::move(params), [completion = WTF::move(completion)](JSON::Object* result, const String&) {
        RefPtr object = result ? result->getObject("result"_s) : nullptr;
        completion(object ? object->getValue("value"_s) : nullptr);
    });
}

// Makes sure WebKit's DOM domain has a document, which it needs before it
// will hand out node ids.
void PageTarget::ensureDocument(Function<void()>&& completion)
{
    if (m_documentNodeID) {
        completion();
        return;
    }
    wip("DOM.getDocument"_s, nullptr, [this, weakThis = WeakPtr { *this }, completion = WTF::move(completion)](JSON::Object* result, const String&) {
        if (!weakThis)
            return;
        if (RefPtr root = result ? result->getObject("root"_s) : nullptr) {
            scanNode(*root);
            m_documentNodeID = root->getInteger("nodeId"_s).value_or(0);
        }
        completion();
    });
}

// A second handle to the same object, which stays valid when the client
// releases the one it gave.
void PageTarget::registerBackendNode(const String& objectID, Function<void(int)>&& completion)
{
    callOn(objectID, "function(){return this}"_s, nullptr, false, [this, weakThis = WeakPtr { *this }, completion = WTF::move(completion)](JSON::Object* result, const String&) {
        RefPtr object = result ? result->getObject("result"_s) : nullptr;
        auto ownID = object ? object->getString("objectId"_s) : String();
        if (!weakThis || ownID.isNull()) {
            completion(0);
            return;
        }
        int id = backendNodeBase + ++m_nextBackendNodeID;
        m_backendNodes.set(id, ownID);
        // Each handle keeps its node alive in the page.
        if (auto oldest = m_backendNodes.take(id - maxBackendNodes); !oldest.isNull()) {
            auto release = JSON::Object::create();
            release->setString("objectId"_s, oldest);
            wip("Runtime.releaseObject"_s, WTF::move(release));
        }
        completion(id);
    });
}

// Finds the object a DOM command is about, whichever of objectId,
// backendNodeId and nodeId names it.
void PageTarget::resolveNode(JSON::Object& params, Function<void(const String&, const String&)>&& completion)
{
    if (auto objectID = params.getString("objectId"_s); !objectID.isNull()) {
        completion(objectID, { });
        return;
    }
    bool byBackendID = !!params.getValue("backendNodeId"_s);
    auto notFound = byBackendID ? "No node found for given backend id"_s : "Could not find node with given id"_s;
    int nodeID = params.getInteger("backendNodeId"_s).value_or(0);
    if (nodeID >= backendNodeBase) {
        auto iterator = m_backendNodes.find(nodeID);
        if (iterator == m_backendNodes.end())
            completion({ }, notFound);
        else
            completion(iterator->value, { });
        return;
    }
    if (!byBackendID)
        nodeID = params.getInteger("nodeId"_s).value_or(0);
    if (!byBackendID && !params.getValue("nodeId"_s)) {
        completion({ }, "Either nodeId, backendNodeId or objectId must be specified"_s);
        return;
    }
    if (nodeID <= 0) {
        completion({ }, notFound);
        return;
    }
    auto resolveParams = JSON::Object::create();
    resolveParams->setInteger("nodeId"_s, nodeID);
    wip("DOM.resolveNode"_s, WTF::move(resolveParams), [notFound, completion = WTF::move(completion)](JSON::Object* result, const String&) {
        RefPtr object = result ? result->getObject("object"_s) : nullptr;
        auto objectID = object ? object->getString("objectId"_s) : String();
        if (objectID.isNull())
            completion({ }, notFound);
        else
            completion(objectID, { });
    });
}

static int contextOfObject(const String& objectID)
{
    RefPtr value = JSON::Value::parseJSON(objectID);
    RefPtr object = value ? value->asObject() : nullptr;
    return object ? object->getInteger("injectedScriptId"_s).value_or(0) : 0;
}

// The element that holds a frame, as an object in the parent frame.
void PageTarget::findFrameOwner(const String& innerFrameID, Function<void(const String& objectID)>&& completion)
{
    auto lookUp = [this, innerFrameID]() -> int {
        for (auto& entry : m_frameOwnerNodes) {
            if (entry.value == innerFrameID)
                return entry.key;
        }
        return 0;
    };
    auto resolve = [this, weakThis = WeakPtr { *this }](int nodeID, Function<void(const String&)>&& completion) {
        auto params = JSON::Object::create();
        params->setInteger("nodeId"_s, nodeID);
        wip("DOM.resolveNode"_s, WTF::move(params), [completion = WTF::move(completion)](JSON::Object* result, const String&) {
            RefPtr object = result ? result->getObject("object"_s) : nullptr;
            completion(object ? object->getString("objectId"_s) : String());
        });
    };

    auto frame = m_frames.find(innerFrameID);
    if (frame == m_frames.end() || frame->value.parentID.isEmpty()) {
        completion({ });
        return;
    }
    int parentContext = contextForFrame(frame->value.parentID);

    ensureDocument([this, weakThis = WeakPtr { *this }, lookUp, resolve, parentContext, completion = WTF::move(completion)]() mutable {
        if (!weakThis)
            return;
        if (int nodeID = lookUp()) {
            resolve(nodeID, WTF::move(completion));
            return;
        }
        // WebKit has not described the element yet. Asking for the node id
        // of each frame element of the parent document makes it do so.
        auto params = JSON::Object::create();
        params->setString("expression"_s, "Array.from(document.querySelectorAll('iframe,frame,object,embed'))"_s);
        if (parentContext)
            params->setInteger("contextId"_s, parentContext);
        wip("Runtime.evaluate"_s, WTF::move(params), [this, weakThis, lookUp, resolve, completion = WTF::move(completion)](JSON::Object* result, const String&) mutable {
            RefPtr array = result ? result->getObject("result"_s) : nullptr;
            auto arrayID = array ? array->getString("objectId"_s) : String();
            if (!weakThis || arrayID.isNull()) {
                completion({ });
                return;
            }
            auto params = JSON::Object::create();
            params->setString("objectId"_s, arrayID);
            params->setBoolean("ownProperties"_s, true);
            wip("Runtime.getProperties"_s, WTF::move(params), [this, weakThis, lookUp, resolve, completion = WTF::move(completion)](JSON::Object* result, const String&) mutable {
                RefPtr properties = result ? result->getArray("properties"_s) : nullptr;
                if (!weakThis || !properties) {
                    completion({ });
                    return;
                }
                auto sharedCompletion = Box<Function<void(const String&)>>::create(WTF::move(completion));
                auto aggregator = CallbackAggregator::create([weakThis, lookUp, resolve, sharedCompletion]() mutable {
                    if (!weakThis)
                        return;
                    if (int nodeID = lookUp())
                        resolve(nodeID, WTF::move(*sharedCompletion));
                    else
                        (*sharedCompletion)({ });
                });
                for (auto& property : *properties) {
                    RefPtr object = property->asObject();
                    RefPtr value = object ? object->getObject("value"_s) : nullptr;
                    auto elementID = value ? value->getString("objectId"_s) : String();
                    if (elementID.isNull() || value->getString("subtype"_s) != "node"_s)
                        continue;
                    auto requestParams = JSON::Object::create();
                    requestParams->setString("objectId"_s, elementID);
                    wip("DOM.requestNode"_s, WTF::move(requestParams), [aggregator](JSON::Object*, const String&) { });
                }
            });
        });
    });
}

static constexpr auto frameContentBoxFunction = "function(){const r=this.getBoundingClientRect(),s=getComputedStyle(this),n=v=>parseFloat(v)||0;return [r.x+n(s.borderLeftWidth)+n(s.paddingLeft),r.y+n(s.borderTopWidth)+n(s.paddingTop)]}"_s;

// Where a frame's viewport lies in the page's viewport. Script inside a
// frame measures against its own viewport, and cannot see out of the frame
// when the parent is another site, so the offsets are added up from here.
void PageTarget::frameOffset(const String& innerFrameID, Function<void(double x, double y)>&& completion)
{
    auto frame = m_frames.find(innerFrameID);
    if (innerFrameID.isEmpty() || frame == m_frames.end() || frame->value.parentID.isEmpty()) {
        completion(0, 0);
        return;
    }
    auto parentID = frame->value.parentID;
    findFrameOwner(innerFrameID, [this, weakThis = WeakPtr { *this }, parentID, completion = WTF::move(completion)](const String& ownerID) mutable {
        if (!weakThis || ownerID.isNull()) {
            completion(0, 0);
            return;
        }
        callOn(ownerID, frameContentBoxFunction, nullptr, true, [this, weakThis, parentID, completion = WTF::move(completion)](JSON::Object* result, const String&) mutable {
            RefPtr object = result ? result->getObject("result"_s) : nullptr;
            RefPtr origin = object ? object->getArray("value"_s) : nullptr;
            if (!weakThis || !origin || origin->length() != 2) {
                completion(0, 0);
                return;
            }
            double x = origin->get(0)->asDouble().value_or(0);
            double y = origin->get(1)->asDouble().value_or(0);
            frameOffset(parentID, [x, y, completion = WTF::move(completion)](double parentX, double parentY) {
                completion(x + parentX, y + parentY);
            });
        });
    });
}

void PageTarget::frameOffsetOfObject(const String& objectID, Function<void(double x, double y)>&& completion)
{
    auto context = m_contexts.find(contextOfObject(objectID));
    if (context == m_contexts.end() || context->value.frameID == mainFrameID()) {
        completion(0, 0);
        return;
    }
    frameOffset(context->value.frameID, WTF::move(completion));
}

static Ref<JSON::Array> offsetQuad(JSON::Array& quad, double x, double y)
{
    auto result = JSON::Array::create();
    for (unsigned index = 0; index < quad.length(); ++index)
        result->pushDouble(quad.get(index)->asDouble().value_or(0) + (index % 2 ? y : x));
    return result;
}

// MARK: Page

void PageTarget::pageEnable(Session& session, JSON::Object&, Reply&& reply)
{
    session.pageEnabled = true;
    replyOK(reply);
}

void PageTarget::pageDisable(Session& session, JSON::Object&, Reply&& reply)
{
    session.pageEnabled = false;
    replyOK(reply);
}

void PageTarget::pageSetLifecycleEventsEnabled(Session& session, JSON::Object& params, Reply&& reply)
{
    bool enabled = params.getBoolean("enabled"_s).value_or(false);
    bool wasEnabled = std::exchange(session.lifecycleEnabled, enabled);
    replyOK(reply);
    if (!enabled || wasEnabled || !session.pageEnabled)
        return;
    // As in Chrome, a client that starts listening is told how far each
    // frame has got. Not if the page has moved on by the time that is known:
    // the new document reports itself.
    auto isCurrent = [this, navigation = m_navigationGeneration, document = m_networkIdleGeneration] {
        return navigation == m_navigationGeneration && document == m_networkIdleGeneration;
    };
    wip("Page.getResourceTree"_s, nullptr, [this, weakThis = WeakPtr { *this }, isCurrent, protectedSession = Ref { session }](JSON::Object* result, const String&) {
        if (!weakThis || !isCurrent())
            return;
        if (RefPtr tree = result ? result->getObject("frameTree"_s) : nullptr)
            frameTreeToCDP(*tree);
        evaluateValue("document.readyState"_s, [this, weakThis, isCurrent, protectedSession](RefPtr<JSON::Value>&& value) {
            if (!weakThis || !isCurrent() || !protectedSession->lifecycleEnabled)
                return;
            auto state = value ? value->asString() : String();
            auto main = mainFrameID();
            for (auto& frame : m_frames) {
                bool isMain = frame.key == main;
                Vector<ASCIILiteral> names { "commit"_s };
                if (!isMain || state == "interactive"_s || state == "complete"_s)
                    names.append("DOMContentLoaded"_s);
                if (!isMain || state == "complete"_s)
                    names.append("load"_s);
                if (isMain && state == "complete"_s && m_inflightRequests.size() <= 2)
                    names.append("networkAlmostIdle"_s);
                if (isMain && state == "complete"_s && m_inflightRequests.isEmpty())
                    names.append("networkIdle"_s);
                for (auto name : names) {
                    auto event = JSON::Object::create();
                    event->setString("frameId"_s, toCDPFrameID(frame.key));
                    event->setString("loaderId"_s, uniqueID(frame.value.loaderID));
                    event->setString("name"_s, name);
                    event->setDouble("timestamp"_s, monotonicSeconds());
                    protectedSession->event("Page.lifecycleEvent"_s, WTF::move(event));
                }
            }
        });
    });
}

void PageTarget::pageGetFrameTree(Session&, JSON::Object&, Reply&& reply)
{
    wip("Page.getResourceTree"_s, nullptr, [this, weakThis = WeakPtr { *this }, reply = WTF::move(reply)](JSON::Object* result, const String& error) mutable {
        RefPtr tree = result ? result->getObject("frameTree"_s) : nullptr;
        if (!weakThis || !tree) {
            replyError(reply, error.isNull() ? "Target closed"_s : error);
            return;
        }
        auto response = JSON::Object::create();
        response->setObject("frameTree"_s, frameTreeToCDP(*tree));
        reply(WTF::move(response), { });
    });
}

void PageTarget::pageReload(Session&, JSON::Object& params, Reply&& reply)
{
    // Through the API, which also works when the page's process has died.
    if (params.getBoolean("ignoreCache"_s).value_or(false))
        webkit_web_view_reload_bypass_cache(m_view);
    else
        webkit_web_view_reload(m_view);
    replyOK(reply);
}

String PageTarget::bindingSource(const String& name)
{
    // The call travels as a console message: WebKit's protocol has no
    // channel from page script to the inspector other than the console.
    // The fourth argument is only there to tell which frame called.
    return makeString("(()=>{const n="_s, quote(name), ";if(globalThis[n]&&globalThis[n].__zebkit)return;const d=console.debug.bind(console);const f=function(p){if(typeof p!=='string')throw new Error('Invalid arguments: should be exactly one string.');d("_s, quote(bindingMarker), ",n,p,globalThis)};Object.defineProperty(f,'__zebkit',{value:true});globalThis[n]=f})();"_s);
}

// What runs in each new global object: of the main world (a null name) and
// of each isolated world.
void PageTarget::updateBootstrapScript()
{
    auto sourceFor = [&](const String& world) {
        StringBuilder source;
        HashSet<String> names;
        for (auto& binding : m_bindings) {
            bool applies = binding.worldName.isNull() || (!world.isNull() && binding.worldName == world);
            if (applies && !binding.name.isEmpty() && names.add(binding.name).isNewEntry)
                source.append(bindingSource(binding.name), '\n');
        }
        for (auto& script : m_scripts) {
            if (script.world.isNull() == world.isNull() && (world.isNull() || script.world == world))
                source.append("try{\n"_s, script.source, "\n}catch(e){console.error(e)}\n"_s);
        }
        return source.toString();
    };
    auto params = JSON::Object::create();
    params->setString("source"_s, sourceFor({ }));
    setState("Page.setBootstrapScript"_s, WTF::move(params));
    for (auto& world : m_worlds) {
        auto worldParams = JSON::Object::create();
        worldParams->setString("source"_s, sourceFor(world));
        worldParams->setString("worldName"_s, makeString(worldPrefix, world));
        setState("Page.setBootstrapScript"_s, WTF::move(worldParams), makeString("world:"_s, world));
    }
}

void PageTarget::pageAddScriptToEvaluateOnNewDocument(Session&, JSON::Object& params, Reply&& reply)
{
    Script script;
    script.identifier = m_nextScriptID++;
    script.source = params.getString("source"_s);
    if (auto worldName = params.getString("worldName"_s); !worldName.isEmpty()) {
        if (!ensureWorld(worldName)) {
            replyError(reply, "Too many isolated worlds"_s);
            return;
        }
        script.world = worldName;
    }
    if (params.getBoolean("runImmediately"_s).value_or(false)) {
        // In the documents that are there now, in the world it is for.
        auto run = [this, weakThis = WeakPtr { *this }, source = script.source, world = script.world] {
            if (!weakThis)
                return;
            for (auto& entry : m_contexts) {
                if (entry.value.world.isNull() != world.isNull() || (!world.isNull() && entry.value.world != world))
                    continue;
                auto evaluate = JSON::Object::create();
                evaluate->setString("expression"_s, source);
                evaluate->setInteger("contextId"_s, entry.key);
                wip("Runtime.evaluate"_s, WTF::move(evaluate));
            }
        };
        afterPageCatchesUp(WTF::move(run));
    }
    auto result = JSON::Object::create();
    result->setString("identifier"_s, String::number(script.identifier));
    m_scripts.append(WTF::move(script));
    updateBootstrapScript();
    afterPageCatchesUp([reply = WTF::move(reply), result = WTF::move(result)]() mutable {
        reply(WTF::move(result), { });
    });
}

void PageTarget::pageRemoveScriptToEvaluateOnNewDocument(Session&, JSON::Object& params, Reply&& reply)
{
    int identifier = parseInteger<int>(params.getString("identifier"_s)).value_or(0);
    bool removed = m_scripts.removeAllMatching([&](auto& script) {
        return script.identifier == identifier;
    });
    if (!removed) {
        replyError(reply, "Script not found"_s);
        return;
    }
    updateBootstrapScript();
    replyOK(reply);
}

void PageTarget::pageCreateIsolatedWorld(Session&, JSON::Object& params, Reply&& reply)
{
    auto worldName = params.getString("worldName"_s);
    auto frameID = toInnerFrameID(params.getString("frameId"_s));
    if (worldName.isNull())
        worldName = emptyString();
    afterPageCatchesUp([this, weakThis = WeakPtr { *this }, worldName, frameID, reply = WTF::move(reply)]() mutable {
        if (!weakThis)
            return;
        if (!m_frames.contains(frameID)) {
            replyInvalid(reply, "No frame for given id found"_s);
            return;
        }
        if (!ensureWorld(worldName)) {
            replyError(reply, "Too many isolated worlds"_s);
            return;
        }
        // WebKit has made the world in every frame by the time it answers.
        afterPageCatchesUp([this, weakThis, worldName, frameID, reply = WTF::move(reply)]() mutable {
            if (!weakThis)
                return;
            int contextID = contextForFrame(frameID, worldName);
            if (!contextID) {
                replyError(reply, "Could not create isolated world"_s);
                return;
            }
            announceContextToAll(contextID);
            auto result = JSON::Object::create();
            result->setInteger("executionContextId"_s, contextID);
            reply(WTF::move(result), { });
        });
    });
}

void PageTarget::pageCaptureScreenshot(Session& session, JSON::Object& params, Reply&& reply)
{
    auto snapshot = JSON::Object::create();
    RefPtr clip = params.getObject("clip"_s);
    if (clip) {
        snapshot->setInteger("x"_s, clip->getDouble("x"_s).value_or(0));
        snapshot->setInteger("y"_s, clip->getDouble("y"_s).value_or(0));
        snapshot->setInteger("width"_s, std::clamp<double>(std::ceil(clip->getDouble("width"_s).value_or(0)), 0, maxScreenshotSide));
        snapshot->setInteger("height"_s, std::clamp<double>(std::ceil(clip->getDouble("height"_s).value_or(0)), 0, maxScreenshotSide));
        snapshot->setString("coordinateSystem"_s, "Page"_s);
    } else if (params.getBoolean("captureBeyondViewport"_s).value_or(false)) {
        // The whole document: find out how large it is, then come back
        // with that as the clip.
        Ref protectedSession { session };
        Ref protectedParams { params };
        static constexpr auto expression = "(()=>{const d=document.documentElement,b=document.body;return [Math.max(d?d.scrollWidth:0,b?b.scrollWidth:0,innerWidth),Math.max(d?d.scrollHeight:0,b?b.scrollHeight:0,innerHeight)]})()"_s;
        evaluateValue(expression, [this, weakThis = WeakPtr { *this }, protectedSession, protectedParams, reply = WTF::move(reply)](RefPtr<JSON::Value>&& value) mutable {
            RefPtr size = value ? value->asArray() : nullptr;
            if (!weakThis || !size || size->length() != 2) {
                replyError(reply, "Unable to capture screenshot"_s);
                return;
            }
            auto whole = JSON::Object::create();
            whole->setDouble("x"_s, 0);
            whole->setDouble("y"_s, 0);
            whole->setDouble("width"_s, size->get(0)->asDouble().value_or(0));
            whole->setDouble("height"_s, size->get(1)->asDouble().value_or(0));
            protectedParams->setObject("clip"_s, WTF::move(whole));
            pageCaptureScreenshot(protectedSession, protectedParams, WTF::move(reply));
        });
        return;
    } else {
        auto* wpeView = webkit_web_view_get_wpe_view(m_view);
        snapshot->setInteger("x"_s, 0);
        snapshot->setInteger("y"_s, 0);
        snapshot->setInteger("width"_s, wpeView ? wpe_view_get_width(wpeView) : 0);
        snapshot->setInteger("height"_s, wpeView ? wpe_view_get_height(wpeView) : 0);
        snapshot->setString("coordinateSystem"_s, "Viewport"_s);
    }
    auto format = params.getString("format"_s);
    if (!format.isNull() && format != "png"_s && format != "jpeg"_s && format != "webp"_s) {
        replyInvalid(reply, "Invalid image format"_s);
        return;
    }
    int quality = params.getInteger("quality"_s).value_or(80);
    wip("Page.snapshotRect"_s, WTF::move(snapshot), [format, quality, reply = WTF::move(reply)](JSON::Object* result, const String& error) mutable {
        auto dataURL = result ? result->getString("dataURL"_s) : String();
        auto comma = dataURL.find(',');
        if (comma == notFound) {
            replyError(reply, error.isNull() ? "Unable to capture screenshot"_s : error);
            return;
        }
        auto data = dataURL.substring(comma + 1);
        if (format == "jpeg"_s || format == "webp"_s)
            data = transcodeScreenshot(data, format, quality);
        if (data.isNull()) {
            replyError(reply, "Unable to encode screenshot"_s);
            return;
        }
        auto response = JSON::Object::create();
        response->setString("data"_s, data);
        reply(WTF::move(response), { });
    });
}

void PageTarget::pageGetLayoutMetrics(Session&, JSON::Object&, Reply&& reply)
{
    static constexpr auto expression = "(()=>{const d=document.documentElement,b=document.body;return [innerWidth,innerHeight,scrollX,scrollY,Math.max(d?d.scrollWidth:0,b?b.scrollWidth:0),Math.max(d?d.scrollHeight:0,b?b.scrollHeight:0),d?d.clientWidth:innerWidth,d?d.clientHeight:innerHeight]})()"_s;
    evaluateValue(expression, [reply = WTF::move(reply)](RefPtr<JSON::Value>&& value) mutable {
        RefPtr numbers = value ? value->asArray() : nullptr;
        if (!numbers || numbers->length() != 8) {
            replyError(reply, "Cannot find context with specified id"_s);
            return;
        }
        auto number = [&](unsigned index) {
            return numbers->get(index)->asDouble().value_or(0);
        };
        auto layoutViewport = [&] {
            auto viewport = JSON::Object::create();
            viewport->setInteger("pageX"_s, number(2));
            viewport->setInteger("pageY"_s, number(3));
            viewport->setInteger("clientWidth"_s, number(6));
            viewport->setInteger("clientHeight"_s, number(7));
            return viewport;
        };
        auto visualViewport = [&] {
            auto viewport = JSON::Object::create();
            viewport->setDouble("offsetX"_s, 0);
            viewport->setDouble("offsetY"_s, 0);
            viewport->setDouble("pageX"_s, number(2));
            viewport->setDouble("pageY"_s, number(3));
            viewport->setDouble("clientWidth"_s, number(0));
            viewport->setDouble("clientHeight"_s, number(1));
            viewport->setDouble("scale"_s, 1);
            viewport->setDouble("zoom"_s, 1);
            return viewport;
        };
        auto contentSize = [&] {
            auto rect = JSON::Object::create();
            rect->setDouble("x"_s, 0);
            rect->setDouble("y"_s, 0);
            rect->setDouble("width"_s, number(4));
            rect->setDouble("height"_s, number(5));
            return rect;
        };
        auto result = JSON::Object::create();
        result->setObject("layoutViewport"_s, layoutViewport());
        result->setObject("visualViewport"_s, visualViewport());
        result->setObject("contentSize"_s, contentSize());
        result->setObject("cssLayoutViewport"_s, layoutViewport());
        result->setObject("cssVisualViewport"_s, visualViewport());
        result->setObject("cssContentSize"_s, contentSize());
        reply(WTF::move(result), { });
    });
}

// MARK: Runtime

void PageTarget::runtimeEnable(Session& session, JSON::Object&, Reply&& reply)
{
    Ref protectedSession { session };
    afterPageCatchesUp([this, weakThis = WeakPtr { *this }, protectedSession, reply = WTF::move(reply)]() mutable {
        if (weakThis && !protectedSession->runtimeEnabled) {
            protectedSession->runtimeEnabled = true;
            // Main worlds first, as when they are created.
            auto ids = copyToVector(m_contexts.keys());
            std::ranges::sort(ids, [&](int a, int b) {
                bool mainA = m_contexts.get(a).world.isNull();
                bool mainB = m_contexts.get(b).world.isNull();
                return mainA != mainB ? mainA : a < b;
            });
            for (auto id : ids)
                announceContext(protectedSession, id);
        }
        replyOK(reply);
    });
}

void PageTarget::runtimeDisable(Session& session, JSON::Object&, Reply&& reply)
{
    session.runtimeEnabled = false;
    replyOK(reply);
}

void PageTarget::finishEvaluation(JSON::Object* result, const String& error, bool returnByValue, bool awaitPromise, bool generatePreview, Reply&& reply)
{
    RefPtr object = result ? result->getObject("result"_s) : nullptr;
    if (!object) {
        replyError(reply, error.isNull() ? "Cannot find context with specified id"_s : errorToCDP(error));
        return;
    }
    if (result->getBoolean("wasThrown"_s).value_or(false)) {
        auto response = JSON::Object::create();
        response->setObject("exceptionDetails"_s, exceptionDetails(*object, m_nextExceptionID++));
        response->setObject("result"_s, remoteObjectToCDP(*object));
        reply(WTF::move(response), { });
        return;
    }

    auto objectID = object->getString("objectId"_s);
    if (awaitPromise && !objectID.isNull() && object->getString("className"_s) == "Promise"_s) {
        auto params = JSON::Object::create();
        params->setString("promiseObjectId"_s, objectID);
        params->setBoolean("returnByValue"_s, returnByValue);
        params->setBoolean("generatePreview"_s, generatePreview);
        wip("Runtime.awaitPromise"_s, WTF::move(params), [this, weakThis = WeakPtr { *this }, returnByValue, generatePreview, reply = WTF::move(reply)](JSON::Object* result, const String& error) mutable {
            if (weakThis)
                finishEvaluation(result, error, returnByValue, false, generatePreview, WTF::move(reply));
        });
        return;
    }
    if (awaitPromise && returnByValue && !objectID.isNull()) {
        // The expression was evaluated to a handle so that a promise could
        // be awaited. It was not one; fetch the value now.
        callOn(objectID, "function(){return this}"_s, nullptr, true, [this, weakThis = WeakPtr { *this }, reply = WTF::move(reply)](JSON::Object* result, const String& error) mutable {
            if (weakThis)
                finishEvaluation(result, error, true, false, false, WTF::move(reply));
        });
        return;
    }

    auto response = JSON::Object::create();
    response->setObject("result"_s, remoteObjectToCDP(*object));
    reply(WTF::move(response), { });
}

void PageTarget::runtimeEvaluate(Session&, JSON::Object& params, Reply&& reply)
{
    bool returnByValue = params.getBoolean("returnByValue"_s).value_or(false);
    bool awaitPromise = params.getBoolean("awaitPromise"_s).value_or(false);
    bool generatePreview = params.getBoolean("generatePreview"_s).value_or(false);

    auto evaluate = JSON::Object::create();
    evaluate->setString("expression"_s, params.getString("expression"_s));
    if (auto group = params.getString("objectGroup"_s); !group.isNull())
        evaluate->setString("objectGroup"_s, group);
    if (params.getBoolean("includeCommandLineAPI"_s).value_or(false))
        evaluate->setBoolean("includeCommandLineAPI"_s, true);
    int contextID = params.getInteger("contextId"_s).value_or(0);
    if (!contextID)
        contextID = parseInteger<int>(params.getString("uniqueContextId"_s)).value_or(0);
    if (contextID) {
        if (!isKnownContext(contextID)) {
            replyError(reply, "Cannot find context with specified id"_s);
            return;
        }
        evaluate->setInteger("contextId"_s, contextID);
    }
    evaluate->setBoolean("returnByValue"_s, returnByValue && !awaitPromise);
    evaluate->setBoolean("generatePreview"_s, generatePreview);
    if (params.getBoolean("userGesture"_s).value_or(false))
        evaluate->setBoolean("emulateUserGesture"_s, true);

    wip("Runtime.evaluate"_s, WTF::move(evaluate), [this, weakThis = WeakPtr { *this }, returnByValue, awaitPromise, generatePreview, reply = WTF::move(reply)](JSON::Object* result, const String& error) mutable {
        if (weakThis)
            finishEvaluation(result, error, returnByValue, awaitPromise, generatePreview, WTF::move(reply));
    });
}

void PageTarget::runtimeCallFunctionOn(Session&, JSON::Object& params, Reply&& reply)
{
    bool returnByValue = params.getBoolean("returnByValue"_s).value_or(false);
    bool generatePreview = params.getBoolean("generatePreview"_s).value_or(false);
    auto declaration = params.getString("functionDeclaration"_s);

    // WebKit takes arguments as JSON values or handles. The values JSON
    // cannot express arrive from CDP as source text and are spliced in.
    auto arguments = JSON::Array::create();
    StringBuilder patches;
    if (RefPtr given = params.getArray("arguments"_s)) {
        unsigned index = 0;
        for (auto& item : *given) {
            RefPtr argument = item->asObject();
            auto innerArgument = JSON::Object::create();
            if (argument) {
                if (auto objectID = argument->getString("objectId"_s); !objectID.isNull())
                    innerArgument->setString("objectId"_s, objectID);
                else if (auto unserializable = argument->getString("unserializableValue"_s); !unserializable.isNull())
                    patches.append("a["_s, index, "]="_s, unserializable, ';');
                else if (RefPtr value = argument->getValue("value"_s))
                    innerArgument->setValue("value"_s, value.releaseNonNull());
            }
            arguments->pushObject(WTF::move(innerArgument));
            ++index;
        }
    }
    // The declaration is wrapped in parentheses on the other side, which a
    // trailing line comment would swallow.
    if (!patches.isEmpty())
        declaration = makeString("function(){const a=Array.from(arguments);"_s, patches.toString(), "return ("_s, declaration, "\n).apply(this,a)}"_s);
    declaration = makeString(declaration, '\n');

    auto call = JSON::Object::create();
    call->setString("functionDeclaration"_s, declaration);
    call->setArray("arguments"_s, WTF::move(arguments));
    call->setBoolean("returnByValue"_s, returnByValue);
    call->setBoolean("generatePreview"_s, generatePreview);
    call->setBoolean("awaitPromise"_s, params.getBoolean("awaitPromise"_s).value_or(false));
    if (params.getBoolean("userGesture"_s).value_or(false))
        call->setBoolean("emulateUserGesture"_s, true);

    auto send = [this, weakThis = WeakPtr { *this }, call, returnByValue, generatePreview, reply = WTF::move(reply)](const String& objectID, const String& error) mutable {
        if (!weakThis)
            return;
        if (objectID.isNull()) {
            replyError(reply, error);
            return;
        }
        call->setString("objectId"_s, objectID);
        wip("Runtime.callFunctionOn"_s, WTF::move(call), [this, weakThis, returnByValue, generatePreview, reply = WTF::move(reply)](JSON::Object* result, const String& error) mutable {
            if (weakThis)
                finishEvaluation(result, error, returnByValue, false, generatePreview, WTF::move(reply));
        });
    };

    if (auto objectID = params.getString("objectId"_s); !objectID.isNull()) {
        send(objectID, { });
        return;
    }
    // Without an object, the function runs in a context with the global
    // object as "this".
    int contextID = params.getInteger("executionContextId"_s).value_or(0);
    if (!contextID)
        contextID = parseInteger<int>(params.getString("uniqueContextId"_s)).value_or(0);
    if (!contextID) {
        send({ }, makeString(invalidValuePrefix, "Either objectId or executionContextId or uniqueContextId must be specified"_s));
        return;
    }
    if (!isKnownContext(contextID)) {
        send({ }, "Cannot find context with specified id"_s);
        return;
    }
    globalObjectFor(contextID, WTF::move(send));
}

void PageTarget::runtimeAwaitPromise(Session&, JSON::Object& params, Reply&& reply)
{
    bool returnByValue = params.getBoolean("returnByValue"_s).value_or(false);
    bool generatePreview = params.getBoolean("generatePreview"_s).value_or(false);
    auto await = JSON::Object::create();
    await->setString("promiseObjectId"_s, params.getString("promiseObjectId"_s));
    await->setBoolean("returnByValue"_s, returnByValue);
    await->setBoolean("generatePreview"_s, generatePreview);
    wip("Runtime.awaitPromise"_s, WTF::move(await), [this, weakThis = WeakPtr { *this }, returnByValue, generatePreview, reply = WTF::move(reply)](JSON::Object* result, const String& error) mutable {
        if (weakThis)
            finishEvaluation(result, error, returnByValue, false, generatePreview, WTF::move(reply));
    });
}

void PageTarget::runtimeGetProperties(Session&, JSON::Object& params, Reply&& reply)
{
    if (params.getBoolean("accessorPropertiesOnly"_s).value_or(false)) {
        auto result = JSON::Object::create();
        result->setArray("result"_s, JSON::Array::create());
        reply(WTF::move(result), { });
        return;
    }
    auto get = JSON::Object::create();
    get->setString("objectId"_s, params.getString("objectId"_s));
    get->setBoolean("ownProperties"_s, params.getBoolean("ownProperties"_s).value_or(false));
    get->setBoolean("generatePreview"_s, params.getBoolean("generatePreview"_s).value_or(false));
    wip("Runtime.getProperties"_s, WTF::move(get), [reply = WTF::move(reply)](JSON::Object* result, const String& error) mutable {
        RefPtr properties = result ? result->getArray("properties"_s) : nullptr;
        if (!properties) {
            replyError(reply, error.isNull() ? "Could not find object with given id"_s : errorToCDP(error));
            return;
        }
        RefPtr internal = result->getArray("internalProperties"_s);
        auto listed = JSON::Array::create();
        for (auto& property : *properties) {
            RefPtr object = property->asObject();
            if (!object)
                continue;
            for (auto key : { "value"_s, "get"_s, "set"_s, "symbol"_s }) {
                if (RefPtr value = object->getObject(key))
                    remoteObjectToCDP(*value);
            }
            // WebKit lists the prototype as a property; in CDP it is an
            // internal one.
            if (RefPtr value = object->getObject("value"_s); value && object->getString("name"_s) == "__proto__"_s) {
                auto prototype = JSON::Object::create();
                prototype->setString("name"_s, "[[Prototype]]"_s);
                prototype->setObject("value"_s, value.releaseNonNull());
                if (!internal)
                    internal = JSON::Array::create();
                internal->pushObject(WTF::move(prototype));
                continue;
            }
            if (!object->getValue("configurable"_s))
                object->setBoolean("configurable"_s, false);
            if (!object->getValue("enumerable"_s))
                object->setBoolean("enumerable"_s, false);
            listed->pushObject(object.releaseNonNull());
        }
        auto response = JSON::Object::create();
        response->setArray("result"_s, WTF::move(listed));
        if (internal) {
            for (auto& property : *internal) {
                RefPtr object = property->asObject();
                if (RefPtr value = object ? object->getObject("value"_s) : nullptr)
                    remoteObjectToCDP(*value);
            }
            response->setArray("internalProperties"_s, internal.releaseNonNull());
        }
        reply(WTF::move(response), { });
    });
}

void PageTarget::runtimeReleaseObject(Session&, JSON::Object& params, Reply&& reply)
{
    auto release = JSON::Object::create();
    release->setString("objectId"_s, params.getString("objectId"_s));
    wip("Runtime.releaseObject"_s, WTF::move(release), [reply = WTF::move(reply)](JSON::Object*, const String&) mutable {
        replyOK(reply);
    });
}

void PageTarget::runtimeReleaseObjectGroup(Session&, JSON::Object& params, Reply&& reply)
{
    auto release = JSON::Object::create();
    release->setString("objectGroup"_s, params.getString("objectGroup"_s));
    wip("Runtime.releaseObjectGroup"_s, WTF::move(release), [reply = WTF::move(reply)](JSON::Object*, const String&) mutable {
        replyOK(reply);
    });
}

void PageTarget::runtimeRunIfWaitingForDebugger(Session& session, JSON::Object&, Reply&& reply)
{
    session.waitingForDebugger = false;
    replyOK(reply);
    resumeIfWaiting();
}

void PageTarget::resumeIfWaiting()
{
    if (pendingInitialURL.isEmpty() || !m_view)
        return;
    for (auto& session : m_sessions) {
        if (session->waitingForDebugger)
            return;
    }
    auto url = std::exchange(pendingInitialURL, String());
    webkit_web_view_load_uri(m_view, url.utf8().data());
}

void PageTarget::runtimeAddBinding(Session&, JSON::Object& params, Reply&& reply)
{
    Binding binding;
    binding.name = params.getString("name"_s);
    // With a world's name the function is in the contexts of that world;
    // without one it is in every context.
    binding.worldName = params.getString("executionContextName"_s);
    if (binding.worldName.isEmpty())
        binding.worldName = String();
    if (int contextID = params.getInteger("executionContextId"_s).value_or(0)) {
        auto context = m_contexts.find(contextID);
        if (context == m_contexts.end()) {
            replyError(reply, "Cannot find execution context with given executionContextId"_s);
            return;
        }
        binding.worldName = context->value.world;
    }
    if (!binding.worldName.isNull() && !ensureWorld(binding.worldName)) {
        replyError(reply, "Too many isolated worlds"_s);
        return;
    }
    m_bindings.removeAllMatching([&](auto& existing) {
        return existing.name == binding.name && existing.worldName == binding.worldName && existing.worldName.isNull() == binding.worldName.isNull();
    });
    auto source = bindingSource(binding.name);
    auto worldName = binding.worldName;
    m_bindings.append(WTF::move(binding));
    updateBootstrapScript();

    for (auto& entry : m_contexts) {
        if (!worldName.isNull() && (entry.value.world.isNull() || entry.value.world != worldName))
            continue;
        auto evaluate = JSON::Object::create();
        evaluate->setString("expression"_s, source);
        evaluate->setInteger("contextId"_s, entry.key);
        wip("Runtime.evaluate"_s, WTF::move(evaluate));
    }
    afterPageCatchesUp([reply = WTF::move(reply)]() mutable {
        replyOK(reply);
    });
}

void PageTarget::runtimeRemoveBinding(Session&, JSON::Object& params, Reply&& reply)
{
    auto name = params.getString("name"_s);
    m_bindings.removeAllMatching([&](auto& binding) {
        return binding.name == name;
    });
    updateBootstrapScript();
    replyOK(reply);
}

// MARK: DOM

static void stitchChildren(JSON::Object& node, IntMap<RefPtr<JSON::Array>>& captured)
{
    int nodeID = node.getInteger("nodeId"_s).value_or(0);
    if (!node.getArray("children"_s)) {
        if (auto iterator = captured.find(nodeID); iterator != captured.end() && iterator->value)
            node.setArray("children"_s, *iterator->value);
    }
    for (auto key : { "children"_s, "shadowRoots"_s, "pseudoElements"_s }) {
        if (RefPtr array = node.getArray(key)) {
            for (auto& child : *array) {
                if (RefPtr object = child->asObject())
                    stitchChildren(*object, captured);
            }
        }
    }
    for (auto key : { "contentDocument"_s, "templateContent"_s }) {
        if (RefPtr object = node.getObject(key))
            stitchChildren(*object, captured);
    }
}

void PageTarget::domEnable(Session& session, JSON::Object&, Reply&& reply)
{
    session.domEnabled = true;
    replyOK(reply);
}

void PageTarget::domDisable(Session& session, JSON::Object&, Reply&& reply)
{
    session.domEnabled = false;
    replyOK(reply);
}

void PageTarget::domGetDocument(Session&, JSON::Object& params, Reply&& reply)
{
    // Two levels unless told otherwise, as in Chrome, whatever the
    // definition says about one.
    int depth = params.getInteger("depth"_s).value_or(2);
    wip("DOM.getDocument"_s, nullptr, [this, weakThis = WeakPtr { *this }, depth, reply = WTF::move(reply)](JSON::Object* result, const String& error) mutable {
        RefPtr root = result ? result->getObject("root"_s) : nullptr;
        if (!weakThis || !root) {
            replyError(reply, error.isNull() ? "Document is not available"_s : error);
            return;
        }
        scanNode(*root);
        m_documentNodeID = root->getInteger("nodeId"_s).value_or(0);
        // WebKit always describes two levels below the document.
        if (depth == 1) {
            if (RefPtr children = root->getArray("children"_s)) {
                for (auto& child : *children) {
                    if (RefPtr object = child->asObject())
                        object->remove("children"_s);
                }
            }
        }
        auto finish = [root, reply = WTF::move(reply)]() mutable {
            auto response = JSON::Object::create();
            response->setObject("root"_s, root.releaseNonNull());
            reply(WTF::move(response), { });
        };
        // WebKit describes two levels and sends the rest as events when
        // asked. CDP clients that ask for a whole tree expect it inline.
        if (depth >= 0 && depth <= 2) {
            finish();
            return;
        }
        // Requests that overlap share what is captured.
        if (!m_childNodeCaptures++)
            m_capturedChildNodes = makeUnique<IntMap<RefPtr<JSON::Array>>>();
        auto request = JSON::Object::create();
        request->setInteger("nodeId"_s, m_documentNodeID);
        request->setInteger("depth"_s, -1);
        wip("DOM.requestChildNodes"_s, WTF::move(request), [this, weakThis, root, finish = WTF::move(finish)](JSON::Object*, const String&) mutable {
            if (!weakThis)
                return;
            if (m_capturedChildNodes)
                stitchChildren(*root, *m_capturedChildNodes);
            if (m_childNodeCaptures && !--m_childNodeCaptures)
                m_capturedChildNodes = nullptr;
            finish();
        });
    });
}

static constexpr auto describeFunction = "function(){return{nodeType:this.nodeType,nodeName:this.nodeName,localName:this.localName||'',nodeValue:this.nodeValue||'',childNodeCount:this.childNodes?this.childNodes.length:0,attributes:this.attributes?Array.from(this.attributes).flatMap(a=>[a.name,a.value]):undefined,isFrame:this instanceof HTMLIFrameElement||this instanceof HTMLFrameElement}}"_s;

void PageTarget::domDescribeNode(Session&, JSON::Object& params, Reply&& reply)
{
    callOnNode(params, describeFunction, "Could not find node with given id"_s, [this, weakThis = WeakPtr { *this }, reply = WTF::move(reply)](RefPtr<JSON::Value>&& value, const String& objectID, const String& error) mutable {
        RefPtr node = value ? value->asObject() : nullptr;
        if (!weakThis || !node) {
            replyError(reply, error.isNull() ? "Could not find node with given id"_s : error);
            return;
        }
        bool isFrame = node->getBoolean("isFrame"_s).value_or(false);
        node->remove("isFrame"_s);
        registerBackendNode(objectID, [this, weakThis, node, isFrame, objectID, reply = WTF::move(reply)](int backendNodeID) mutable {
            if (!weakThis)
                return;
            node->setInteger("nodeId"_s, 0);
            node->setInteger("backendNodeId"_s, backendNodeID);
            auto finish = [node, reply = WTF::move(reply)]() mutable {
                auto response = JSON::Object::create();
                response->setObject("node"_s, node.releaseNonNull());
                reply(WTF::move(response), { });
            };
            if (!isFrame) {
                finish();
                return;
            }
            // Which frame an element holds is known to WebKit's DOM domain
            // only, and comes with the description of the element.
            ensureDocument([this, weakThis, node, objectID, finish = WTF::move(finish)]() mutable {
                if (!weakThis)
                    return;
                auto request = JSON::Object::create();
                request->setString("objectId"_s, objectID);
                wip("DOM.requestNode"_s, WTF::move(request), [this, weakThis, node, finish = WTF::move(finish)](JSON::Object* result, const String&) mutable {
                    if (!weakThis)
                        return;
                    int nodeID = result ? result->getInteger("nodeId"_s).value_or(0) : 0;
                    if (auto iterator = m_frameOwnerNodes.find(nodeID); iterator != m_frameOwnerNodes.end())
                        node->setString("frameId"_s, toCDPFrameID(iterator->value));
                    finish();
                });
            });
        });
    });
}

// A handle for the node behind objectID in another context of its frame. A
// node has one wrapper object per world, and only WebKit can make them.
void PageTarget::resolveNodeInContext(const String& objectID, int contextID, Function<void(JSON::Object*, const String&)>&& completion)
{
    if (!contextID || contextOfObject(objectID) == contextID) {
        callOn(objectID, "function(){return this}"_s, nullptr, false, [completion = WTF::move(completion)](JSON::Object* result, const String& error) {
            RefPtr object = result ? result->getObject("result"_s) : nullptr;
            completion(object.get(), error);
        });
        return;
    }
    ensureDocument([this, weakThis = WeakPtr { *this }, objectID, contextID, completion = WTF::move(completion)]() mutable {
        if (!weakThis)
            return;
        auto request = JSON::Object::create();
        request->setString("objectId"_s, objectID);
        wip("DOM.requestNode"_s, WTF::move(request), [this, weakThis, contextID, completion = WTF::move(completion)](JSON::Object* result, const String& error) mutable {
            int nodeID = result ? result->getInteger("nodeId"_s).value_or(0) : 0;
            if (!weakThis || !nodeID) {
                completion(nullptr, error);
                return;
            }
            auto resolve = JSON::Object::create();
            resolve->setInteger("nodeId"_s, nodeID);
            resolve->setInteger("executionContextId"_s, contextID);
            wip("DOM.resolveNode"_s, WTF::move(resolve), [completion = WTF::move(completion)](JSON::Object* result, const String& error) {
                RefPtr object = result ? result->getObject("object"_s) : nullptr;
                if (!object && error.contains("does not belong"_s))
                    completion(nullptr, "Node with given id does not belong to the document"_s);
                else
                    completion(object.get(), error);
            });
        });
    });
}

void PageTarget::domResolveNode(Session&, JSON::Object& params, Reply&& reply)
{
    int requestedContext = params.getInteger("executionContextId"_s).value_or(0);
    if (requestedContext && !isKnownContext(requestedContext)) {
        replyError(reply, "Node with given id does not belong to the document"_s);
        return;
    }
    resolveNode(params, [this, weakThis = WeakPtr { *this }, requestedContext, reply = WTF::move(reply)](const String& objectID, const String& error) mutable {
        if (!weakThis || objectID.isNull()) {
            replyError(reply, error.isNull() ? "No node with given id found"_s : error);
            return;
        }
        // Without a context, the main world of the node's frame.
        int contextID = requestedContext;
        if (!contextID) {
            if (auto context = m_contexts.find(contextOfObject(objectID)); context != m_contexts.end() && !context->value.world.isNull())
                contextID = contextForFrame(context->value.frameID);
        }
        resolveNodeInContext(objectID, contextID, [reply = WTF::move(reply)](JSON::Object* object, const String& error) mutable {
            if (!object) {
                replyError(reply, error.isNull() ? "No node with given id found"_s : errorToCDP(error));
                return;
            }
            auto response = JSON::Object::create();
            response->setObject("object"_s, remoteObjectToCDP(*object));
            reply(WTF::move(response), { });
        });
    });
}

void PageTarget::domRequestNode(Session&, JSON::Object& params, Reply&& reply)
{
    auto objectID = params.getString("objectId"_s);
    ensureDocument([this, weakThis = WeakPtr { *this }, objectID, reply = WTF::move(reply)]() mutable {
        if (!weakThis)
            return;
        auto request = JSON::Object::create();
        request->setString("objectId"_s, objectID);
        wip("DOM.requestNode"_s, WTF::move(request), [reply = WTF::move(reply)](JSON::Object* result, const String& error) mutable {
            if (!result) {
                replyError(reply, error);
                return;
            }
            reply(Ref { *result }, { });
        });
    });
}

static constexpr auto boxModelFunction = "function(){const e=this.nodeType===1?this:this.parentElement;if(!e)return null;const r=e.getBoundingClientRect();if(!e.getClientRects().length)return null;const s=getComputedStyle(e),n=v=>parseFloat(v)||0,q=(x,y,w,h)=>[x,y,x+w,y,x+w,y+h,x,y+h];const bl=n(s.borderLeftWidth),bt=n(s.borderTopWidth),br=n(s.borderRightWidth),bb=n(s.borderBottomWidth),pl=n(s.paddingLeft),pt=n(s.paddingTop),pr=n(s.paddingRight),pb=n(s.paddingBottom),ml=n(s.marginLeft),mt=n(s.marginTop),mr=n(s.marginRight),mb=n(s.marginBottom);return{content:q(r.x+bl+pl,r.y+bt+pt,r.width-bl-br-pl-pr,r.height-bt-bb-pt-pb),padding:q(r.x+bl,r.y+bt,r.width-bl-br,r.height-bt-bb),border:q(r.x,r.y,r.width,r.height),margin:q(r.x-ml,r.y-mt,r.width+ml+mr,r.height+mt+mb),width:Math.round(r.width),height:Math.round(r.height)}}"_s;

void PageTarget::domGetBoxModel(Session&, JSON::Object& params, Reply&& reply)
{
    callOnNode(params, boxModelFunction, "Could not compute box model."_s, [this, weakThis = WeakPtr { *this }, reply = WTF::move(reply)](RefPtr<JSON::Value>&& value, const String& objectID, const String& error) mutable {
        RefPtr model = value ? value->asObject() : nullptr;
        if (!weakThis || !model) {
            replyError(reply, error.isNull() ? "Could not compute box model."_s : error);
            return;
        }
        frameOffsetOfObject(objectID, [model, reply = WTF::move(reply)](double x, double y) mutable {
            for (auto key : { "content"_s, "padding"_s, "border"_s, "margin"_s }) {
                if (RefPtr quad = model->getArray(key))
                    model->setArray(key, offsetQuad(*quad, x, y));
            }
            auto response = JSON::Object::create();
            response->setObject("model"_s, model.releaseNonNull());
            reply(WTF::move(response), { });
        });
    });
}

static constexpr auto contentQuadsFunction = "function(){let t=this;if(t.nodeType!==1){t=document.createRange();t.selectNodeContents(this)}const q=[];for(const r of t.getClientRects())if(r.width&&r.height)q.push([r.x,r.y,r.x+r.width,r.y,r.x+r.width,r.y+r.height,r.x,r.y+r.height]);return q}"_s;

void PageTarget::domGetContentQuads(Session&, JSON::Object& params, Reply&& reply)
{
    callOnNode(params, contentQuadsFunction, "Could not compute content quads."_s, [this, weakThis = WeakPtr { *this }, reply = WTF::move(reply)](RefPtr<JSON::Value>&& value, const String& objectID, const String& error) mutable {
        RefPtr quads = value ? value->asArray() : nullptr;
        if (!weakThis || !quads) {
            replyError(reply, error.isNull() ? "Could not compute content quads."_s : error);
            return;
        }
        frameOffsetOfObject(objectID, [quads, reply = WTF::move(reply)](double x, double y) mutable {
            auto result = JSON::Array::create();
            for (auto& quad : *quads) {
                if (RefPtr array = quad->asArray())
                    result->pushArray(offsetQuad(*array, x, y));
            }
            auto response = JSON::Object::create();
            response->setArray("quads"_s, WTF::move(result));
            reply(WTF::move(response), { });
        });
    });
}

void PageTarget::domScrollIntoViewIfNeeded(Session&, JSON::Object& params, Reply&& reply)
{
    static constexpr auto function = "function(){const e=this.nodeType===1?this:this.parentElement;if(!e||!e.isConnected)return null;e.scrollIntoView({block:'center',inline:'center',behavior:'instant'});return true}"_s;
    callOnNode(params, function, "Node is detached from document"_s, [reply = WTF::move(reply)](RefPtr<JSON::Value>&& value, const String&, const String& error) mutable {
        if (value)
            replyOK(reply);
        else
            replyError(reply, error);
    });
}

void PageTarget::domFocus(Session&, JSON::Object& params, Reply&& reply)
{
    focusView();
    static constexpr auto function = "function(){if(typeof this.focus!=='function')return null;this.focus();const r=this.getRootNode();return r.activeElement===this||null}"_s;
    callOnNode(params, function, "Element is not focusable"_s, [reply = WTF::move(reply)](RefPtr<JSON::Value>&& value, const String&, const String& error) mutable {
        if (value)
            replyOK(reply);
        else
            replyError(reply, error);
    });
}

void PageTarget::domGetOuterHTML(Session&, JSON::Object& params, Reply&& reply)
{
    static constexpr auto function = "function(){return typeof this.outerHTML==='string'?this.outerHTML:new XMLSerializer().serializeToString(this)}"_s;
    callOnNode(params, function, "Could not find node with given id"_s, [reply = WTF::move(reply)](RefPtr<JSON::Value>&& value, const String&, const String& error) mutable {
        if (!value) {
            replyError(reply, error);
            return;
        }
        auto response = JSON::Object::create();
        response->setString("outerHTML"_s, value->asString());
        reply(WTF::move(response), { });
    });
}

// WebKit's protocol cannot put files into an <input type=file>. The API
// can answer a file chooser, though, so the files are remembered and the
// input is clicked as a user would; see runFileChooser().
void PageTarget::domSetFileInputFiles(Session&, JSON::Object& params, Reply&& reply)
{
    Vector<String> files;
    if (RefPtr array = params.getArray("files"_s)) {
        for (auto& file : *array)
            files.append(file->asString());
    }
    resolveNode(params, [this, weakThis = WeakPtr { *this }, files = WTF::move(files), reply = WTF::move(reply)](const String& objectID, const String& error) mutable {
        if (!weakThis || objectID.isNull()) {
            replyError(reply, error.isNull() ? "Could not find node with given id"_s : error);
            return;
        }
        if (files.isEmpty()) {
            callOn(objectID, "function(){this.value='';this.dispatchEvent(new Event('input',{bubbles:true}));this.dispatchEvent(new Event('change',{bubbles:true}))}"_s, nullptr, true, [reply = WTF::move(reply)](JSON::Object*, const String&) mutable {
                replyOK(reply);
            });
            return;
        }
        m_pendingFiles = WTF::move(files);
        m_expectingFileChooser = true;
        // Only a file input is clicked: on anything else a click would do
        // whatever that element does.
        callOn(objectID, "function(){if(this.localName==='input'&&this.type==='file')this.click()}"_s, nullptr, true, [this, weakThis, reply = WTF::move(reply)](JSON::Object*, const String&) mutable {
            if (!weakThis)
                return;
            if (std::exchange(m_expectingFileChooser, false)) {
                m_pendingFiles.clear();
                replyError(reply, "Node is not a file input element"_s);
                return;
            }
            afterPageCatchesUp([reply = WTF::move(reply)]() mutable {
                replyOK(reply);
            });
        }, true);
    });
}

void PageTarget::domGetFrameOwner(Session&, JSON::Object& params, Reply&& reply)
{
    findFrameOwner(toInnerFrameID(params.getString("frameId"_s)), [this, weakThis = WeakPtr { *this }, reply = WTF::move(reply)](const String& objectID) mutable {
        if (!weakThis || objectID.isNull()) {
            replyError(reply, "Frame with the given id does not belong to the target."_s);
            return;
        }
        registerBackendNode(objectID, [reply = WTF::move(reply)](int backendNodeID) mutable {
            auto response = JSON::Object::create();
            response->setInteger("backendNodeId"_s, backendNodeID);
            reply(WTF::move(response), { });
        });
    });
}

// DOM commands that have the same parameters and results in both protocols.
void PageTarget::domPassThrough(const String& method, JSON::Object& params, Reply&& reply)
{
    static constexpr ASCIILiteral methods[] = {
        "DOM.querySelector"_s, "DOM.querySelectorAll"_s, "DOM.getAttributes"_s, "DOM.setAttributeValue"_s,
        "DOM.setAttributesAsText"_s, "DOM.removeAttribute"_s, "DOM.removeNode"_s, "DOM.setNodeValue"_s,
        "DOM.setNodeName"_s, "DOM.setOuterHTML"_s, "DOM.requestChildNodes"_s, "DOM.performSearch"_s,
        "DOM.getSearchResults"_s, "DOM.discardSearchResults"_s, "DOM.moveTo"_s, "DOM.undo"_s, "DOM.redo"_s,
        "DOM.markUndoableState"_s, "DOM.pushNodeByPathToFrontend"_s,
    };
    for (auto name : methods) {
        if (method != name)
            continue;
        ensureDocument([this, weakThis = WeakPtr { *this }, name, params = Ref { params }, reply = WTF::move(reply)]() mutable {
            if (!weakThis)
                return;
            wip(name, WTF::move(params), [reply = WTF::move(reply)](JSON::Object* result, const String& error) mutable {
                if (result)
                    reply(Ref { *result }, { });
                else
                    replyError(reply, error);
            });
        });
        return;
    }
    replyError(reply, makeString('\'', method, "' wasn't found"_s));
}

// MARK: Network

void PageTarget::networkEnable(Session& session, JSON::Object&, Reply&& reply)
{
    session.networkEnabled = true;
    replyOK(reply);
}

void PageTarget::networkDisable(Session& session, JSON::Object&, Reply&& reply)
{
    session.networkEnabled = false;
    replyOK(reply);
}

void PageTarget::networkSetExtraHTTPHeaders(Session&, JSON::Object& params, Reply&& reply)
{
    auto state = JSON::Object::create();
    RefPtr headers = params.getObject("headers"_s);
    state->setObject("headers"_s, headers ? headers.releaseNonNull() : JSON::Object::create());
    setState("Network.setExtraHTTPHeaders"_s, WTF::move(state));
    replyOK(reply);
}

void PageTarget::networkSetCacheDisabled(Session&, JSON::Object& params, Reply&& reply)
{
    auto state = JSON::Object::create();
    state->setBoolean("disabled"_s, params.getBoolean("cacheDisabled"_s).value_or(false));
    setState("Network.setResourceCachingDisabled"_s, WTF::move(state));
    replyOK(reply);
}

void PageTarget::networkSetUserAgentOverride(Session&, JSON::Object& params, Reply&& reply)
{
    auto state = JSON::Object::create();
    if (auto userAgent = params.getString("userAgent"_s); !userAgent.isEmpty())
        state->setString("value"_s, userAgent);
    setState("Page.overrideUserAgent"_s, WTF::move(state));
    replyOK(reply);
}

void PageTarget::networkGetResponseBody(Session&, JSON::Object& params, Reply&& reply)
{
    auto request = JSON::Object::create();
    request->setString("requestId"_s, toInnerRequestID(params.getString("requestId"_s)));
    wip("Network.getResponseBody"_s, WTF::move(request), [reply = WTF::move(reply)](JSON::Object* result, const String& error) mutable {
        if (result)
            reply(Ref { *result }, { });
        else
            replyError(reply, "No resource with given identifier found"_s);
    });
}

void PageTarget::networkGetCookies(Session&, JSON::Object& params, Reply&& reply)
{
    // Cookies that apply to the given addresses, or to the documents of
    // the page when none are given.
    Vector<GRefPtr<GUri>> uris;
    auto addURL = [&](const String& url) {
        if (GRefPtr<GUri> uri = adoptGRef(g_uri_parse(url.utf8().data(), G_URI_FLAGS_NONE, nullptr)))
            uris.append(WTF::move(uri));
    };
    if (RefPtr urls = params.getArray("urls"_s)) {
        for (auto& url : *urls)
            addURL(url->asString());
    } else {
        addURL(str(webkit_web_view_get_uri(m_view)));
        for (auto& frame : m_frames.values())
            addURL(frame.url);
    }
    allCookies(webkit_web_view_get_network_session(m_view), [uris = WTF::move(uris), reply = WTF::move(reply)](CookieList&& cookies) mutable {
        auto array = JSON::Array::create();
        for (auto& cookie : cookies) {
            bool applies = false;
            for (auto& uri : uris)
                applies |= g_uri_get_host(uri.get()) && soup_cookie_applies_to_uri(cookie.get(), uri.get());
            if (applies)
                array->pushObject(cookieToCDP(cookie.get()));
        }
        auto result = JSON::Object::create();
        result->setArray("cookies"_s, WTF::move(array));
        reply(WTF::move(result), { });
    });
}

void PageTarget::networkGetAllCookies(Session&, JSON::Object&, Reply&& reply)
{
    allCookies(webkit_web_view_get_network_session(m_view), [reply = WTF::move(reply)](CookieList&& cookies) mutable {
        auto array = JSON::Array::create();
        for (auto& cookie : cookies)
            array->pushObject(cookieToCDP(cookie.get()));
        auto result = JSON::Object::create();
        result->setArray("cookies"_s, WTF::move(array));
        reply(WTF::move(result), { });
    });
}

void PageTarget::networkSetCookie(Session&, JSON::Object& params, Reply&& reply)
{
    if (params.getString("url"_s).isEmpty() && params.getString("domain"_s).isEmpty()) {
        replyInvalid(reply, "At least one of the url or domain needs to be specified"_s);
        return;
    }
    auto cookie = cookieFromCDP(params, str(webkit_web_view_get_uri(m_view)));
    if (!cookie) {
        replyError(reply, "Invalid cookie fields"_s);
        return;
    }
    addCookie(webkit_web_view_get_network_session(m_view), cookie.get(), [reply = WTF::move(reply)]() mutable {
        auto result = JSON::Object::create();
        result->setBoolean("success"_s, true);
        reply(WTF::move(result), { });
    });
}

void PageTarget::networkSetCookies(Session&, JSON::Object& params, Reply&& reply)
{
    auto sharedReply = Box<Reply>::create(WTF::move(reply));
    auto aggregator = CallbackAggregator::create([sharedReply] {
        replyOK(*sharedReply);
    });
    RefPtr cookies = params.getArray("cookies"_s);
    if (!cookies)
        return;
    for (auto& value : *cookies) {
        RefPtr object = value->asObject();
        auto cookie = object ? cookieFromCDP(*object, str(webkit_web_view_get_uri(m_view))) : nullptr;
        if (cookie)
            addCookie(webkit_web_view_get_network_session(m_view), cookie.get(), [aggregator] { });
    }
}

void PageTarget::networkDeleteCookies(Session&, JSON::Object& params, Reply&& reply)
{
    auto name = params.getString("name"_s);
    auto domain = params.getString("domain"_s);
    auto path = params.getString("path"_s);
    auto url = params.getString("url"_s);
    if (url.isEmpty() && domain.isEmpty()) {
        replyInvalid(reply, "At least one of the url and domain needs to be specified"_s);
        return;
    }
    GRefPtr<GUri> uri = url.isEmpty() ? nullptr : adoptGRef(g_uri_parse(url.utf8().data(), G_URI_FLAGS_NONE, nullptr));
    GRefPtr<WebKitNetworkSession> networkSession = webkit_web_view_get_network_session(m_view);
    allCookies(networkSession.get(), [networkSession, name, domain, path, uri, reply = WTF::move(reply)](CookieList&& cookies) mutable {
        auto sharedReply = Box<Reply>::create(WTF::move(reply));
        auto aggregator = CallbackAggregator::create([sharedReply] {
            replyOK(*sharedReply);
        });
        for (auto& cookie : cookies) {
            if (name != str(soup_cookie_get_name(cookie.get())))
                continue;
            if (!domain.isEmpty() && !soup_cookie_domain_matches(cookie.get(), domain.utf8().data()))
                continue;
            if (!path.isEmpty() && path != str(soup_cookie_get_path(cookie.get())))
                continue;
            if (uri && g_uri_get_host(uri.get()) && !soup_cookie_domain_matches(cookie.get(), g_uri_get_host(uri.get())))
                continue;
            deleteCookie(networkSession.get(), cookie.get(), [aggregator] { });
        }
    });
}

void PageTarget::networkClearBrowserCookies(Session&, JSON::Object&, Reply&& reply)
{
    GRefPtr<WebKitNetworkSession> networkSession = webkit_web_view_get_network_session(m_view);
    allCookies(networkSession.get(), [networkSession, reply = WTF::move(reply)](CookieList&& cookies) mutable {
        auto sharedReply = Box<Reply>::create(WTF::move(reply));
        auto aggregator = CallbackAggregator::create([sharedReply] {
            replyOK(*sharedReply);
        });
        for (auto& cookie : cookies)
            deleteCookie(networkSession.get(), cookie.get(), [aggregator] { });
    });
}

void PageTarget::securitySetIgnoreCertificateErrors(Session&, JSON::Object& params, Reply&& reply)
{
    bool ignore = params.getBoolean("ignore"_s).value_or(false);
    // The session is shared with other views; see disconnectInspector().
    if (!m_originalTLSPolicy)
        m_originalTLSPolicy = webkit_network_session_get_tls_errors_policy(webkit_web_view_get_network_session(m_view));
    webkit_network_session_set_tls_errors_policy(webkit_web_view_get_network_session(m_view), ignore ? WEBKIT_TLS_ERRORS_POLICY_IGNORE : WEBKIT_TLS_ERRORS_POLICY_FAIL);
    replyOK(reply);
}

// MARK: Emulation and Log

void PageTarget::emulationSetEmulatedMedia(Session&, JSON::Object& params, Reply&& reply)
{
    auto state = JSON::Object::create();
    state->setString("media"_s, params.getString("media"_s).isNull() ? emptyString() : params.getString("media"_s));
    setState("Page.setEmulatedMedia"_s, WTF::move(state));
    replyOK(reply);
}

void PageTarget::logEnable(Session& session, JSON::Object&, Reply&& reply)
{
    session.logEnabled = true;
    replyOK(reply);
}

void PageTarget::logDisable(Session& session, JSON::Object&, Reply&& reply)
{
    session.logEnabled = false;
    replyOK(reply);
}

// MARK: Table

const HashMap<String, PageTarget::Handler>& PageTarget::handlers()
{
    static NeverDestroyed<HashMap<String, Handler>> table = [] {
        HashMap<String, Handler> map;
        auto add = [&](ASCIILiteral name, Handler handler) {
            map.set(String(name), handler);
        };

        add("Page.enable"_s, &PageTarget::pageEnable);
        add("Page.disable"_s, &PageTarget::pageDisable);
        add("Page.getFrameTree"_s, &PageTarget::pageGetFrameTree);
        add("Page.navigate"_s, &PageTarget::pageNavigate);
        add("Page.reload"_s, &PageTarget::pageReload);
        add("Page.stopLoading"_s, &PageTarget::pageStopLoading);
        add("Page.setLifecycleEventsEnabled"_s, &PageTarget::pageSetLifecycleEventsEnabled);
        add("Page.addScriptToEvaluateOnNewDocument"_s, &PageTarget::pageAddScriptToEvaluateOnNewDocument);
        add("Page.removeScriptToEvaluateOnNewDocument"_s, &PageTarget::pageRemoveScriptToEvaluateOnNewDocument);
        add("Page.createIsolatedWorld"_s, &PageTarget::pageCreateIsolatedWorld);
        add("Page.captureScreenshot"_s, &PageTarget::pageCaptureScreenshot);
        add("Page.getLayoutMetrics"_s, &PageTarget::pageGetLayoutMetrics);
        add("Page.getNavigationHistory"_s, &PageTarget::pageGetNavigationHistory);
        add("Page.navigateToHistoryEntry"_s, &PageTarget::pageNavigateToHistoryEntry);
        add("Page.handleJavaScriptDialog"_s, &PageTarget::pageHandleJavaScriptDialog);
        add("Page.bringToFront"_s, &PageTarget::pageBringToFront);
        add("Page.close"_s, &PageTarget::pageClose);
        add("Page.crash"_s, &PageTarget::pageCrash);

        add("Runtime.enable"_s, &PageTarget::runtimeEnable);
        add("Runtime.disable"_s, &PageTarget::runtimeDisable);
        add("Runtime.evaluate"_s, &PageTarget::runtimeEvaluate);
        add("Runtime.callFunctionOn"_s, &PageTarget::runtimeCallFunctionOn);
        add("Runtime.awaitPromise"_s, &PageTarget::runtimeAwaitPromise);
        add("Runtime.getProperties"_s, &PageTarget::runtimeGetProperties);
        add("Runtime.releaseObject"_s, &PageTarget::runtimeReleaseObject);
        add("Runtime.releaseObjectGroup"_s, &PageTarget::runtimeReleaseObjectGroup);
        add("Runtime.runIfWaitingForDebugger"_s, &PageTarget::runtimeRunIfWaitingForDebugger);
        add("Runtime.addBinding"_s, &PageTarget::runtimeAddBinding);
        add("Runtime.removeBinding"_s, &PageTarget::runtimeRemoveBinding);

        add("DOM.enable"_s, &PageTarget::domEnable);
        add("DOM.disable"_s, &PageTarget::domDisable);
        add("DOM.getDocument"_s, &PageTarget::domGetDocument);
        add("DOM.describeNode"_s, &PageTarget::domDescribeNode);
        add("DOM.resolveNode"_s, &PageTarget::domResolveNode);
        add("DOM.requestNode"_s, &PageTarget::domRequestNode);
        add("DOM.getBoxModel"_s, &PageTarget::domGetBoxModel);
        add("DOM.getContentQuads"_s, &PageTarget::domGetContentQuads);
        add("DOM.scrollIntoViewIfNeeded"_s, &PageTarget::domScrollIntoViewIfNeeded);
        add("DOM.focus"_s, &PageTarget::domFocus);
        add("DOM.getOuterHTML"_s, &PageTarget::domGetOuterHTML);
        add("DOM.setFileInputFiles"_s, &PageTarget::domSetFileInputFiles);
        add("DOM.getFrameOwner"_s, &PageTarget::domGetFrameOwner);

        add("Network.enable"_s, &PageTarget::networkEnable);
        add("Network.disable"_s, &PageTarget::networkDisable);
        add("Network.setExtraHTTPHeaders"_s, &PageTarget::networkSetExtraHTTPHeaders);
        add("Network.setCacheDisabled"_s, &PageTarget::networkSetCacheDisabled);
        add("Network.setUserAgentOverride"_s, &PageTarget::networkSetUserAgentOverride);
        add("Network.getResponseBody"_s, &PageTarget::networkGetResponseBody);
        add("Network.getCookies"_s, &PageTarget::networkGetCookies);
        add("Network.getAllCookies"_s, &PageTarget::networkGetAllCookies);
        add("Network.setCookie"_s, &PageTarget::networkSetCookie);
        add("Network.setCookies"_s, &PageTarget::networkSetCookies);
        add("Network.deleteCookies"_s, &PageTarget::networkDeleteCookies);
        add("Network.clearBrowserCookies"_s, &PageTarget::networkClearBrowserCookies);
        add("Security.setIgnoreCertificateErrors"_s, &PageTarget::securitySetIgnoreCertificateErrors);

        add("Emulation.setDeviceMetricsOverride"_s, &PageTarget::emulationSetDeviceMetricsOverride);
        add("Emulation.clearDeviceMetricsOverride"_s, &PageTarget::emulationClearDeviceMetricsOverride);
        add("Emulation.setUserAgentOverride"_s, &PageTarget::networkSetUserAgentOverride);
        add("Emulation.setEmulatedMedia"_s, &PageTarget::emulationSetEmulatedMedia);
        add("Log.enable"_s, &PageTarget::logEnable);
        add("Log.disable"_s, &PageTarget::logDisable);

        add("Input.dispatchMouseEvent"_s, &PageTarget::inputDispatchMouseEvent);
        add("Input.dispatchKeyEvent"_s, &PageTarget::inputDispatchKeyEvent);
        add("Input.insertText"_s, &PageTarget::inputInsertText);

        // Requests that only switch on reporting this bridge does not have,
        // or adjust something that is already the case here. Answering them
        // lets client libraries get through their setup; nothing is
        // acknowledged that would change what a page does.
        static constexpr ASCIILiteral acknowledged[] = {
            "CSS.enable"_s, "CSS.disable"_s, "Overlay.enable"_s, "Overlay.disable"_s, "Overlay.hideHighlight"_s,
            "Inspector.enable"_s, "Performance.enable"_s, "Performance.disable"_s, "Security.enable"_s, "Security.disable"_s,
            "ServiceWorker.enable"_s, "ServiceWorker.disable"_s, "Audits.enable"_s, "Accessibility.enable"_s,
            "Accessibility.disable"_s, "Fetch.disable"_s, "Log.clear"_s, "Runtime.discardConsoleEntries"_s,
            "Page.setInterceptFileChooserDialog"_s, "Page.setDownloadBehavior"_s, "Page.setAdBlockingEnabled"_s,
            "Page.setWebLifecycleState"_s, "Page.resetNavigationHistory"_s, "Page.setPrerenderingAllowed"_s,
            "Network.setBypassServiceWorker"_s, "Network.setAttachDebugStack"_s,
            "Emulation.setTouchEmulationEnabled"_s,
            "Emulation.setFocusEmulationEnabled"_s, "Emulation.setScrollbarsHidden"_s,
            "Emulation.setDefaultBackgroundColorOverride"_s, "Emulation.setEmitTouchEventsForMouse"_s,
            "Emulation.setAutomationOverride"_s, "Input.setIgnoreInputEvents"_s,
        };
        for (auto name : acknowledged)
            add(name, &PageTarget::acknowledge);
        return map;
    }();
    return table;
}

} // namespace Zebkit::CDP
