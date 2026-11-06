// zebkit's automation, the renderer's part (docs/AUTOMATION.md): the steps
// of Playwright's commands, each run as one call over Playwright's injected
// script, where Playwright's server makes one CDP call per primitive
// (packages/playwright-core/src/server/frames.ts, dom.ts, javascript.ts,
// which each step follows and names).
//
// automation_scripts.py puts this after injected_script.js in a function of
// `isUtilityWorld` whose value is the runner below; automation_renderer.cc
// makes one in the page's world and one in zebkit's isolated world, keeps
// it, and calls its methods. Each method answers [json, handles, previews, nodes]:
// json is {ok: true, value} or {ok: false, error}; an object the value
// refers to as {"$h": i} is handles[i], which the agent keeps as a handle,
// previews[i] what Playwright shows of it, nodes[i] whether it is a node. An argument refers to the
// agent's handles the same way.

// The context's selector engines and test id attribute (dom.ts's
// injectedScript()): the injected script made again when they change.
let selectorOptions = '';
function useSelectorOptions(given) {
  const json = stringifyJson(given);
  if (json === selectorOptions)
    return;
  selectorOptions = json;
  options.customEngines = given.customEngines || [];
  if (given.testIdAttributeName)
    options.testIdAttributeName = given.testIdAttributeName;
  injectedScript = undefined;
}

// Playwright's InjectedScript, made the first time a step needs it: an
// evaluation in the page's world does not, and must work when the page has
// broken what its constructor uses (Array.prototype.map, say).
let injectedScript;
const injectedNow = () => injectedScript ??= new (module.exports.InjectedScript())(globalThis, { ...options, isUtilityWorld, isUnderTest });
const lazily = get => new Proxy({}, {
  get(_, key) {
    const target = get();
    const value = target[key];
    return typeof value === 'function' ? value.bind(target) : value;
  },
});
const injected = lazily(injectedNow);
// PWTEST_UNDER_TEST, as Playwright's server has isUnderTest(): window.builtins.
if (isUnderTest)
  injectedNow();
const serializeAsCallArgument = module.exports.serializeAsCallArgument();
const parseEvaluationResultValue = module.exports.parseEvaluationResultValue();
const renderAriaSnapshotAsYaml = module.exports.renderAriaSnapshotAsYaml();
const parseAriaSnapshotUnsafe = module.exports.parseAriaSnapshotUnsafe();
// Playwright's module prelude keeps each export as a function that gives
// it, a namespace's too.
let yamlNamespace;
const yaml = lazily(() => yamlNamespace ??= Object.fromEntries(Object.entries(module.exports.yaml()).map(([name, get]) => [name, get()])));
const builtins = lazily(() => injectedNow().utils.builtins);
const interceptors = new Map();
let nextInterceptor = 1;
// Kept: a page may replace them or give prototypes a toJSON, and the runner's
// answers are its own.
const parseJson = JSON.parse;
const stringifyJson = JSON.stringify;
const arrayPrototype = Array.prototype;
const objectPrototype = Object.prototype;
// JSON of a plain tree (what serialize() and the steps give), past any toJSON
// the page has put on a prototype.
function treeToJson(value) {
  if (arrayPrototype.toJSON === undefined && objectPrototype.toJSON === undefined && Date.prototype.toJSON === dateToJson)
    return stringifyJson(value);
  const write = v => {
    if (v === null || v === undefined)
      return 'null';
    if (typeof v !== 'object')
      return stringifyJson(v) ?? 'null';
    if (v instanceof Date)
      return stringifyJson(dateToJson.call(v));
    let text = '';
    if (Array.isArray(v)) {
      for (let i = 0; i < v.length; ++i)
        text += (i ? ',' : '') + write(v[i]);
      return '[' + text + ']';
    }
    for (const key of Object.keys(v)) {
      if (v[key] !== undefined && typeof v[key] !== 'function')
        text += (text ? ',' : '') + stringifyJson(key) + ':' + write(v[key]);
    }
    return '{' + text + '}';
  };
  return write(value);
}
const dateToJson = Date.prototype.toJSON;

// Waits between tries, as Playwright's retryWithProgressAndBackoff(), and
// between action retries, as _retryAction().
const kPollWaits = [0, 20, 50, 100, 100, 500];
const kActionWaits = [0, 20, 100, 100, 500];
const sleep = ms => new Promise(resolve => builtins.setTimeout(resolve, ms));

// An error retrying will not mend: the command fails with it.
class FatalError extends Error {}
const fatal = message => new FatalError(message);

// Tries @attempt until it returns something other than undefined. Gives
// {timedOut, log} when @timeout ms (0: none) pass first; @log is what the
// tries said, for the call log of Playwright's timeout error.
async function poll(timeout, waits, attempt) {
  const start = builtins.performance.now();
  if (timeout) {
    // As Playwright: a short timeout keeps small waits.
    waits = waits.filter((wait, i) => i === 0 || wait <= timeout / 5);
  }
  // Every line, as Playwright's progress keeps them: the error compresses
  // what repeats ("3 × waiting for ...").
  const log = [];
  const say = line => {
    if (log.length < 1000)
      log.push(line);
  };
  for (let i = 0; ; ++i) {
    const wait = waits[Math.min(i, waits.length - 1)];
    if (timeout && builtins.performance.now() - start + wait >= timeout)
      return { timedOut: true, log };
    if (wait)
      await sleep(wait);
    const result = await attempt(say, i, wait);
    if (result !== undefined)
      return { result, log };
  }
}

function query(arg, strict) {
  const parsed = injected.parseSelector(arg.selector);
  const root = arg.scope || document;
  if (arg.scope && !arg.scope.isConnected)
    throw fatal('Element is not attached to the DOM');
  const elements = injected.querySelectorAll(parsed, root);
  if (strict && elements.length > 1)
    throw injected.strictModeViolationError(parsed, elements);
  return elements;
}

// The element an action acts on, as frames.ts resolves it once and then
// retries the action on it: "locator resolved to" when it is found anew.
function resolveForAction(arg, say, state) {
  const elements = query(arg, arg.strict);
  const element = elements[0];
  if (!element) {
    state.element = undefined;
    return undefined;
  }
  if (state.element !== element) {
    state.element = element;
    say(elements.length > 1
      ? `locator resolved to ${elements.length} elements. Proceeding with the first one: ${injected.previewNode(element)}`
      : `locator resolved to ${injected.previewNode(element)}`);
  }
  return element;
}

// dom.ts's _retryAction() lines: "attempting click action" the first time,
// then "retrying click action" and the wait before it.
function actionTry(arg, say, state, wait) {
  const name = `${arg.name || arg.action || 'click'} action${arg.trial ? ' (trial run)' : ''}`;
  if (!state.tries++ && !arg.attempt) {
    say(`attempting ${name}`);
    return;
  }
  say(`retrying ${name}`);
  if (wait)
    say(`  waiting ${wait}ms`);
}

// The element a command acts on, with the log line Playwright writes.
function resolve(arg, say) {
  const elements = query(arg, arg.strict);
  const element = elements[0];
  if (!element) {
    say(`waiting for ${arg.selector}`);
    return undefined;
  }
  say(elements.length > 1
    ? `locator resolved to ${elements.length} elements. Proceeding with the first one: ${injected.previewNode(element)}`
    : `locator resolved to ${injected.previewNode(element)}`);
  return element;
}

async function checkStates(element, states, say) {
  const missing = await injected.checkElementStates(element, states, true);
  if (missing === 'error:notconnected') {
    say('element was detached from the DOM, retrying');
    return false;
  }
  if (missing) {
    say(missing.missingState === 'non-inert' ? 'element is inert' : `element is not ${missing.missingState}`);
    return false;
  }
  return true;
}

// What _waitForFunctionOnSelector()'s callbacks in frames.ts return.
const readers = {
  textContent: element => element.textContent,
  innerText: element => {
    if (element.namespaceURI !== 'http://www.w3.org/1999/xhtml')
      throw fatal('Node is not an HTMLElement');
    return element.innerText;
  },
  innerHTML: element => element.innerHTML,
  getAttribute: (element, arg) => element.getAttribute(arg.name),
  inputValue: element => {
    const target = injected.retarget(element, 'follow-label');
    if (!target || (target.nodeName !== 'INPUT' && target.nodeName !== 'TEXTAREA' && target.nodeName !== 'SELECT'))
      throw fatal('Node is not an <input>, <textarea> or <select> element');
    return target.value;
  },
  elementState: (element, arg) => {
    const state = injected.elementState(element, arg.state);
    if (state.received === 'error:notconnected')
      throw fatal('Element is not attached to the DOM');
    return state.matches;
  },
};

// Scrolls @element into view if it is not, as DOM.scrollIntoViewIfNeeded
// does (centred, at once); later tries cycle through the alignments
// _retryPointerAction() uses against position: sticky overlays.
function scrollIntoView(element, attempt) {
  const alignments = [undefined, 'end', 'center', 'start'];
  const alignment = alignments[attempt % alignments.length];
  // InspectorDOMAgent::scrollIntoViewIfNeeded(): an element with no box of
  // its own (display: contents) is scrolled to as its first child in layout.
  if (element.nodeType === 1 && !element.getClientRects().length &&
      getComputedStyle(element).display === 'contents') {
    for (let child = element.firstChild; child; child = child.nextSibling) {
      if (child.nodeType === 1 && (child.getClientRects().length || getComputedStyle(child).display === 'contents')) {
        scrollIntoView(child, attempt);
        return;
      }
      if (child.nodeType === 3 && child.data.trim()) {
        const range = document.createRange();
        range.selectNodeContents(child);
        const box = range.getBoundingClientRect();
        if (alignment || box.top < 0 || box.left < 0 || box.bottom > innerHeight || box.right > innerWidth)
          scrollBy({ left: box.left + box.width / 2 - innerWidth / 2, top: box.top + box.height / 2 - innerHeight / 2, behavior: 'instant' });
        return;
      }
    }
  }
  if (alignment) {
    element.scrollIntoView({ block: alignment, inline: alignment, behavior: 'instant' });
    return;
  }
  const rect = element.getBoundingClientRect();
  const inside = rect.top >= 0 && rect.left >= 0 && rect.bottom <= innerHeight && rect.right <= innerWidth;
  if (!inside)
    element.scrollIntoView({ block: 'center', inline: 'center', behavior: 'instant' });
}

// dom.ts's _clickablePoint(): the middle of the element's first box with
// some area inside the viewport.
function clickablePoint(element) {
  const rects = [...element.getClientRects()];
  if (!rects.length)
    return 'error:notvisible';
  for (const rect of rects) {
    const left = Math.max(rect.left, 0), top = Math.max(rect.top, 0);
    const right = Math.min(rect.right, innerWidth), bottom = Math.min(rect.bottom, innerHeight);
    if ((right - left) * (bottom - top) > 0.99)
      return { x: (left + right) / 2, y: (top + bottom) / 2 };
  }
  return 'error:notinviewport';
}

// dom.ts's _offsetPoint(): @position from the padding box's corner.
function offsetPoint(element, position) {
  const rect = element.getBoundingClientRect();
  if (!rect.width && !rect.height)
    return 'error:notvisible';
  const border = injected.getElementBorderWidth(element);
  return { x: rect.x + border.left + position.x, y: rect.y + border.top + position.y };
}

const roundPoint = point => ({ x: Math.floor(point.x * 100) / 100, y: Math.floor(point.y * 100) / 100 });

const messages = {
  'error:notvisible': 'element is not visible',
  'error:notinviewport': 'element is outside of the viewport',
};

// The frame's offset in its parent's frame, for points in nested frames:
// the browser adds the offsets of the frames above.
function frameOffset() {
  const owner = window.frameElement;
  if (!owner)
    return { x: 0, y: 0 };
  const rect = owner.getBoundingClientRect();
  const border = injected.getElementBorderWidth(owner);
  return { x: rect.left + border.left, y: rect.top + border.top };
}

const steps = {};

steps.queryCount = arg => {
  try {
    return query(arg, false).length;
  } catch (e) {
    // frames.ts's queryCount(): 0, unless the selector is wrong.
    if (e instanceof FatalError || /selector|engine|Unexpected/i.test(e.message))
      throw e;
    return 0;
  }
};

// Frame.querySelector, ElementHandle.querySelector.
steps.query = (arg, out) => {
  const element = query(arg, arg.strict)[0];
  return element ? out(element) : null;
};

steps.queryAll = (arg, out) => query(arg, false).map(out);

steps.isVisible = arg => {
  const element = query(arg, arg.strict)[0];
  return element ? injected.elementState(element, 'visible', arg.frameVisible !== false).matches : false;
};

steps.read = arg => poll(arg.timeout, kPollWaits, say => {
  const element = resolve(arg, say);
  if (!element)
    return undefined;
  return { value: readers[arg.what](element, arg) };
});

// frames.ts's waitForSelector(); the element, unless omitReturnValue.
steps.waitForSelector = (arg, out) => {
  const state = arg.state || 'visible';
  return poll(arg.timeout, kPollWaits, say => {
    const element = query(arg, arg.strict)[0];
    const visible = element ? injected.elementState(element, 'visible', arg.frameVisible !== false).matches : false;
    if (element)
      say(`locator resolved to ${visible ? 'visible' : 'hidden'} ${injected.previewNode(element)}`);
    const success = { attached: !!element, detached: !element, visible, hidden: !visible }[state];
    if (!success)
      return undefined;
    const keep = element && (state === 'attached' || state === 'visible') && !arg.omitReturnValue;
    return { element: keep ? out(element) : null };
  });
};

// ElementHandle.waitForElementState.
steps.waitForElementState = arg => poll(arg.timeout, kPollWaits, async say => {
  const element = resolve(arg, say);
  if (!element)
    return undefined;
  if (arg.state === 'stable')
    return (await checkStates(element, ['stable'], say)) ? { done: true } : undefined;
  const state = injected.elementState(element, arg.state, arg.frameVisible !== false);
  if (state.received === 'error:notconnected')
    throw fatal('Element is not attached to the DOM');
  if (!state.matches) {
    say(`element is not ${arg.state}`);
    return undefined;
  }
  return { done: true };
});

// dom.ts's _performPointerAction() up to the input itself: the element's
// states, scrolled into view, the point, the hit-target check and its
// interceptor. Gives {point, interceptor}: the browser sends the input,
// then pointerFinish says whether it reached the element.
steps.pointerPrepare = arg => {
  const state = { tries: 0 };
  return poll(arg.timeout, kActionWaits, async (say, attempt, wait) => {
  const element = arg.scope && !arg.selector ? resolve(arg, say) : resolveForAction(arg, say, state);
  if (!element)
    return undefined;
  actionTry(arg, say, state, wait);
  const inner = line => say('  ' + line);
  if (arg.checked !== undefined) {
    // _setChecked(): nothing to do when it is already so.
    const state = injected.elementState(element, 'checked');
    if (state.received === 'error:notconnected')
      return undefined;
    if (state.matches === arg.checked)
      return { done: true };
    if (!arg.checked && state.isRadio)
      throw fatal('Cannot uncheck radio button. Radio buttons can only be unchecked by selecting another radio button in the same group.');
  }
  if (!arg.force) {
    const states = arg.waitForEnabled ? ['visible', 'enabled', 'stable'] : ['visible', 'stable'];
    const wanted = arg.waitForEnabled ? 'visible, enabled and stable' : 'visible and stable';
    inner(`waiting for element to be ${wanted}`);
    if (!(await checkStates(element, states, inner)))
      return undefined;
    inner(`element is ${wanted}`);
  } else {
    inner('forcing action');
  }
  if (arg.scroll !== 'none') {
    inner('scrolling into view if needed');
    scrollIntoView(element, attempt + (arg.attempt || 0));
    inner('done scrolling');
  }
  const point = arg.position ? offsetPoint(element, arg.position) : clickablePoint(element);
  if (typeof point === 'string') {
    if (arg.force)
      throw fatal(point === 'error:notvisible' ? 'Element is not visible' : 'Element is outside of the viewport');
    inner(messages[point]);
    return undefined;
  }
  const rounded = roundPoint(point);
  if (arg.force)
    return { point: rounded, offset: frameOffset() };
  const interceptor = injected.setupHitTargetInterceptor(element, arg.action, rounded, !!arg.trial);
  if (interceptor === 'error:notconnected')
    return undefined;
  if (typeof interceptor === 'string') {
    inner(`${interceptor} intercepts pointer events`);
    return undefined;
  }
  const id = nextInterceptor++;
  interceptors.set(id, interceptor);
  return { point: rounded, interceptor: id, offset: frameOffset() };
  });
};

// dom.ts's _drop(): a DataTransfer with the files and the data, and the
// drag events at the point; 'not-accepted' when dragover was not cancelled.
steps.drop = arg => {
  const element = query(arg, arg.strict)[0];
  if (!element || !element.isConnected || element.nodeType !== 1)
    return 'error:notconnected';
  const dt = new DataTransfer();
  for (const p of arg.payloads) {
    const bytes = Uint8Array.from(atob(p.buffer), c => c.charCodeAt(0));
    dt.items.add(new File([bytes], p.name, { type: p.mimeType, lastModified: p.lastModifiedMs }));
  }
  for (const entry of arg.data)
    dt.setData(entry.mimeType, entry.value);
  const makeEvent = type => new DragEvent(type, {
    bubbles: true,
    cancelable: true,
    composed: true,
    clientX: arg.point.x,
    clientY: arg.point.y,
    dataTransfer: dt,
  });
  element.dispatchEvent(makeEvent('dragenter'));
  const over = makeEvent('dragover');
  element.dispatchEvent(over);
  if (!over.defaultPrevented) {
    element.dispatchEvent(makeEvent('dragleave'));
    return 'not-accepted';
  }
  element.dispatchEvent(makeEvent('drop'));
  return 'accepted';
};

steps.pointerFinish = arg => {
  const interceptor = interceptors.get(arg.interceptor);
  interceptors.delete(arg.interceptor);
  const result = interceptor ? interceptor.stop() : 'done';
  let checked;
  if (arg.selector && arg.checked !== undefined) {
    const element = query(arg, arg.strict)[0];
    checked = element ? injected.elementState(element, 'checked').matches : undefined;
  }
  return { result, checked };
};

// dom.ts's _fill(): the injected script's fill, and the text the agent
// then commits as an IME does (automation_renderer.cc).
steps.fill = arg => poll(arg.timeout, kActionWaits, async say => {
  const element = resolve(arg, say);
  if (!element)
    return undefined;
  if (!arg.force && !(await checkStates(element, ['visible', 'enabled', 'editable'], say)))
    return undefined;
  const filled = injected.fill(element, arg.value);
  if (filled === 'error:notconnected')
    return undefined;
  if (filled === 'needsinput')
    return arg.value ? { insertText: arg.value } : { pressDelete: true };
  return { done: true };
});

// The element focused before keys are sent: _focus(), with the selection
// reset as _type() and _press() ask.
steps.focus = arg => poll(arg.timeout, kPollWaits, say => {
  const element = resolve(arg, say);
  if (!element)
    return undefined;
  return injected.focusNode(element, !!arg.resetSelection) === 'done' ? { done: true } : undefined;
});

steps.blur = arg => poll(arg.timeout, kPollWaits, say => {
  const element = resolve(arg, say);
  if (!element)
    return undefined;
  return injected.blurNode(element) === 'done' ? { done: true } : undefined;
});

// dom.ts's selectText().
steps.selectText = arg => poll(arg.timeout, kActionWaits, async say => {
  const element = resolve(arg, say);
  if (!element)
    return undefined;
  if (!arg.force && !(await checkStates(element, ['visible'], say)))
    return undefined;
  return injected.selectText(element) === 'done' ? { done: true } : undefined;
});

// dom.ts's _selectOption(): options by value or label, by index, or as
// elements.
steps.selectOption = arg => poll(arg.timeout, kActionWaits, async say => {
  const element = resolve(arg, say);
  if (!element)
    return undefined;
  if (!arg.force && !(await checkStates(element, ['visible', 'enabled'], say)))
    return undefined;
  const selected = injected.selectOptions(element, [...(arg.elements || []), ...(arg.options || [])]);
  if (Array.isArray(selected))
    return { values: selected };
  if (selected === 'error:optionsnotfound')
    say('did not find some options');
  else if (selected === 'error:optionnotenabled')
    say('option being selected is not enabled');
  return undefined;
});

// dom.ts's _setInputFiles(): the input checked, then given the files: their
// contents here, paths by the agent (automation_renderer.cc).
steps.setInputFiles = (arg, out) => poll(arg.timeout, kPollWaits, say => {
  // An input's own handle, one out of the document too (a file chooser's,
  // as DOM.setFileInputFiles takes it).
  const element = arg.scope && arg.selector === ':scope' ? arg.scope : resolve(arg, say);
  if (!element)
    return undefined;
  const input = injected.retarget(element, 'follow-label');
  if (!input)
    return undefined;
  if (input.tagName !== 'INPUT')
    throw fatal('Node is not an HTMLInputElement');
  const count = (arg.payloads || arg.localPaths || []).length;
  if (count > 1 && !input.multiple && !input.webkitdirectory)
    throw fatal('Non-multiple file input can only accept single file');
  if (arg.localDirectory && !input.webkitdirectory)
    throw fatal('File input does not support directories, pass individual files instead');
  if (!arg.localDirectory && input.webkitdirectory)
    throw fatal('[webkitdirectory] input requires passing a path to a directory');
  if (arg.payloads) {
    injected.setInputFiles(input, arg.payloads);
    return { done: true };
  }
  return { input: out(input) };
});

// frames.ts's dispatchEvent(), in the page's world.
steps.dispatchEvent = arg => poll(arg.timeout, kPollWaits, say => {
  const element = resolve(arg, say);
  if (!element)
    return undefined;
  // Playwright's serialized eventInit, its handles in arg.handles.
  const init = arg.serializedInit ? parseEvaluationResultValue(arg.eventInit, arg.handles || []) : arg.eventInit;
  injected.dispatchEvent(element, arg.type, init || {});
  return { done: true };
});

// dom.ts's scrollIntoViewIfNeeded(): stable, then scrolled.
// _waitAndScrollIntoViewIfNeeded(): retried as an action named "scroll into
// view"; an element of display: contents, which has no box, by its first
// descendant that has one, as DOM.scrollIntoViewIfNeeded does.
steps.scrollIntoViewIfNeeded = arg => {
  const state = { tries: 0 };
  const named = { ...arg, name: 'scroll into view', trial: false };
  return poll(arg.timeout, kActionWaits, async (say, attempt, wait) => {
    const element = resolveForAction(arg, say, state);
    if (!element)
      return undefined;
    actionTry(named, say, state, wait);
    const inner = line => say('  ' + line);
    inner('waiting for element to be stable');
    if (!(await checkStates(element, ['stable'], inner)))
      return undefined;
    let target = element;
    if (!target.getClientRects().length) {
      const walker = document.createTreeWalker(element, NodeFilter.SHOW_ELEMENT);
      for (let node = walker.nextNode(); node; node = walker.nextNode()) {
        if (node.getClientRects().length) {
          target = node;
          break;
        }
      }
    }
    if (!target.getClientRects().length) {
      inner('element is not visible');
      return undefined;
    }
    scrollIntoView(target, attempt);
    return { done: true };
  });
};

// Page.webStorage*.
steps.webStorage = arg => {
  const storage = arg.kind === 'session' ? sessionStorage : localStorage;
  switch (arg.command) {
    case 'webStorageItems': {
      const items = [];
      for (let i = 0; i < storage.length; ++i) {
        const name = storage.key(i);
        items.push({ name, value: storage.getItem(name) });
      }
      return { items };
    }
    case 'webStorageGetItem': {
      const value = storage.getItem(arg.name);
      return value === null ? {} : { value };
    }
    case 'webStorageSetItem':
      storage.setItem(arg.name, arg.value);
      return {};
    case 'webStorageRemoveItem':
      storage.removeItem(arg.name);
      return {};
    default:
      storage.clear();
      return {};
  }
};

// Frame.frameElement, here in the parent: the agent has found the child's
// element, in any tree of this document.
// The input a file chooser is of (page.ts's _onFileChooserOpened()): it, and
// whether it takes several files.
steps.chooserInput = (arg, out) => {
  if (!arg.owner)
    throw fatal('Element is not attached to the DOM');
  return { element: out(arg.owner), multiple: !!arg.owner.multiple };
};

steps.frameOwner = (arg, out) => {
  if (!arg.owner)
    throw fatal('Frame has been detached.');
  return { element: out(arg.owner) };
};

// ElementHandle.screenshot's wait, screenshotter.ts's
// _waitAndScrollIntoViewIfNeeded(progress, waitForVisible): visible, stable, in view.
steps.scrollIntoViewForScreenshot = arg => poll(arg.timeout, kActionWaits, async (say, attempt) => {
  const element = resolve(arg, say);
  if (!element)
    return undefined;
  if (!(await checkStates(element, ['visible', 'stable'], say)))
    return undefined;
  scrollIntoView(element, attempt);
  return { done: true };
});

// screenshotter.ts's inPagePrepareForScreenshots() and the masks, undone by
// restoreScreenshot; and what the clip needs of the page.
let screenshotUndo = [];
// screenshotter.ts's inPagePrepareForScreenshots(): the style in every root,
// shadow ones too; the caret hidden (unless caret is "initial") on what can have
// one; finite animations finished and infinite ones cancelled until after, those
// that start meanwhile too; the masks as Playwright's highlight draws them; then
// the fonts loaded.
steps.prepareScreenshot = async arg => {
  // What a screenshot that timed out left.
  steps.restoreScreenshot();
  const hideCaret = arg.caret !== 'initial';
  const disableAnimations = arg.animations === 'disabled';
  if (arg.style || hideCaret || disableAnimations) {
    const roots = [];
    const collect = root => {
      roots.push(root);
      const walker = document.createTreeWalker(root, NodeFilter.SHOW_ELEMENT);
      do {
        const node = walker.currentNode;
        const shadow = node instanceof Element ? node.shadowRoot : null;
        if (shadow)
          collect(shadow);
      } while (walker.nextNode());
    };
    collect(document);
    if (arg.style) {
      for (const root of roots) {
        const tag = document.createElement('style');
        tag.textContent = arg.style;
        if (root === document)
          document.documentElement.append(tag);
        else
          root.append(tag);
        screenshotUndo.push(() => tag.remove());
      }
    }
    if (hideCaret) {
      const elements = new Map();
      for (const root of roots) {
        root.querySelectorAll('input,textarea,[contenteditable]').forEach(element => {
          elements.set(element, { value: element.style.getPropertyValue('caret-color'), priority: element.style.getPropertyPriority('caret-color') });
          element.style.setProperty('caret-color', 'transparent', 'important');
        });
      }
      screenshotUndo.push(() => {
        for (const [element, value] of elements)
          element.style.setProperty('caret-color', value.value, value.priority);
      });
    }
    if (disableAnimations) {
      const resume = new Set();
      const handle = root => {
        for (const animation of root.getAnimations()) {
          if (!animation.effect || animation.playbackRate === 0 || resume.has(animation))
            continue;
          const endTime = animation.effect.getComputedTiming().endTime;
          if (Number.isFinite(endTime)) {
            try { animation.finish(); } catch (e) {}
          } else {
            try { animation.cancel(); resume.add(animation); } catch (e) {}
          }
        }
      };
      for (const root of roots) {
        const handleRoot = () => handle(root);
        handleRoot();
        root.addEventListener('transitionrun', handleRoot);
        root.addEventListener('animationstart', handleRoot);
        screenshotUndo.push(() => {
          root.removeEventListener('transitionrun', handleRoot);
          root.removeEventListener('animationstart', handleRoot);
        });
      }
      screenshotUndo.push(() => {
        for (const animation of resume) {
          try { animation.play(); } catch (e) {}
        }
      });
    }
  }
  if (arg.mask && arg.mask.length) {
    const elements = [];
    for (const selector of arg.mask)
      elements.push(...injected.querySelectorAll(injected.parseSelector(selector), document));
    injected.addMaskedElements(elements, arg.maskColor || '#F0F');
    screenshotUndo.push(() => injected.hideHighlight());
  }
  try {
    await document.fonts.ready;
  } catch (e) {
  }
  return {};
};
steps.restoreScreenshot = () => {
  const undo = screenshotUndo;
  screenshotUndo = [];
  for (const step of undo.reverse())
    step();
  return {};
};
steps.screenshotMetrics = () => {
  const root = document.documentElement, body = document.body;
  return {
    scrollX: scrollX, scrollY: scrollY,
    viewportWidth: innerWidth, viewportHeight: innerHeight,
    documentWidth: Math.max(body?.scrollWidth ?? 0, root.scrollWidth, body?.offsetWidth ?? 0, root.offsetWidth, body?.clientWidth ?? 0, root.clientWidth),
    documentHeight: Math.max(body?.scrollHeight ?? 0, root.scrollHeight, body?.offsetHeight ?? 0, root.offsetHeight, body?.clientHeight ?? 0, root.clientHeight),
    deviceScaleFactor: devicePixelRatio,
    // Page.getLayoutMetrics's visualViewport (crPage.ts's takeScreenshot()).
    visualScale: visualViewport ? visualViewport.scale : 1,
    visualPageX: visualViewport ? visualViewport.pageLeft : scrollX,
    visualPageY: visualViewport ? visualViewport.pageTop : scrollY,
  };
};

// ElementHandle.boundingBox: in the main frame's viewport, as
// DOM.getBoxModel gives it (the browser adds the frames' offsets).
steps.boundingBox = arg => {
  const element = query(arg, false)[0];
  if (!element || !element.isConnected)
    return null;
  const rect = element.getBoundingClientRect();
  if (!element.getClientRects().length)
    return null;
  const offset = frameOffset();
  return { x: rect.x, y: rect.y, width: rect.width, height: rect.height, offset };
};

// ariaSnapshot: of the element (waiting for it) or of the whole frame.
steps.ariaSnapshot = arg => {
  const options = { mode: arg.mode || 'default', depth: arg.depth, boxes: arg.boxes };
  const render = element => {
    const json = injected.ariaSnapshotJSON(element, options).json;
    return arg.json ? json : renderAriaSnapshotAsYaml(json);
  };
  if (!arg.selector)
    return { snapshot: render(document.body || document.documentElement) };
  return poll(arg.timeout, kPollWaits, say => {
    const element = resolve(arg, say);
    return element ? { snapshot: render(element) } : undefined;
  });
};

// page.ts's ariaSnapshotJSONForFrame(): one frame's snapshot, with the refs
// of the iframes it shows, for the browser to stitch theirs in.
steps.ariaSnapshotTree = arg => {
  const options = { mode: arg.mode || 'default', depth: arg.depth, boxes: arg.boxes, doNotRenderActive: arg.doNotRenderActive };
  return poll(arg.timeout, kPollWaits, say => {
    const element = resolve(arg, say);
    if (!element)
      return undefined;
    const snapshot = injected.ariaSnapshotJSON(element, options);
    return { tree: { json: snapshot.json, iframeRefs: snapshot.iframeRefs, iframeDepths: snapshot.iframeDepths } };
  });
};

// The stitched snapshot as the client gets it (frames.ts's ariaSnapshot()).
steps.renderAria = arg => renderAriaSnapshotAsYaml(arg.json);

// server/callLog.ts: repetitions as "N x", each line as the client prints it.
function compressCallLog(log) {
  const lines = [];
  const same = (from, to, length) => {
    for (let i = 0; i < length; ++i) {
      if (log[from + i] !== log[to + i])
        return false;
    }
    return true;
  };
  for (let i = 0; i < log.length;) {
    let bestCount = 1, bestLength = 1;
    for (let length = 1; length <= log.length - i; ++length) {
      let count = 1;
      while (i + length * (count + 1) <= log.length && same(i, i + length * count, length))
        ++count;
      if (count > 1 && count * length > bestCount * bestLength) {
        bestCount = count;
        bestLength = length;
      }
    }
    for (let at = 0; at < bestLength; ++at) {
      const line = log[i + at];
      const prefix = '  ' + line.match(/^\s*/)[0];
      const counted = `${bestCount} \u00d7 `;
      if (bestCount > 1 && at === 0)
        lines[lines.length] = prefix + counted + line.trim();
      else if (bestCount > 1)
        lines[lines.length] = prefix + ' '.repeat(counted.length - 2) + '- ' + line.trim();
      else
        lines[lines.length] = prefix + '- ' + line.trim();
    }
    i += bestCount * bestLength;
  }
  return lines;
}

// frames.ts's expect() and _expectInternal(): one check at once, then
// again until it matches or the time is out. The answer is what
// Playwright's FrameExpectResult has: matches, received, timedOut, log.
steps.expect = async arg => {
  const options = { ...arg.options };
  const timeout = options.expectTimeout ?? arg.timeout;
  const title = options.expectTitle;
  delete options.expectTimeout;
  delete options.expectTitle;
  // The endpoint's: serialized, and its received value is to be serialized too.
  const serialized = options.expectedValueSerialized;
  delete options.expectedValueSerialized;
  if (serialized)
    options.expectedValue = parseEvaluationResultValue(options.expectedValue, []);
  const answered = result => {
    if (serialized && result.received && 'value' in result.received)
      result.received = { ...result.received, value: serialize(result.received.value) };
    if (title !== undefined && result.log)
      result.log = compressCallLog(result.log);
    return result;
  };
  if (options.expression === 'to.match.aria' && options.expectedValue) {
    try {
      options.expectedValue = parseAriaSnapshotUnsafe(yaml, options.expectedValue);
    } catch (e) {
      return { matches: options.isNot, errorMessage: e.message };
    }
  }
  const isArray = options.expression === 'to.have.count' || options.expression.endsWith('.array');
  const selector = arg.selector ?? (options.expression === 'to.match.aria' ? 'body' : ':root');
  let last;
  const once = async say => {
    const elements = query({ selector, scope: arg.scope }, !isArray);
    if (!elements.length) {
      let matches = options.isNot, received;
      let missing = false;
      if (options.expression === 'to.have.count') {
        say('  locator resolved to 0 elements');
        received = { value: 0 };
        matches = options.expectedNumber === 0;
      } else if (options.expression.endsWith('.array')) {
        say('  locator resolved to 0 elements');
        received = { value: [] };
        matches = (options.expectedText?.length ?? 0) === 0;
      } else if (!options.isNot && ['to.be.hidden', 'to.be.detached'].includes(options.expression)) {
        matches = true;
      } else if (options.isNot && ['to.be.visible', 'to.be.attached', 'to.be.in.viewport'].includes(options.expression)) {
        matches = false;
      } else {
        missing = true;
      }
      if (matches === options.isNot && !isArray && document.body)
        received = { ...received, ariaSnapshot: injected.ariaSnapshotForExpectFailure(document.body, { mode: 'default' }) };
      return { matches, received, errorMessage: missing ? 'element(s) not found' : undefined };
    }
    say(isArray
      ? `  locator resolved to ${elements.length} element${elements.length === 1 ? '' : 's'}`
      : `  locator resolved to ${injected.previewNode(elements[0])}`);
    const result = await injected.expect(elements[0], options, elements, arg.frameVisible !== false);
    if (result.matches === options.isNot && !Array.isArray(result.received?.value))
      say(`  unexpected value "${options.expression === 'to.match.aria' ? result.received?.value?.raw : result.received?.value}"`);
    return result;
  };
  const log = [];
  const say = line => {
    if (log[log.length - 1] !== line)
      log.push(line);
  };
  // dispatchers/frameDispatcher.ts: the call's title, first in its log.
  if (title !== undefined) {
    const locator = arg.selector === undefined ? '' : ' ' + asLocator('javascript', arg.displaySelector ?? arg.selector);
    log.push(`${title}${locator}${timeout ? ` with timeout ${timeout}ms` : ''}`);
    if (arg.selector !== undefined)
      log.push(`waiting for${locator}`);
  }
  // frames.ts: an error in the page, an invalid selector or a strictness
  // violation ends the assertion; only a mismatch retries.
  try {
    last = await once(say);
    if (last.matches !== options.isNot)
      return answered({ ...last, log });
  } catch (e) {
    return answered({ matches: options.isNot, errorMessage: e.message, log });
  }
  let thrown;
  const polled = await poll(timeout, kPollWaits, async () => {
    try {
      last = await once(say);
    } catch (e) {
      thrown = e;
      return last = { matches: !options.isNot };
    }
    return last.matches !== options.isNot ? last : undefined;
  });
  if (thrown)
    return answered({ matches: options.isNot, errorMessage: thrown.message, log });
  if (polled.timedOut)
    return answered({ matches: options.isNot, received: last?.received, errorMessage: last?.errorMessage, timedOut: true, log });
  return answered({ ...polled.result, log });
};

// frames.ts's setContent(): the document written, then its load.
steps.setContent = async arg => {
  document.open();
  document.write(arg.html);
  document.close();
  const ready = arg.waitUntil === 'domcontentloaded' ? ['interactive', 'complete'] : ['complete'];
  if (arg.waitUntil !== 'commit' && !ready.includes(document.readyState)) {
    await new Promise(resolve => {
      const check = () => ready.includes(document.readyState) && resolve();
      document.addEventListener('readystatechange', check);
      builtins.setTimeout(check, 0);
    });
  }
  return { done: true };
};

// frames.ts's addScriptTag() and addStyleTag(): the element, once loaded.
steps.addTag = (arg, out) => new Promise((fulfill, reject) => {
  if (!arg.url && typeof arg.content !== 'string') {
    reject(fatal('Provide an object with a `url`, `path` or `content` property'));
    return;
  }
  const script = arg.tag === 'script';
  const element = document.createElement(script ? 'script' : arg.url ? 'link' : 'style');
  if (script) {
    if (arg.type)
      element.type = arg.type;
    if (arg.url)
      element.src = arg.url;
    else
      element.text = arg.content;
  } else if (arg.url) {
    element.rel = 'stylesheet';
    element.href = arg.url;
  } else {
    element.appendChild(document.createTextNode(arg.content));
  }
  const done = () => fulfill({ element: out(element) });
  if (arg.url) {
    element.onload = done;
    element.onerror = () => reject(fatal(script ? 'Failed to load script at ' + arg.url : 'Error: Failed to load stylesheet'));
  }
  // frames.ts's _raceWithCSPError(): a policy that refuses it is an error.
  let refused = null;
  const violation = e => { refused = e; };
  document.addEventListener('securitypolicyviolation', violation);
  (document.head || document.documentElement).appendChild(element);
  if (!arg.url) {
    builtins.setTimeout(() => {
      document.removeEventListener('securitypolicyviolation', violation);
      if (refused)
        reject(fatal(`Refused to apply inline ${script ? 'script' : 'style'}: it violates the Content Security Policy directive "${refused.violatedDirective}"`));
      else
        done();
    }, 0);
  }
});

// A console call's arguments, which the browser left under @key on the
// global object (Runtime.callFunctionOn), as handles (crPage.ts's
// _onConsoleAPI()).
steps.consoleArgs = (arg, out) => {
  const values = globalThis[arg.key] || [];
  delete globalThis[arg.key];
  const handles = [];
  for (let i = 0; i < values.length; ++i)
    handles[i] = out(values[i]);
  return handles;
};

// frames.ts's resolveSelector(): the element's own selector, or null.
steps.generateSelector = arg => {
  const element = query(arg, false)[0];
  return element ? injected.generateSelectorSimple(element) : null;
};

// The options a step carries, alone (before a call that is not a step).
steps.noop = () => true;
steps.title = () => document.title;
// A selector as Playwright's asLocator() writes it, for a call log.
steps.locatorText = arg => asLocator('javascript', arg.selector);
steps.content = () => injected.documentContent(false);
steps.viewport = () => ({ width: innerWidth, height: innerHeight });
steps.frameOffset = () => frameOffset();

// Where the content box of a child frame's <iframe>, which the agent has found,
// is in this frame's viewport: what the browser adds to a point of the child's
// to have it in this frame (automation.cc's ToRoot()).
steps.childFrameBox = arg => {
  const owner = arg.owner;
  if (!owner)
    return { x: 0, y: 0 };
  const rect = owner.getBoundingClientRect();
  const style = owner.ownerDocument.defaultView.getComputedStyle(owner);
  return {
    x: rect.left + owner.clientLeft + parseFloat(style.paddingLeft || '0'),
    y: rect.top + owner.clientTop + parseFloat(style.paddingTop || '0'),
  };
};

// ---- Evaluation, as javascript.ts and utilityScript.ts do it -------------

// What crExecutionContext.ts's rewriteError() says when the protocol cannot
// carry the value back. V8's inspector gives up beyond this nesting of objects
// and arrays (measured on Chrome: {o: [{k, v}]} 98 levels, {a: []} 148).
const kMaxSerializedDepth = 298;
function checkDepth(tree) {
  let level = [tree];
  for (let depth = 0; level.length; ++depth) {
    const next = [];
    for (const item of level) {
      if (item === null || typeof item !== 'object')
        continue;
      if (depth >= kMaxSerializedDepth)
        throw new Error('Cannot serialize result: object reference chain is too long.');
      for (const key of Object.keys(item))
        next.push(item[key]);
    }
    level = next;
  }
}

// A value as Playwright's protocol serializes it, or as JSON.
function serialize(value, plain) {
  if (!plain) {
    // What is not serializable (a function, a symbol) is undefined, as
    // Runtime.callFunctionOn's returnByValue gives Playwright.
    // A function inside the value, which the serializer leaves out ({k} with
    // no v), is undefined too: Playwright's server parses the page's value and
    // serializes it again for its client, whose validator takes no hole.
    const result = serializeAsCallArgument(value, v =>
      typeof v === 'function' && !String(v.name).startsWith('__pw_fn_') ? { v: 'undefined' } : { fallThrough: v });
    if (result === undefined)
      return { v: 'undefined' };
    checkDepth(result);
    return result;
  }
  return parseJson(stringifyJson(value === undefined ? null : value, (key, v) => {
    if (typeof v === 'bigint')
      return v.toString();
    if (typeof v === 'number' && !Number.isFinite(v))
      return String(v);
    if (v instanceof Node)
      return injected.previewNode(v);
    return v;
  }) ?? 'null');
}

// A node of any document: Node of this realm is not that of another frame's, but
// its getter takes any. A forged {nodeType, nodeName} is no node, and a Proxy runs
// none of its traps here: the getter throws on both.
// (A worker has no nodes; a page may have nulled its Node: from the document.)
const nodeTypeOf = (() => {
  for (let prototype = globalThis.document; prototype; prototype = Object.getPrototypeOf(prototype)) {
    const getter = Object.getOwnPropertyDescriptor(prototype, 'nodeType')?.get;
    if (getter)
      return getter;
  }
})();
const applyGetter = Reflect.apply;
const isNode = value => {
  if (!nodeTypeOf || !value || typeof value !== 'object')
    return false;
  try {
    return applyGetter(nodeTypeOf, value, []) > 0;
  } catch (e) {
    return false;
  }
};

// What Playwright shows of a handle (JSHandle@object, Array(3), the node, as
// dom.ts's ElementHandle sets its preview).
function preview(value) {
  if (isNode(value)) {
    try {
      return 'JSHandle@' + injected.previewNode(value);
    } catch (e) {
      return 'JSHandle@node';
    }
  }
  return describe(value);
}

// crExecutionContext.ts's renderPreview() over what Runtime's remote object
// says of a value, which has no preview unless asked for: its value or its
// description.
function describe(value) {
  switch (typeof value) {
    case 'string':
    case 'boolean':
    case 'undefined':
      return String(value);
    case 'number':
      return Object.is(value, -0) ? '-0' : String(value);
    case 'bigint':
      return value + 'n';
    case 'symbol':
      return value.toString();
    case 'function':
      return Function.prototype.toString.call(value);
  }
  if (value === null)
    return 'null';
  const tag = Object.prototype.toString.call(value).slice(8, -1);
  if (Array.isArray(value))
    return `Array(${value.length})`;
  if (value instanceof RegExp)
    return String(value);
  if (value instanceof Date)
    return String(value);
  if (value instanceof Error)
    return String(value.stack || value);
  if (value instanceof Map || value instanceof Set)
    return `${tag}(${value.size})`;
  if (ArrayBuffer.isView(value) && !(value instanceof DataView))
    return `${tag}(${value.length})`;
  return value.constructor?.name || tag;
}
// The argument: Playwright's serialized value with {h: i} for handles, or
// JSON with {"$h": i}.
function argument(arg, handles, plain) {
  if (!plain)
    return parseEvaluationResultValue(arg, handles);
  const revive = v => {
    if (Array.isArray(v))
      return v.map(revive);
    if (v && typeof v === 'object') {
      if (Object.keys(v).length === 1 && typeof v.$h === 'number')
        return handles[v.$h];
      const o = {};
      for (const k of Object.keys(v))
        o[k] = revive(v[k]);
      return o;
    }
    return v;
  };
  return revive(arg);
}

// An evaluation's arguments, without Array.prototype.map, the page's to break.
function parametersOf(args, handles, plain) {
  // A function among them ({fn}, evaluate's exposeFunctions) calls the binding
  // the client exposed under that name, through this (bindingsController.ts).
  if (!plain && !globalThis.__playwright__binding__controller__) {
    Object.defineProperty(globalThis, '__playwright__binding__controller__', {
      configurable: true,
      value: {
        callBinding: (name, ...callArgs) => globalThis[name](...callArgs),
        parseInitScriptArg: value => parseEvaluationResultValue(value),
      },
    });
  }
  const list = Array.isArray(args) ? args : [args];
  const parameters = new Array(list.length);
  for (let i = 0; i < list.length; ++i)
    parameters[i] = argument(list[i], handles, plain);
  return parameters;
}

// One answer of the runner, the handles it keeps collected by out().
async function answer(body, evaluation) {
  const handles = [];
  const previews = [];
  const nodes = [];
  const out = value => {
    const index = handles.length;
    handles[index] = value;
    previews[index] = preview(value);
    nodes[index] = isNode(value);
    return { $h: index };
  };
  try {
    const value = await body(out);
    return [treeToJson({ ok: true, value: value === undefined ? null : value }), handles, previews, nodes];
  } catch (e) {
    // An evaluation's error is the page's, as Playwright shows it ("Error:
    // boom"); a step's is Playwright's own message.
    // The injected script's own errors ("Malformed value", "strict mode
    // violation") reach Playwright's client as its evaluations' do: "Error: ...".
    const message = !(e instanceof Error) ? (evaluation ? 'Error: ' : '') + String(e)
      : e instanceof FatalError ? e.message
      : e.name + ': ' + e.message;
    return [stringifyJson({ ok: false, error: message, fatal: e instanceof FatalError || /strict mode violation/.test(message) }), handles, previews, nodes];
  }
}

return {
  // A binding call's arguments as Playwright's page serializes them
  // (bindingsController.ts's callBinding()), with the page's JSON.stringify
  // as it has it.
  bindingArgs(args) {
    const serialized = [];
    for (let i = 0; i < args.length; i++)
      serialized[i] = serializeAsCallArgument(args[i], v => ({ fallThrough: v }));
    return JSON.stringify(serialized);
  },

  // A binding call's answer, Playwright's serialized value whose {h: i} are
  // @handles[i] (deliverBindingResult()).
  bindingResult(json, handles) {
    return parseEvaluationResultValue(parseJson(json), handles);
  },

  // A step: steps[name](arg, out), @handles what arg's {"$h": i} are.
  step(name, argJson, handles) {
    return answer(out => {
      const step = steps[name];
      if (!step)
        throw fatal(`zebkit: no step ${name}`);
      const parsed = parseJson(argJson);
      if (parsed.selectorOptions)
        useSelectorOptions(parsed.selectorOptions);
      // frames.ts's Frame.seq, the prefix of the frame's ARIA refs.
      if (parsed.frameSeq !== undefined && parsed.frameSeq !== options.frameSeq) {
        options.frameSeq = parsed.frameSeq;
        if (injectedScript)
          injectedScript._frameSeq = parsed.frameSeq;
      }
      const arg = argument(parsed, handles, true);
      return step(arg, out);
    });
  },

  // utilityScript.ts's evaluate(), with the expression compiled by the
  // agent (no eval(), which a page's Content Security Policy may forbid):
  // @compiled is its value. The result is a handle unless @returnByValue.
  evaluate(compiled, isFunction, argJson, handles, returnByValue, plain) {
    return answer(async out => {
      const args = parseJson(argJson);
      const parameters = parametersOf(args, handles, plain);
      let result = compiled;
      if (isFunction === true || (isFunction === undefined && typeof result === 'function'))
        result = await result(...parameters);
      else
        result = await result;
      if (!returnByValue)
        return out(result);
      return serialize(result, plain);
    }, true);
  },

  // JSHandle.getProperty and getPropertyList, jsonValue.
  properties(object, names) {
    return answer(out => {
      if (names)
        return { [names]: out(object == null ? undefined : object[names]) };
      const result = {};
      if (object && typeof object === 'object') {
        for (const key of Object.keys(object))
          result[key] = out(object[key]);
      }
      return result;
    });
  },
  jsonValue(object, plain) {
    return answer(() => serialize(object, plain));
  },

  // frames.ts's waitForFunctionExpression(): the predicate polled at each
  // animation frame or every @polling ms, until it is truthy; its value
  // as a handle.
  waitForFunction(compiled, isFunction, argJson, handles, polling, timeout, plain, thunk) {
    return answer(async out => {
      const args = parseJson(argJson);
      // locator.waitForFunction(): the agent puts the locator first; the
      // function takes its element, found again at each try.
      const on = Array.isArray(args) && args[0]?.waitOnSelector !== undefined ? args.shift() : null;
      const parameters = parametersOf(args, handles, plain);
      const predicate = () => {
        if (on) {
          const element = query({ selector: on.waitOnSelector, strict: on.strict })[0];
          return element ? compiled(element, ...parameters) : undefined;
        }
        if (thunk) {
          // frames.ts's predicate: the expression's value, a function's
          // kept and called each time.
          let result = evaluated ?? compiled();
          if (isFunction === undefined && typeof result === 'function') {
            evaluated = result;
            result = result(...parameters);
          }
          return result;
        }
        if (isFunction === true || (isFunction === undefined && typeof compiled === 'function'))
          return compiled(...parameters);
        return compiled;
      };
      let evaluated;
      const start = builtins.performance.now();
      return await new Promise((fulfill, reject) => {
        const next = () => {
          try {
            const value = predicate();
            if (value) {
              fulfill(out(value));
              return;
            }
            if (timeout && builtins.performance.now() - start >= timeout) {
              fulfill({ timedOut: true });
              return;
            }
            if (typeof polling === 'number')
              builtins.setTimeout(next, polling);
            else
              builtins.requestAnimationFrame(next);
          } catch (e) {
            reject(e);
          }
        };
        next();
      });
    }, true);
  },

  // The element-based reads Frame.evalOnSelector and evalOnSelectorAll
  // evaluate on, in the page's world.
  select(selector, strict, scope, all) {
    return answer(out => {
      const elements = query({ selector, scope }, strict && !all);
      if (all)
        return out(elements);
      if (!elements.length)
        throw fatal(`Failed to find element matching selector "${selector}"`);
      return out(elements[0]);
    });
  },
};
