// Playwright's injected script, bundled by scripts/playwright-sources.mjs
// from https://github.com/microsoft/playwright at cfe490d941f992655948eee8fd4fd94e1c4234bf.
// Copyright (c) Microsoft Corporation. Licensed under the Apache License,
// Version 2.0 (src/engine/playwright/LICENSE). Generated: do not edit.

var __commonJS = obj => {
  let required = false;
  let result;
  return function __require() {
    if (!required) {
      required = true;
      let fn;
      for (const name in obj) { fn = obj[name]; break; }
      const module = { exports: {} };
      fn(module.exports, module);
      result = module.exports;
    }
    return result;
  }
};
var __export = (target, all) => {for (var name in all) target[name] = all[name];};
var __toESM = mod => ({ ...mod, 'default': mod });
var __toCommonJS = mod => ({ ...mod, __esModule: true });


// zebkit-injected.ts
var zebkit_injected_exports = {};
__export(zebkit_injected_exports, {
  InjectedScript: () => InjectedScript,
  parseAriaSnapshotUnsafe: () => parseAriaSnapshotUnsafe,
  parseEvaluationResultValue: () => parseEvaluationResultValue,
  renderAriaSnapshotAsYaml: () => renderAriaSnapshotAsYaml,
  serializeAsCallArgument: () => serializeAsCallArgument,
  yaml: () => browser_exports
});
module.exports = __toCommonJS(zebkit_injected_exports);

// packages/isomorphic/ariaSnapshot.ts
function hasPointerCursor(ariaNode) {
  return ariaNode.box.cursor === "pointer";
}
function parseAriaSnapshotUnsafe(yaml, text, options = {}) {
  const result = parseAriaSnapshot(yaml, text, options);
  if (result.errors.length)
    throw new Error(result.errors[0].message);
  return result.fragment;
}
function parseAriaSnapshot(yaml, text, options = {}) {
  var _a;
  const lineCounter = new yaml.LineCounter();
  const parseOptions2 = {
    keepSourceTokens: true,
    lineCounter,
    ...options
  };
  const yamlDoc = yaml.parseDocument(text, parseOptions2);
  const errors = [];
  const convertRange = (range) => {
    return [lineCounter.linePos(range[0]), lineCounter.linePos(range[1])];
  };
  const addError = (error) => {
    errors.push({
      message: error.message,
      range: [lineCounter.linePos(error.pos[0]), lineCounter.linePos(error.pos[1])]
    });
  };
  const convertSeq = (container, seq2) => {
    for (const item of seq2.items) {
      const itemIsString = item instanceof yaml.Scalar && typeof item.value === "string";
      if (itemIsString) {
        const childNode = KeyParser.parse(item, parseOptions2, errors);
        if (childNode) {
          container.children = container.children || [];
          container.children.push(childNode);
        }
        continue;
      }
      const itemIsMap = item instanceof yaml.YAMLMap;
      if (itemIsMap) {
        convertMap(container, item);
        continue;
      }
      errors.push({
        message: "Sequence items should be strings or maps",
        range: convertRange(item.range || seq2.range)
      });
    }
  };
  const convertMap = (container, map2) => {
    var _a2;
    for (const entry of map2.items) {
      container.children = container.children || [];
      const keyIsString = entry.key instanceof yaml.Scalar && typeof entry.key.value === "string";
      if (!keyIsString) {
        errors.push({
          message: "Only string keys are supported",
          range: convertRange(entry.key.range || map2.range)
        });
        continue;
      }
      const key = entry.key;
      const value = entry.value;
      if (key.value === "text") {
        const valueIsString = value instanceof yaml.Scalar && typeof value.value === "string";
        if (!valueIsString) {
          errors.push({
            message: "Text value should be a string",
            range: convertRange(entry.value.range || map2.range)
          });
          continue;
        }
        container.children.push({
          kind: "text",
          text: textValue(value.value)
        });
        continue;
      }
      if (key.value === "/children") {
        const valueIsString = value instanceof yaml.Scalar && typeof value.value === "string";
        if (!valueIsString || value.value !== "contain" && value.value !== "equal" && value.value !== "deep-equal") {
          errors.push({
            message: 'Strict value should be "contain", "equal" or "deep-equal"',
            range: convertRange(entry.value.range || map2.range)
          });
          continue;
        }
        container.containerMode = value.value;
        continue;
      }
      if (key.value.startsWith("/")) {
        const valueIsString = value instanceof yaml.Scalar && typeof value.value === "string";
        if (!valueIsString) {
          errors.push({
            message: "Property value should be a string",
            range: convertRange(entry.value.range || map2.range)
          });
          continue;
        }
        container.props = (_a2 = container.props) != null ? _a2 : {};
        container.props[key.value.slice(1)] = textValue(value.value);
        continue;
      }
      const childNode = KeyParser.parse(key, parseOptions2, errors);
      if (!childNode)
        continue;
      const valueIsScalar = value instanceof yaml.Scalar;
      if (valueIsScalar) {
        const type = typeof value.value;
        if (type !== "string" && type !== "number" && type !== "boolean") {
          errors.push({
            message: "Node value should be a string or a sequence",
            range: convertRange(entry.value.range || map2.range)
          });
          continue;
        }
        container.children.push({
          ...childNode,
          children: [{
            kind: "text",
            text: textValue(String(value.value))
          }]
        });
        continue;
      }
      const valueIsSequence = value instanceof yaml.YAMLSeq;
      if (valueIsSequence) {
        container.children.push(childNode);
        convertSeq(childNode, value);
        continue;
      }
      errors.push({
        message: "Map values should be strings or sequences",
        range: convertRange(entry.value.range || map2.range)
      });
    }
  };
  const fragment = { kind: "role", role: "fragment" };
  yamlDoc.errors.forEach(addError);
  if (errors.length)
    return { errors, fragment };
  if (!(yamlDoc.contents instanceof yaml.YAMLSeq)) {
    errors.push({
      message: 'Aria snapshot must be a YAML sequence, elements starting with " -"',
      range: yamlDoc.contents ? convertRange(yamlDoc.contents.range) : [{ line: 0, col: 0 }, { line: 0, col: 0 }]
    });
  }
  if (errors.length)
    return { errors, fragment };
  convertSeq(fragment, yamlDoc.contents);
  if (errors.length)
    return { errors, fragment: emptyFragment };
  if (((_a = fragment.children) == null ? void 0 : _a.length) === 1 && (!fragment.containerMode || fragment.containerMode === "contain"))
    return { fragment: fragment.children[0], errors: [] };
  return { fragment, errors: [] };
}
var emptyFragment = { kind: "role", role: "fragment" };
function normalizeWhitespace(text) {
  return text.replace(/[\u200b\u00ad]/g, "").replace(/[\r\n\s\t]+/g, " ").trim();
}
function textValue(value) {
  return {
    raw: value,
    normalized: normalizeWhitespace(value)
  };
}
var KeyParser = class _KeyParser {
  static parse(text, options, errors) {
    try {
      return new _KeyParser(text.value)._parse();
    } catch (e) {
      if (e instanceof ParserError) {
        const message = options.prettyErrors === false ? e.message : e.message + ":\n\n" + text.value + "\n" + " ".repeat(e.pos) + "^\n";
        errors.push({
          message,
          range: [options.lineCounter.linePos(text.range[0]), options.lineCounter.linePos(text.range[0] + e.pos)]
        });
        return null;
      }
      throw e;
    }
  }
  constructor(input) {
    this._input = input;
    this._pos = 0;
    this._length = input.length;
  }
  _peek() {
    return this._input[this._pos] || "";
  }
  _next() {
    if (this._pos < this._length)
      return this._input[this._pos++];
    return null;
  }
  _eof() {
    return this._pos >= this._length;
  }
  _isWhitespace() {
    return !this._eof() && /\s/.test(this._peek());
  }
  _skipWhitespace() {
    while (this._isWhitespace())
      this._pos++;
  }
  _readIdentifier(type) {
    if (this._eof())
      this._throwError(`Unexpected end of input when expecting ${type}`);
    const start = this._pos;
    while (!this._eof() && /[a-zA-Z]/.test(this._peek()))
      this._pos++;
    return this._input.slice(start, this._pos);
  }
  _readString() {
    let result = "";
    let escaped = false;
    while (!this._eof()) {
      const ch = this._next();
      if (escaped) {
        result += ch;
        escaped = false;
      } else if (ch === "\\") {
        escaped = true;
      } else if (ch === '"') {
        return result;
      } else {
        result += ch;
      }
    }
    this._throwError("Unterminated string");
  }
  _throwError(message, offset = 0) {
    throw new ParserError(message, offset || this._pos);
  }
  _readRegex() {
    let result = "";
    let escaped = false;
    let insideClass = false;
    while (!this._eof()) {
      const ch = this._next();
      if (escaped) {
        result += ch;
        escaped = false;
      } else if (ch === "\\") {
        escaped = true;
        result += ch;
      } else if (ch === "/" && !insideClass) {
        return { pattern: result };
      } else if (ch === "[") {
        insideClass = true;
        result += ch;
      } else if (ch === "]" && insideClass) {
        result += ch;
        insideClass = false;
      } else {
        result += ch;
      }
    }
    this._throwError("Unterminated regex");
  }
  _readStringOrRegex() {
    const ch = this._peek();
    if (ch === '"') {
      this._next();
      return normalizeWhitespace(this._readString());
    }
    if (ch === "/") {
      this._next();
      return this._readRegex();
    }
    return null;
  }
  _readAttributes(result) {
    let errorPos = this._pos;
    while (true) {
      this._skipWhitespace();
      if (this._peek() === "[") {
        this._next();
        this._skipWhitespace();
        errorPos = this._pos;
        const flagName = this._readIdentifier("attribute");
        this._skipWhitespace();
        let flagValue = "";
        if (this._peek() === "=") {
          this._next();
          this._skipWhitespace();
          errorPos = this._pos;
          while (this._peek() !== "]" && !this._isWhitespace() && !this._eof())
            flagValue += this._next();
        }
        this._skipWhitespace();
        if (this._peek() !== "]")
          this._throwError("Expected ]");
        this._next();
        this._applyAttribute(result, flagName, flagValue || "true", errorPos);
      } else {
        break;
      }
    }
  }
  _parse() {
    this._skipWhitespace();
    const role = this._readIdentifier("role");
    this._skipWhitespace();
    const name = this._readStringOrRegex() || "";
    const result = { kind: "role", role, name };
    this._readAttributes(result);
    this._skipWhitespace();
    if (!this._eof())
      this._throwError("Unexpected input");
    return result;
  }
  _applyAttribute(node, key, value, errorPos) {
    if (key === "checked") {
      this._assert(value === "true" || value === "false" || value === "mixed", 'Value of "checked" attribute must be a boolean or "mixed"', errorPos);
      node.checked = value === "true" ? true : value === "false" ? false : "mixed";
      return;
    }
    if (key === "disabled") {
      this._assert(value === "true" || value === "false", 'Value of "disabled" attribute must be a boolean', errorPos);
      node.disabled = value === "true";
      return;
    }
    if (key === "expanded") {
      this._assert(value === "true" || value === "false", 'Value of "expanded" attribute must be a boolean', errorPos);
      node.expanded = value === "true";
      return;
    }
    if (key === "active") {
      this._assert(value === "true" || value === "false", 'Value of "active" attribute must be a boolean', errorPos);
      node.active = value === "true";
      return;
    }
    if (key === "invalid") {
      this._assert(value === "true" || value === "false" || value === "grammar" || value === "spelling", 'Value of "invalid" attribute must be a boolean, "grammar" or "spelling"', errorPos);
      node.invalid = value === "true" ? true : value === "false" ? false : value;
      return;
    }
    if (key === "level") {
      this._assert(!isNaN(Number(value)), 'Value of "level" attribute must be a number', errorPos);
      node.level = Number(value);
      return;
    }
    if (key === "pressed") {
      this._assert(value === "true" || value === "false" || value === "mixed", 'Value of "pressed" attribute must be a boolean or "mixed"', errorPos);
      node.pressed = value === "true" ? true : value === "false" ? false : "mixed";
      return;
    }
    if (key === "selected") {
      this._assert(value === "true" || value === "false", 'Value of "selected" attribute must be a boolean', errorPos);
      node.selected = value === "true";
      return;
    }
    this._assert(false, `Unsupported attribute [${key}]`, errorPos);
  }
  _assert(value, message, valuePos) {
    if (!value)
      this._throwError(message || "Assertion error", valuePos);
  }
};
var ParserError = class extends Error {
  constructor(message, pos) {
    super(message);
    this.pos = pos;
  }
};
function findNewNode(from, to) {
  var _a, _b;
  function fillMap(root, map2, position) {
    let size = 1;
    let childPosition = position + size;
    for (const child of root.children || []) {
      if (typeof child === "string") {
        size++;
        childPosition++;
      } else {
        size += fillMap(child, map2, childPosition);
        childPosition += size;
      }
    }
    if (!["none", "presentation", "fragment", "iframe", "generic"].includes(root.role) && root.name) {
      let byRole = map2.get(root.role);
      if (!byRole) {
        byRole = /* @__PURE__ */ new Map();
        map2.set(root.role, byRole);
      }
      const existing = byRole.get(root.name);
      const sizeAndPosition = size * 100 - position;
      if (!existing || existing.sizeAndPosition < sizeAndPosition)
        byRole.set(root.name, { node: root, sizeAndPosition });
    }
    return size;
  }
  const fromMap = /* @__PURE__ */ new Map();
  if (from)
    fillMap(from, fromMap, 0);
  const toMap = /* @__PURE__ */ new Map();
  fillMap(to, toMap, 0);
  const result = [];
  for (const [role, byRole] of toMap) {
    for (const [name, byName] of byRole) {
      const inFrom = (_a = fromMap.get(role)) == null ? void 0 : _a.get(name);
      if (!inFrom)
        result.push(byName);
    }
  }
  result.sort((a, b) => b.sizeAndPosition - a.sizeAndPosition);
  return (_b = result[0]) == null ? void 0 : _b.node;
}

// packages/isomorphic/stringUtils.ts
function escapeWithQuotes(text, char = "'") {
  const stringified = JSON.stringify(text);
  const escapedText = stringified.substring(1, stringified.length - 1).replace(/\\"/g, '"');
  if (char === "'")
    return char + escapedText.replace(/[']/g, "\\'") + char;
  if (char === '"')
    return char + escapedText.replace(/["]/g, '\\"') + char;
  if (char === "`")
    return char + escapedText.replace(/[`]/g, "\\`") + char;
  throw new Error("Invalid escape char");
}
function toTitleCase(name) {
  return name.charAt(0).toUpperCase() + name.substring(1);
}
function toSnakeCase(name) {
  return name.replace(/([a-z0-9])([A-Z])/g, "$1_$2").replace(/([A-Z])([A-Z][a-z])/g, "$1_$2").toLowerCase();
}
function quoteCSSAttributeValue(text) {
  return `"${text.replace(/["\\]/g, (char) => "\\" + char)}"`;
}
var normalizedWhitespaceCache;
function cacheNormalizedWhitespaces() {
  normalizedWhitespaceCache = /* @__PURE__ */ new Map();
}
function normalizeWhiteSpace(text) {
  let result = normalizedWhitespaceCache == null ? void 0 : normalizedWhitespaceCache.get(text);
  if (result === void 0) {
    result = text.replace(/[\u200b\u00ad]/g, "").trim().replace(/\s+/g, " ");
    normalizedWhitespaceCache == null ? void 0 : normalizedWhitespaceCache.set(text, result);
  }
  return result;
}
function normalizeEscapedRegexQuotes(source) {
  return source.replace(/(^|[^\\])(\\\\)*\\(['"`])/g, "$1$2$3");
}
function escapeRegexForSelector(re) {
  if (re.unicode || re.unicodeSets)
    return String(re);
  return String(re).replace(/(^|[^\\])(\\\\)*(["'`])/g, "$1$2\\$3").replace(/>>/g, "\\>\\>");
}
function escapeForTextSelector(text, exact) {
  if (typeof text !== "string")
    return escapeRegexForSelector(text);
  return `${JSON.stringify(text)}${exact ? "s" : "i"}`;
}
function escapeForAttributeSelector(value, exact) {
  if (typeof value !== "string")
    return escapeRegexForSelector(value);
  return `"${value.replace(/\\/g, "\\\\").replace(/["]/g, '\\"')}"${exact ? "s" : "i"}`;
}
function trimString(input, cap, suffix = "") {
  if (input.length <= cap)
    return input;
  const chars = [...input];
  if (chars.length > cap)
    return chars.slice(0, cap - suffix.length).join("") + suffix;
  return chars.join("");
}
function trimStringWithEllipsis(input, cap) {
  return trimString(input, cap, "\u2026");
}
function truncateDataUrl(url) {
  if (!url.startsWith("data:"))
    return url;
  const comma = url.indexOf(",");
  if (comma === -1)
    return url;
  return url.slice(0, comma + 1) + "\u2026";
}
function escapeRegExp(s) {
  return s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}
function longestCommonSubstring(s1, s2) {
  const n = s1.length;
  const m = s2.length;
  let maxLen = 0;
  let endingIndex = 0;
  const dp = Array(n + 1).fill(null).map(() => Array(m + 1).fill(0));
  for (let i = 1; i <= n; i++) {
    for (let j = 1; j <= m; j++) {
      if (s1[i - 1] === s2[j - 1]) {
        dp[i][j] = dp[i - 1][j - 1] + 1;
        if (dp[i][j] > maxLen) {
          maxLen = dp[i][j];
          endingIndex = i;
        }
      }
    }
  }
  return s1.slice(endingIndex - maxLen, endingIndex);
}
var ansiRegex = new RegExp("([\\u001B\\u009B][[\\]()#?]*(?:(?:(?:[a-zA-Z\\d]*(?:;[-a-zA-Z\\d\\/#&.:=?%@~_]*)*)?\\u0007)|(?:(?:\\d{0,4}(?:;\\d{0,4})*)?[\\dA-PR-TZcf-ntqry=><~])))", "g");

// packages/isomorphic/yaml.ts
function yamlEscapeKeyIfNeeded(str) {
  if (!yamlStringNeedsQuotes(str))
    return str;
  return `'` + str.replace(/'/g, `''`) + `'`;
}
function yamlEscapeValueIfNeeded(str) {
  if (!yamlStringNeedsQuotes(str))
    return str;
  return '"' + str.replace(/[\\"\x00-\x1f\x7f-\x9f]/g, (c) => {
    switch (c) {
      case "\\":
        return "\\\\";
      case '"':
        return '\\"';
      case "\b":
        return "\\b";
      case "\f":
        return "\\f";
      case "\n":
        return "\\n";
      case "\r":
        return "\\r";
      case "	":
        return "\\t";
      default:
        const code = c.charCodeAt(0);
        return "\\x" + code.toString(16).padStart(2, "0");
    }
  }) + '"';
}
function yamlStringNeedsQuotes(str) {
  if (str.length === 0)
    return true;
  if (/^\s|\s$/.test(str))
    return true;
  if (/[\x00-\x08\x0b\x0c\x0e-\x1f\x7f-\x9f]/.test(str))
    return true;
  if (/^-/.test(str))
    return true;
  if (/[\n:](\s|$)/.test(str))
    return true;
  if (/\s#/.test(str))
    return true;
  if (/[\n\r]/.test(str))
    return true;
  if (/^[&*\],?!>|@"'#%]/.test(str))
    return true;
  if (/[{}`]/.test(str))
    return true;
  if (/^\[/.test(str))
    return true;
  if (!isNaN(Number(str)) || ["y", "n", "yes", "no", "true", "false", "on", "off", "null"].includes(str.toLowerCase()))
    return true;
  return false;
}

// packages/isomorphic/ariaSnapshotRenderer.ts
function renderAriaSnapshotAsYaml(snapshot, options = {}) {
  const lines = [];
  const includeText = options.convertStringsToRegex ? textContributesInfo : () => true;
  const renderString = options.convertStringsToRegex ? convertToBestGuessRegex : (str) => str;
  const visitText = (text, depth) => {
    const escaped = yamlEscapeValueIfNeeded(renderString(text));
    if (escaped)
      lines.push(indent(depth) + "- text: " + escaped);
  };
  const createKey = (node) => {
    let key = node.role;
    if (node.name && node.name.length <= 900) {
      const name = renderString(node.name);
      if (name) {
        const isRegex = options.convertStringsToRegex && name !== node.name;
        key += " " + (isRegex ? name : JSON.stringify(name));
      }
    }
    if (node.checked === "mixed")
      key += ` [checked=mixed]`;
    if (node.checked === true)
      key += ` [checked]`;
    if (node.disabled)
      key += ` [disabled]`;
    if (node.expanded)
      key += ` [expanded]`;
    if (node.active)
      key += ` [active]`;
    if (node.invalid === "grammar" || node.invalid === "spelling")
      key += ` [invalid=${node.invalid}]`;
    if (node.invalid === true)
      key += ` [invalid]`;
    if (node.level)
      key += ` [level=${node.level}]`;
    if (node.pressed === "mixed")
      key += ` [pressed=mixed]`;
    if (node.pressed === true)
      key += ` [pressed]`;
    if (node.selected === true)
      key += ` [selected]`;
    if (node.ariaHidden)
      key += ` [aria-hidden]`;
    if (node.ref) {
      key += ` [ref=${node.ref}]`;
      if (node.cursor === "pointer")
        key += " [cursor=pointer]";
    }
    if (node.box)
      key += ` [box=${node.box.x},${node.box.y},${node.box.width},${node.box.height}]`;
    return key;
  };
  const visit3 = (node, depth) => {
    var _a, _b;
    if (node.role === "text") {
      visitText(node.text || "", depth);
      return;
    }
    (_a = options.lineToNode) == null ? void 0 : _a.set(lines.length, node);
    const escapedKey = indent(depth) + "- " + yamlEscapeKeyIfNeeded(createKey(node));
    const props = [];
    if (node.url !== void 0)
      props.push(["url", node.url]);
    if (node.placeholder !== void 0)
      props.push(["placeholder", node.placeholder]);
    if (node.text === void 0 && !props.length && !((_b = node.children) == null ? void 0 : _b.length)) {
      lines.push(escapedKey);
    } else if (node.text !== void 0 && !props.length) {
      if (includeText(node, node.text))
        lines.push(escapedKey + ": " + yamlEscapeValueIfNeeded(renderString(node.text)));
      else
        lines.push(escapedKey);
    } else {
      lines.push(escapedKey + ":");
      for (const [name, value] of props)
        lines.push(indent(depth + 1) + "- /" + name + ": " + yamlEscapeValueIfNeeded(value));
      if (node.text !== void 0) {
        visitText(includeText(node, node.text) ? node.text : "", depth + 1);
      } else {
        for (const child of node.children || []) {
          if (typeof child === "string")
            visitText(includeText(node, child) ? child : "", depth + 1);
          else
            visit3(child, depth + 1);
        }
      }
    }
  };
  for (const node of snapshot)
    visit3(node, 0);
  return lines.join("\n");
}
function indent(depth) {
  return "  ".repeat(depth);
}
function convertToBestGuessRegex(text) {
  const dynamicContent = [
    // 550e8400-e29b-41d4-a716-446655440000
    { regex: /\b[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\b/, replacement: "[0-9a-fA-F-]+" },
    // 2mb
    { regex: /\b[\d,.]+[bkmBKM]+\b/, replacement: "[\\d,.]+[bkmBKM]+" },
    // 2ms, 20s
    { regex: /\b\d+[hmsp]+\b/, replacement: "\\d+[hmsp]+" },
    { regex: /\b[\d,.]+[hmsp]+\b/, replacement: "[\\d,.]+[hmsp]+" },
    // Do not replace single digits with regex by default.
    // 2+ digits: [Issue 22, 22.3, 2.33, 2,333]
    { regex: /\b\d+,\d+\b/, replacement: "\\d+,\\d+" },
    { regex: /\b\d+\.\d{2,}\b/, replacement: "\\d+\\.\\d+" },
    { regex: /\b\d{2,}\.\d+\b/, replacement: "\\d+\\.\\d+" },
    { regex: /\b\d{2,}\b/, replacement: "\\d+" }
  ];
  let pattern = "";
  let lastIndex = 0;
  const combinedRegex = new RegExp(dynamicContent.map((r) => "(" + r.regex.source + ")").join("|"), "g");
  text.replace(combinedRegex, (match, ...args) => {
    const offset = args[args.length - 2];
    const groups = args.slice(0, -2);
    pattern += escapeRegExp(text.slice(lastIndex, offset));
    for (let i = 0; i < groups.length; i++) {
      if (groups[i]) {
        const { replacement } = dynamicContent[i];
        pattern += replacement;
        break;
      }
    }
    lastIndex = offset + match.length;
    return match;
  });
  if (!pattern)
    return text;
  pattern += escapeRegExp(text.slice(lastIndex));
  return String(new RegExp(pattern));
}
function textContributesInfo(node, text) {
  if (!text.length)
    return false;
  if (!node.name)
    return true;
  const substr = text.length <= 200 && node.name.length <= 200 ? longestCommonSubstring(text, node.name) : "";
  let filtered = text;
  while (substr && filtered.includes(substr))
    filtered = filtered.replace(substr, "");
  return filtered.trim().length / text.length > 0.1;
}

// packages/isomorphic/cssTokenizer.ts
var between = function(num, first, last) {
  return num >= first && num <= last;
};
function digit(code) {
  return between(code, 48, 57);
}
function hexdigit(code) {
  return digit(code) || between(code, 65, 70) || between(code, 97, 102);
}
function uppercaseletter(code) {
  return between(code, 65, 90);
}
function lowercaseletter(code) {
  return between(code, 97, 122);
}
function letter(code) {
  return uppercaseletter(code) || lowercaseletter(code);
}
function nonascii(code) {
  return code >= 128;
}
function namestartchar(code) {
  return letter(code) || nonascii(code) || code === 95;
}
function namechar(code) {
  return namestartchar(code) || digit(code) || code === 45;
}
function nonprintable(code) {
  return between(code, 0, 8) || code === 11 || between(code, 14, 31) || code === 127;
}
function newline(code) {
  return code === 10;
}
function whitespace(code) {
  return newline(code) || code === 9 || code === 32;
}
var maximumallowedcodepoint = 1114111;
var InvalidCharacterError = class extends Error {
  constructor(message) {
    super(message);
    this.name = "InvalidCharacterError";
  }
};
function preprocess(str) {
  const codepoints = [];
  for (let i = 0; i < str.length; i++) {
    let code = str.charCodeAt(i);
    if (code === 13 && str.charCodeAt(i + 1) === 10) {
      code = 10;
      i++;
    }
    if (code === 13 || code === 12)
      code = 10;
    if (code === 0)
      code = 65533;
    if (between(code, 55296, 56319) && between(str.charCodeAt(i + 1), 56320, 57343)) {
      const lead = code - 55296;
      const trail = str.charCodeAt(i + 1) - 56320;
      code = Math.pow(2, 16) + lead * Math.pow(2, 10) + trail;
      i++;
    }
    codepoints.push(code);
  }
  return codepoints;
}
function stringFromCode(code) {
  if (code <= 65535)
    return String.fromCharCode(code);
  code -= Math.pow(2, 16);
  const lead = Math.floor(code / Math.pow(2, 10)) + 55296;
  const trail = code % Math.pow(2, 10) + 56320;
  return String.fromCharCode(lead) + String.fromCharCode(trail);
}
function tokenize(str1) {
  const str = preprocess(str1);
  let i = -1;
  const tokens = [];
  let code;
  let line = 0;
  let column = 0;
  let lastLineLength = 0;
  const incrLineno = function() {
    line += 1;
    lastLineLength = column;
    column = 0;
  };
  const locStart = { line, column };
  const codepoint = function(i2) {
    if (i2 >= str.length)
      return -1;
    return str[i2];
  };
  const next = function(num) {
    if (num === void 0)
      num = 1;
    if (num > 3)
      throw "Spec Error: no more than three codepoints of lookahead.";
    return codepoint(i + num);
  };
  const consume = function(num) {
    if (num === void 0)
      num = 1;
    i += num;
    code = codepoint(i);
    if (newline(code))
      incrLineno();
    else
      column += num;
    return true;
  };
  const reconsume = function() {
    i -= 1;
    if (newline(code)) {
      line -= 1;
      column = lastLineLength;
    } else {
      column -= 1;
    }
    locStart.line = line;
    locStart.column = column;
    return true;
  };
  const eof = function(codepoint2) {
    if (codepoint2 === void 0)
      codepoint2 = code;
    return codepoint2 === -1;
  };
  const donothing = function() {
  };
  const parseerror = function() {
  };
  const consumeAToken = function() {
    consumeComments();
    consume();
    if (whitespace(code)) {
      while (whitespace(next()))
        consume();
      return new WhitespaceToken();
    } else if (code === 34) {
      return consumeAStringToken();
    } else if (code === 35) {
      if (namechar(next()) || areAValidEscape(next(1), next(2))) {
        const token = new HashToken("");
        if (wouldStartAnIdentifier(next(1), next(2), next(3)))
          token.type = "id";
        token.value = consumeAName();
        return token;
      } else {
        return new DelimToken(code);
      }
    } else if (code === 36) {
      if (next() === 61) {
        consume();
        return new SuffixMatchToken();
      } else {
        return new DelimToken(code);
      }
    } else if (code === 39) {
      return consumeAStringToken();
    } else if (code === 40) {
      return new OpenParenToken();
    } else if (code === 41) {
      return new CloseParenToken();
    } else if (code === 42) {
      if (next() === 61) {
        consume();
        return new SubstringMatchToken();
      } else {
        return new DelimToken(code);
      }
    } else if (code === 43) {
      if (startsWithANumber()) {
        reconsume();
        return consumeANumericToken();
      } else {
        return new DelimToken(code);
      }
    } else if (code === 44) {
      return new CommaToken();
    } else if (code === 45) {
      if (startsWithANumber()) {
        reconsume();
        return consumeANumericToken();
      } else if (next(1) === 45 && next(2) === 62) {
        consume(2);
        return new CDCToken();
      } else if (startsWithAnIdentifier()) {
        reconsume();
        return consumeAnIdentlikeToken();
      } else {
        return new DelimToken(code);
      }
    } else if (code === 46) {
      if (startsWithANumber()) {
        reconsume();
        return consumeANumericToken();
      } else {
        return new DelimToken(code);
      }
    } else if (code === 58) {
      return new ColonToken();
    } else if (code === 59) {
      return new SemicolonToken();
    } else if (code === 60) {
      if (next(1) === 33 && next(2) === 45 && next(3) === 45) {
        consume(3);
        return new CDOToken();
      } else {
        return new DelimToken(code);
      }
    } else if (code === 64) {
      if (wouldStartAnIdentifier(next(1), next(2), next(3)))
        return new AtKeywordToken(consumeAName());
      else
        return new DelimToken(code);
    } else if (code === 91) {
      return new OpenSquareToken();
    } else if (code === 92) {
      if (startsWithAValidEscape()) {
        reconsume();
        return consumeAnIdentlikeToken();
      } else {
        parseerror();
        return new DelimToken(code);
      }
    } else if (code === 93) {
      return new CloseSquareToken();
    } else if (code === 94) {
      if (next() === 61) {
        consume();
        return new PrefixMatchToken();
      } else {
        return new DelimToken(code);
      }
    } else if (code === 123) {
      return new OpenCurlyToken();
    } else if (code === 124) {
      if (next() === 61) {
        consume();
        return new DashMatchToken();
      } else if (next() === 124) {
        consume();
        return new ColumnToken();
      } else {
        return new DelimToken(code);
      }
    } else if (code === 125) {
      return new CloseCurlyToken();
    } else if (code === 126) {
      if (next() === 61) {
        consume();
        return new IncludeMatchToken();
      } else {
        return new DelimToken(code);
      }
    } else if (digit(code)) {
      reconsume();
      return consumeANumericToken();
    } else if (namestartchar(code)) {
      reconsume();
      return consumeAnIdentlikeToken();
    } else if (eof()) {
      return new EOFToken();
    } else {
      return new DelimToken(code);
    }
  };
  const consumeComments = function() {
    while (next(1) === 47 && next(2) === 42) {
      consume(2);
      while (true) {
        consume();
        if (code === 42 && next() === 47) {
          consume();
          break;
        } else if (eof()) {
          parseerror();
          return;
        }
      }
    }
  };
  const consumeANumericToken = function() {
    const num = consumeANumber();
    if (wouldStartAnIdentifier(next(1), next(2), next(3))) {
      const token = new DimensionToken();
      token.value = num.value;
      token.repr = num.repr;
      token.type = num.type;
      token.unit = consumeAName();
      return token;
    } else if (next() === 37) {
      consume();
      const token = new PercentageToken();
      token.value = num.value;
      token.repr = num.repr;
      return token;
    } else {
      const token = new NumberToken();
      token.value = num.value;
      token.repr = num.repr;
      token.type = num.type;
      return token;
    }
  };
  const consumeAnIdentlikeToken = function() {
    const str2 = consumeAName();
    if (str2.toLowerCase() === "url" && next() === 40) {
      consume();
      while (whitespace(next(1)) && whitespace(next(2)))
        consume();
      if (next() === 34 || next() === 39)
        return new FunctionToken(str2);
      else if (whitespace(next()) && (next(2) === 34 || next(2) === 39))
        return new FunctionToken(str2);
      else
        return consumeAURLToken();
    } else if (next() === 40) {
      consume();
      return new FunctionToken(str2);
    } else {
      return new IdentToken(str2);
    }
  };
  const consumeAStringToken = function(endingCodePoint) {
    if (endingCodePoint === void 0)
      endingCodePoint = code;
    let string2 = "";
    while (consume()) {
      if (code === endingCodePoint || eof()) {
        return new StringToken(string2);
      } else if (newline(code)) {
        parseerror();
        reconsume();
        return new BadStringToken();
      } else if (code === 92) {
        if (eof(next()))
          donothing();
        else if (newline(next()))
          consume();
        else
          string2 += stringFromCode(consumeEscape());
      } else {
        string2 += stringFromCode(code);
      }
    }
    throw new Error("Internal error");
  };
  const consumeAURLToken = function() {
    const token = new URLToken("");
    while (whitespace(next()))
      consume();
    if (eof(next()))
      return token;
    while (consume()) {
      if (code === 41 || eof()) {
        return token;
      } else if (whitespace(code)) {
        while (whitespace(next()))
          consume();
        if (next() === 41 || eof(next())) {
          consume();
          return token;
        } else {
          consumeTheRemnantsOfABadURL();
          return new BadURLToken();
        }
      } else if (code === 34 || code === 39 || code === 40 || nonprintable(code)) {
        parseerror();
        consumeTheRemnantsOfABadURL();
        return new BadURLToken();
      } else if (code === 92) {
        if (startsWithAValidEscape()) {
          token.value += stringFromCode(consumeEscape());
        } else {
          parseerror();
          consumeTheRemnantsOfABadURL();
          return new BadURLToken();
        }
      } else {
        token.value += stringFromCode(code);
      }
    }
    throw new Error("Internal error");
  };
  const consumeEscape = function() {
    consume();
    if (hexdigit(code)) {
      const digits = [code];
      for (let total = 0; total < 5; total++) {
        if (hexdigit(next())) {
          consume();
          digits.push(code);
        } else {
          break;
        }
      }
      if (whitespace(next()))
        consume();
      let value = parseInt(digits.map(function(x) {
        return String.fromCharCode(x);
      }).join(""), 16);
      if (value > maximumallowedcodepoint)
        value = 65533;
      return value;
    } else if (eof()) {
      return 65533;
    } else {
      return code;
    }
  };
  const areAValidEscape = function(c1, c2) {
    if (c1 !== 92)
      return false;
    if (newline(c2))
      return false;
    return true;
  };
  const startsWithAValidEscape = function() {
    return areAValidEscape(code, next());
  };
  const wouldStartAnIdentifier = function(c1, c2, c3) {
    if (c1 === 45)
      return namestartchar(c2) || c2 === 45 || areAValidEscape(c2, c3);
    else if (namestartchar(c1))
      return true;
    else if (c1 === 92)
      return areAValidEscape(c1, c2);
    else
      return false;
  };
  const startsWithAnIdentifier = function() {
    return wouldStartAnIdentifier(code, next(1), next(2));
  };
  const wouldStartANumber = function(c1, c2, c3) {
    if (c1 === 43 || c1 === 45) {
      if (digit(c2))
        return true;
      if (c2 === 46 && digit(c3))
        return true;
      return false;
    } else if (c1 === 46) {
      if (digit(c2))
        return true;
      return false;
    } else if (digit(c1)) {
      return true;
    } else {
      return false;
    }
  };
  const startsWithANumber = function() {
    return wouldStartANumber(code, next(1), next(2));
  };
  const consumeAName = function() {
    let result = "";
    while (consume()) {
      if (namechar(code)) {
        result += stringFromCode(code);
      } else if (startsWithAValidEscape()) {
        result += stringFromCode(consumeEscape());
      } else {
        reconsume();
        return result;
      }
    }
    throw new Error("Internal parse error");
  };
  const consumeANumber = function() {
    let repr = "";
    let type = "integer";
    if (next() === 43 || next() === 45) {
      consume();
      repr += stringFromCode(code);
    }
    while (digit(next())) {
      consume();
      repr += stringFromCode(code);
    }
    if (next(1) === 46 && digit(next(2))) {
      consume();
      repr += stringFromCode(code);
      consume();
      repr += stringFromCode(code);
      type = "number";
      while (digit(next())) {
        consume();
        repr += stringFromCode(code);
      }
    }
    const c1 = next(1);
    const c2 = next(2);
    const c3 = next(3);
    if ((c1 === 69 || c1 === 101) && digit(c2)) {
      consume();
      repr += stringFromCode(code);
      consume();
      repr += stringFromCode(code);
      type = "number";
      while (digit(next())) {
        consume();
        repr += stringFromCode(code);
      }
    } else if ((c1 === 69 || c1 === 101) && (c2 === 43 || c2 === 45) && digit(c3)) {
      consume();
      repr += stringFromCode(code);
      consume();
      repr += stringFromCode(code);
      consume();
      repr += stringFromCode(code);
      type = "number";
      while (digit(next())) {
        consume();
        repr += stringFromCode(code);
      }
    }
    const value = convertAStringToANumber(repr);
    return { type, value, repr };
  };
  const convertAStringToANumber = function(string2) {
    return +string2;
  };
  const consumeTheRemnantsOfABadURL = function() {
    while (consume()) {
      if (code === 41 || eof()) {
        return;
      } else if (startsWithAValidEscape()) {
        consumeEscape();
        donothing();
      } else {
        donothing();
      }
    }
  };
  let iterationCount = 0;
  while (!eof(next())) {
    tokens.push(consumeAToken());
    iterationCount++;
    if (iterationCount > str.length * 2)
      throw new Error("I'm infinite-looping!");
  }
  return tokens;
}
var CSSParserToken = class {
  constructor() {
    this.tokenType = "";
  }
  toJSON() {
    return { token: this.tokenType };
  }
  toString() {
    return this.tokenType;
  }
  toSource() {
    return "" + this;
  }
};
var BadStringToken = class extends CSSParserToken {
  constructor() {
    super(...arguments);
    this.tokenType = "BADSTRING";
  }
};
var BadURLToken = class extends CSSParserToken {
  constructor() {
    super(...arguments);
    this.tokenType = "BADURL";
  }
};
var WhitespaceToken = class extends CSSParserToken {
  constructor() {
    super(...arguments);
    this.tokenType = "WHITESPACE";
  }
  toString() {
    return "WS";
  }
  toSource() {
    return " ";
  }
};
var CDOToken = class extends CSSParserToken {
  constructor() {
    super(...arguments);
    this.tokenType = "CDO";
  }
  toSource() {
    return "<!--";
  }
};
var CDCToken = class extends CSSParserToken {
  constructor() {
    super(...arguments);
    this.tokenType = "CDC";
  }
  toSource() {
    return "-->";
  }
};
var ColonToken = class extends CSSParserToken {
  constructor() {
    super(...arguments);
    this.tokenType = ":";
  }
};
var SemicolonToken = class extends CSSParserToken {
  constructor() {
    super(...arguments);
    this.tokenType = ";";
  }
};
var CommaToken = class extends CSSParserToken {
  constructor() {
    super(...arguments);
    this.tokenType = ",";
  }
};
var GroupingToken = class extends CSSParserToken {
  constructor() {
    super(...arguments);
    this.value = "";
    this.mirror = "";
  }
};
var OpenCurlyToken = class extends GroupingToken {
  constructor() {
    super();
    this.tokenType = "{";
    this.value = "{";
    this.mirror = "}";
  }
};
var CloseCurlyToken = class extends GroupingToken {
  constructor() {
    super();
    this.tokenType = "}";
    this.value = "}";
    this.mirror = "{";
  }
};
var OpenSquareToken = class extends GroupingToken {
  constructor() {
    super();
    this.tokenType = "[";
    this.value = "[";
    this.mirror = "]";
  }
};
var CloseSquareToken = class extends GroupingToken {
  constructor() {
    super();
    this.tokenType = "]";
    this.value = "]";
    this.mirror = "[";
  }
};
var OpenParenToken = class extends GroupingToken {
  constructor() {
    super();
    this.tokenType = "(";
    this.value = "(";
    this.mirror = ")";
  }
};
var CloseParenToken = class extends GroupingToken {
  constructor() {
    super();
    this.tokenType = ")";
    this.value = ")";
    this.mirror = "(";
  }
};
var IncludeMatchToken = class extends CSSParserToken {
  constructor() {
    super(...arguments);
    this.tokenType = "~=";
  }
};
var DashMatchToken = class extends CSSParserToken {
  constructor() {
    super(...arguments);
    this.tokenType = "|=";
  }
};
var PrefixMatchToken = class extends CSSParserToken {
  constructor() {
    super(...arguments);
    this.tokenType = "^=";
  }
};
var SuffixMatchToken = class extends CSSParserToken {
  constructor() {
    super(...arguments);
    this.tokenType = "$=";
  }
};
var SubstringMatchToken = class extends CSSParserToken {
  constructor() {
    super(...arguments);
    this.tokenType = "*=";
  }
};
var ColumnToken = class extends CSSParserToken {
  constructor() {
    super(...arguments);
    this.tokenType = "||";
  }
};
var EOFToken = class extends CSSParserToken {
  constructor() {
    super(...arguments);
    this.tokenType = "EOF";
  }
  toSource() {
    return "";
  }
};
var DelimToken = class extends CSSParserToken {
  constructor(code) {
    super();
    this.tokenType = "DELIM";
    this.value = "";
    this.value = stringFromCode(code);
  }
  toString() {
    return "DELIM(" + this.value + ")";
  }
  toJSON() {
    const json = this.constructor.prototype.constructor.prototype.toJSON.call(this);
    json.value = this.value;
    return json;
  }
  toSource() {
    if (this.value === "\\")
      return "\\\n";
    else
      return this.value;
  }
};
var StringValuedToken = class extends CSSParserToken {
  constructor() {
    super(...arguments);
    this.value = "";
  }
  ASCIIMatch(str) {
    return this.value.toLowerCase() === str.toLowerCase();
  }
  toJSON() {
    const json = this.constructor.prototype.constructor.prototype.toJSON.call(this);
    json.value = this.value;
    return json;
  }
};
var IdentToken = class extends StringValuedToken {
  constructor(val) {
    super();
    this.tokenType = "IDENT";
    this.value = val;
  }
  toString() {
    return "IDENT(" + this.value + ")";
  }
  toSource() {
    return escapeIdent(this.value);
  }
};
var FunctionToken = class extends StringValuedToken {
  constructor(val) {
    super();
    this.tokenType = "FUNCTION";
    this.value = val;
    this.mirror = ")";
  }
  toString() {
    return "FUNCTION(" + this.value + ")";
  }
  toSource() {
    return escapeIdent(this.value) + "(";
  }
};
var AtKeywordToken = class extends StringValuedToken {
  constructor(val) {
    super();
    this.tokenType = "AT-KEYWORD";
    this.value = val;
  }
  toString() {
    return "AT(" + this.value + ")";
  }
  toSource() {
    return "@" + escapeIdent(this.value);
  }
};
var HashToken = class extends StringValuedToken {
  constructor(val) {
    super();
    this.tokenType = "HASH";
    this.value = val;
    this.type = "unrestricted";
  }
  toString() {
    return "HASH(" + this.value + ")";
  }
  toJSON() {
    const json = this.constructor.prototype.constructor.prototype.toJSON.call(this);
    json.value = this.value;
    json.type = this.type;
    return json;
  }
  toSource() {
    if (this.type === "id")
      return "#" + escapeIdent(this.value);
    else
      return "#" + escapeHash(this.value);
  }
};
var StringToken = class extends StringValuedToken {
  constructor(val) {
    super();
    this.tokenType = "STRING";
    this.value = val;
  }
  toString() {
    return '"' + escapeString(this.value) + '"';
  }
};
var URLToken = class extends StringValuedToken {
  constructor(val) {
    super();
    this.tokenType = "URL";
    this.value = val;
  }
  toString() {
    return "URL(" + this.value + ")";
  }
  toSource() {
    return 'url("' + escapeString(this.value) + '")';
  }
};
var NumberToken = class extends CSSParserToken {
  constructor() {
    super();
    this.tokenType = "NUMBER";
    this.type = "integer";
    this.repr = "";
  }
  toString() {
    if (this.type === "integer")
      return "INT(" + this.value + ")";
    return "NUMBER(" + this.value + ")";
  }
  toJSON() {
    const json = super.toJSON();
    json.value = this.value;
    json.type = this.type;
    json.repr = this.repr;
    return json;
  }
  toSource() {
    return this.repr;
  }
};
var PercentageToken = class extends CSSParserToken {
  constructor() {
    super();
    this.tokenType = "PERCENTAGE";
    this.repr = "";
  }
  toString() {
    return "PERCENTAGE(" + this.value + ")";
  }
  toJSON() {
    const json = this.constructor.prototype.constructor.prototype.toJSON.call(this);
    json.value = this.value;
    json.repr = this.repr;
    return json;
  }
  toSource() {
    return this.repr + "%";
  }
};
var DimensionToken = class extends CSSParserToken {
  constructor() {
    super();
    this.tokenType = "DIMENSION";
    this.type = "integer";
    this.repr = "";
    this.unit = "";
  }
  toString() {
    return "DIM(" + this.value + "," + this.unit + ")";
  }
  toJSON() {
    const json = this.constructor.prototype.constructor.prototype.toJSON.call(this);
    json.value = this.value;
    json.type = this.type;
    json.repr = this.repr;
    json.unit = this.unit;
    return json;
  }
  toSource() {
    const source = this.repr;
    let unit = escapeIdent(this.unit);
    if (unit[0].toLowerCase() === "e" && (unit[1] === "-" || between(unit.charCodeAt(1), 48, 57))) {
      unit = "\\65 " + unit.slice(1, unit.length);
    }
    return source + unit;
  }
};
function escapeIdent(string2) {
  string2 = "" + string2;
  let result = "";
  const firstcode = string2.charCodeAt(0);
  for (let i = 0; i < string2.length; i++) {
    const code = string2.charCodeAt(i);
    if (code === 0)
      throw new InvalidCharacterError("Invalid character: the input contains U+0000.");
    if (between(code, 1, 31) || code === 127 || i === 0 && between(code, 48, 57) || i === 1 && between(code, 48, 57) && firstcode === 45)
      result += "\\" + code.toString(16) + " ";
    else if (code >= 128 || code === 45 || code === 95 || between(code, 48, 57) || between(code, 65, 90) || between(code, 97, 122))
      result += string2[i];
    else
      result += "\\" + string2[i];
  }
  return result;
}
function escapeHash(string2) {
  string2 = "" + string2;
  let result = "";
  for (let i = 0; i < string2.length; i++) {
    const code = string2.charCodeAt(i);
    if (code === 0)
      throw new InvalidCharacterError("Invalid character: the input contains U+0000.");
    if (code >= 128 || code === 45 || code === 95 || between(code, 48, 57) || between(code, 65, 90) || between(code, 97, 122))
      result += string2[i];
    else
      result += "\\" + code.toString(16) + " ";
  }
  return result;
}
function escapeString(string2) {
  string2 = "" + string2;
  let result = "";
  for (let i = 0; i < string2.length; i++) {
    const code = string2.charCodeAt(i);
    if (code === 0)
      throw new InvalidCharacterError("Invalid character: the input contains U+0000.");
    if (between(code, 1, 31) || code === 127)
      result += "\\" + code.toString(16) + " ";
    else if (code === 34 || code === 92)
      result += "\\" + string2[i];
    else
      result += string2[i];
  }
  return result;
}

// packages/isomorphic/cssParser.ts
var InvalidSelectorError = class extends Error {
};
function parseCSS(selector, customNames) {
  let tokens;
  try {
    tokens = tokenize(selector);
    if (!(tokens[tokens.length - 1] instanceof EOFToken))
      tokens.push(new EOFToken());
  } catch (e) {
    const newMessage = e.message + ` while parsing css selector "${selector}". Did you mean to CSS.escape it?`;
    const index = (e.stack || "").indexOf(e.message);
    if (index !== -1)
      e.stack = e.stack.substring(0, index) + newMessage + e.stack.substring(index + e.message.length);
    e.message = newMessage;
    throw e;
  }
  const unsupportedToken = tokens.find((token) => {
    return token instanceof AtKeywordToken || token instanceof BadStringToken || token instanceof BadURLToken || token instanceof ColumnToken || token instanceof CDOToken || token instanceof CDCToken || token instanceof SemicolonToken || // TODO: Consider using these for something, e.g. to escape complex strings.
    // For example :xpath{ (//div/bar[@attr="foo"])[2]/baz }
    // Or this way :xpath( {complex-xpath-goes-here("hello")} )
    token instanceof OpenCurlyToken || token instanceof CloseCurlyToken || // TODO: Consider treating these as strings?
    token instanceof URLToken || token instanceof PercentageToken;
  });
  if (unsupportedToken)
    throw new InvalidSelectorError(`Unsupported token "${unsupportedToken.toSource()}" while parsing css selector "${selector}". Did you mean to CSS.escape it?`);
  let pos = 0;
  const names = /* @__PURE__ */ new Set();
  function unexpected() {
    return new InvalidSelectorError(`Unexpected token "${tokens[pos].toSource()}" while parsing css selector "${selector}". Did you mean to CSS.escape it?`);
  }
  function skipWhitespace() {
    while (tokens[pos] instanceof WhitespaceToken)
      pos++;
  }
  function isIdent(p = pos) {
    return tokens[p] instanceof IdentToken;
  }
  function isString(p = pos) {
    return tokens[p] instanceof StringToken;
  }
  function isNumber(p = pos) {
    return tokens[p] instanceof NumberToken;
  }
  function isComma(p = pos) {
    return tokens[p] instanceof CommaToken;
  }
  function isOpenParen(p = pos) {
    return tokens[p] instanceof OpenParenToken;
  }
  function isCloseParen(p = pos) {
    return tokens[p] instanceof CloseParenToken;
  }
  function isFunction(p = pos) {
    return tokens[p] instanceof FunctionToken;
  }
  function isStar(p = pos) {
    return tokens[p] instanceof DelimToken && tokens[p].value === "*";
  }
  function isEOF(p = pos) {
    return tokens[p] instanceof EOFToken;
  }
  function isClauseCombinator(p = pos) {
    return tokens[p] instanceof DelimToken && [">", "+", "~"].includes(tokens[p].value);
  }
  function isSelectorClauseEnd(p = pos) {
    return isComma(p) || isCloseParen(p) || isEOF(p) || isClauseCombinator(p) || tokens[p] instanceof WhitespaceToken;
  }
  function consumeFunctionArguments() {
    const result2 = [consumeArgument()];
    while (true) {
      skipWhitespace();
      if (!isComma())
        break;
      pos++;
      result2.push(consumeArgument());
    }
    return result2;
  }
  function consumeArgument() {
    skipWhitespace();
    if (isNumber())
      return tokens[pos++].value;
    if (isString())
      return tokens[pos++].value;
    return consumeComplexSelector();
  }
  function consumeComplexSelector() {
    const result2 = { simples: [] };
    skipWhitespace();
    if (isClauseCombinator()) {
      result2.simples.push({ selector: { functions: [{ name: "scope", args: [] }] }, combinator: "" });
    } else {
      result2.simples.push({ selector: consumeSimpleSelector(), combinator: "" });
    }
    while (true) {
      skipWhitespace();
      if (isClauseCombinator()) {
        result2.simples[result2.simples.length - 1].combinator = tokens[pos++].value;
        skipWhitespace();
      } else if (isSelectorClauseEnd()) {
        break;
      }
      result2.simples.push({ combinator: "", selector: consumeSimpleSelector() });
    }
    return result2;
  }
  function consumeSimpleSelector() {
    let rawCSSString = "";
    const functions = [];
    while (!isSelectorClauseEnd()) {
      if (isIdent() || isStar()) {
        rawCSSString += tokens[pos++].toSource();
      } else if (tokens[pos] instanceof HashToken) {
        rawCSSString += tokens[pos++].toSource();
      } else if (tokens[pos] instanceof DelimToken && tokens[pos].value === ".") {
        pos++;
        if (isIdent())
          rawCSSString += "." + tokens[pos++].toSource();
        else
          throw unexpected();
      } else if (tokens[pos] instanceof ColonToken) {
        pos++;
        if (isIdent()) {
          if (!customNames.has(tokens[pos].value.toLowerCase())) {
            rawCSSString += ":" + tokens[pos++].toSource();
          } else {
            const name = tokens[pos++].value.toLowerCase();
            functions.push({ name, args: [] });
            names.add(name);
          }
        } else if (isFunction()) {
          const name = tokens[pos++].value.toLowerCase();
          if (!customNames.has(name)) {
            rawCSSString += `:${name}(${consumeBuiltinFunctionArguments()})`;
          } else {
            functions.push({ name, args: consumeFunctionArguments() });
            names.add(name);
          }
          skipWhitespace();
          if (!isCloseParen())
            throw unexpected();
          pos++;
        } else {
          throw unexpected();
        }
      } else if (tokens[pos] instanceof OpenSquareToken) {
        rawCSSString += "[";
        pos++;
        while (!(tokens[pos] instanceof CloseSquareToken) && !isEOF())
          rawCSSString += tokens[pos++].toSource();
        if (!(tokens[pos] instanceof CloseSquareToken))
          throw unexpected();
        rawCSSString += "]";
        pos++;
      } else {
        throw unexpected();
      }
    }
    if (!rawCSSString && !functions.length)
      throw unexpected();
    return { css: rawCSSString || void 0, functions };
  }
  function consumeBuiltinFunctionArguments() {
    let s = "";
    let balance = 1;
    while (!isEOF()) {
      if (isOpenParen() || isFunction())
        balance++;
      if (isCloseParen())
        balance--;
      if (!balance)
        break;
      s += tokens[pos++].toSource();
    }
    return s;
  }
  const result = consumeFunctionArguments();
  if (!isEOF())
    throw unexpected();
  if (result.some((arg) => typeof arg !== "object" || !("simples" in arg)))
    throw new InvalidSelectorError(`Error while parsing css selector "${selector}". Did you mean to CSS.escape it?`);
  return { selector: result, names: Array.from(names) };
}

// packages/isomorphic/selectorParser.ts
var kNestedSelectorNames = /* @__PURE__ */ new Set(["internal:has", "internal:has-not", "internal:and", "internal:or", "internal:chain", "left-of", "right-of", "above", "below", "near"]);
var kNestedSelectorNamesWithDistance = /* @__PURE__ */ new Set(["left-of", "right-of", "above", "below", "near"]);
var customCSSNames = /* @__PURE__ */ new Set(["not", "is", "where", "has", "scope", "light", "visible", "text", "text-matches", "text-is", "has-text", "above", "below", "right-of", "left-of", "near", "nth-match"]);
function parseSelector(selector) {
  var _a;
  const parsedStrings = parseSelectorString(selector);
  const parts = [];
  for (const part of parsedStrings.parts) {
    if (part.name === "css" || part.name === "css:light") {
      if (part.name === "css:light")
        part.body = ":light(" + part.body + ")";
      const parsedCSS = parseCSS(part.body, customCSSNames);
      parts.push({
        name: "css",
        body: parsedCSS.selector,
        source: part.body
      });
      continue;
    }
    if (kNestedSelectorNames.has(part.name)) {
      let innerSelector;
      let distance;
      try {
        const unescaped = JSON.parse("[" + part.body + "]");
        if (!Array.isArray(unescaped) || unescaped.length < 1 || unescaped.length > 2 || typeof unescaped[0] !== "string")
          throw new InvalidSelectorError(`Malformed selector: ${part.name}=` + part.body);
        innerSelector = unescaped[0];
        if (unescaped.length === 2) {
          if (typeof unescaped[1] !== "number" || !kNestedSelectorNamesWithDistance.has(part.name))
            throw new InvalidSelectorError(`Malformed selector: ${part.name}=` + part.body);
          distance = unescaped[1];
        }
      } catch (e) {
        throw new InvalidSelectorError(`Malformed selector: ${part.name}=` + part.body);
      }
      const nested = { name: part.name, source: part.body, body: { parsed: parseSelector(innerSelector), distance } };
      const lastFrame = [...nested.body.parsed.parts].reverse().find((part2) => part2.name === "internal:control" && part2.body === "enter-frame");
      const lastFrameIndex = lastFrame ? nested.body.parsed.parts.indexOf(lastFrame) : -1;
      const outerParts = ((_a = parts[0]) == null ? void 0 : _a.name) === "internal:control" && parts[0].body === "any-frame" ? parts.slice(1) : parts;
      if (lastFrameIndex !== -1 && selectorPartsEqual(nested.body.parsed.parts.slice(0, lastFrameIndex + 1), outerParts.slice(0, lastFrameIndex + 1))) {
        nested.body.parsed.parts.splice(0, lastFrameIndex + 1);
        if (nested.body.parsed.capture !== void 0) {
          if (nested.body.parsed.capture <= lastFrameIndex)
            throw new InvalidSelectorError(`Can not capture the selector before diving into the frame. Only use * after the last frame has been selected`);
          nested.body.parsed.capture -= lastFrameIndex + 1;
        }
      }
      parts.push(nested);
      continue;
    }
    parts.push({ ...part, source: part.body });
  }
  if (kNestedSelectorNames.has(parts[0].name))
    throw new InvalidSelectorError(`"${parts[0].name}" selector cannot be first`);
  return {
    capture: parsedStrings.capture,
    parts
  };
}
function selectorPartsEqual(list1, list2) {
  return stringifySelector({ parts: list1 }) === stringifySelector({ parts: list2 });
}
function stringifySelector(selector, forceEngineName) {
  if (typeof selector === "string")
    return selector;
  return selector.parts.map((p, i) => {
    let includeEngine = true;
    if (!forceEngineName && i !== selector.capture) {
      if (p.name === "css")
        includeEngine = false;
      else if (p.name === "xpath" && (p.source.startsWith("//") || p.source.startsWith("..")))
        includeEngine = false;
    }
    const prefix = includeEngine ? p.name + "=" : "";
    return `${i === selector.capture ? "*" : ""}${prefix}${p.source}`;
  }).join(" >> ");
}
function visitAllSelectorParts(selector, visitor) {
  const visit3 = (selector2, nested) => {
    for (const part of selector2.parts) {
      visitor(part, nested);
      if (kNestedSelectorNames.has(part.name))
        visit3(part.body.parsed, true);
    }
  };
  visit3(selector, false);
}
function parseSelectorString(selector) {
  let index = 0;
  let quote;
  let start = 0;
  const result = { parts: [] };
  const append = () => {
    const part = selector.substring(start, index).trim();
    const eqIndex = part.indexOf("=");
    let name;
    let body;
    if (eqIndex !== -1 && part.substring(0, eqIndex).trim().match(/^[a-zA-Z_0-9-+:*]+$/)) {
      name = part.substring(0, eqIndex).trim();
      body = part.substring(eqIndex + 1);
    } else if (part.length > 1 && part[0] === '"' && part[part.length - 1] === '"') {
      name = "text";
      body = part;
    } else if (part.length > 1 && part[0] === "'" && part[part.length - 1] === "'") {
      name = "text";
      body = part;
    } else if (/^\(*\/\//.test(part) || part.startsWith("..")) {
      name = "xpath";
      body = part;
    } else {
      name = "css";
      body = part;
    }
    let capture = false;
    if (name[0] === "*") {
      capture = true;
      name = name.substring(1);
    }
    result.parts.push({ name, body });
    if (capture) {
      if (result.capture !== void 0)
        throw new InvalidSelectorError(`Only one of the selectors can capture using * modifier`);
      result.capture = result.parts.length - 1;
    }
  };
  if (!selector.includes(">>")) {
    index = selector.length;
    append();
    return result;
  }
  const shouldIgnoreTextSelectorQuote = () => {
    const prefix = selector.substring(start, index);
    const match = prefix.match(/^\s*text\s*=(.*)$/);
    return !!match && !!match[1];
  };
  while (index < selector.length) {
    const c = selector[index];
    if (c === "\\" && index + 1 < selector.length) {
      index += 2;
    } else if (c === quote) {
      quote = void 0;
      index++;
    } else if (!quote && (c === '"' || c === "'" || c === "`") && !shouldIgnoreTextSelectorQuote()) {
      quote = c;
      index++;
    } else if (!quote && c === ">" && selector[index + 1] === ">") {
      append();
      index += 2;
      start = index;
    } else {
      index++;
    }
  }
  append();
  return result;
}
function parseAttributeSelector(selector, allowUnquotedStrings) {
  let wp = 0;
  let EOL = selector.length === 0;
  const next = () => selector[wp] || "";
  const eat1 = () => {
    const result2 = next();
    ++wp;
    EOL = wp >= selector.length;
    return result2;
  };
  const syntaxError = (stage) => {
    if (EOL)
      throw new InvalidSelectorError(`Unexpected end of selector while parsing selector \`${selector}\``);
    throw new InvalidSelectorError(`Error while parsing selector \`${selector}\` - unexpected symbol "${next()}" at position ${wp}` + (stage ? " during " + stage : ""));
  };
  function skipSpaces() {
    while (!EOL && /\s/.test(next()))
      eat1();
  }
  function isCSSNameChar(char) {
    return char >= "\x80" || char >= "0" && char <= "9" || char >= "A" && char <= "Z" || char >= "a" && char <= "z" || char >= "0" && char <= "9" || char === "_" || char === "-";
  }
  function readIdentifier() {
    let result2 = "";
    skipSpaces();
    while (!EOL && isCSSNameChar(next()))
      result2 += eat1();
    return result2;
  }
  function readQuotedString(quote) {
    let result2 = eat1();
    if (result2 !== quote)
      syntaxError("parsing quoted string");
    while (!EOL && next() !== quote) {
      if (next() === "\\")
        eat1();
      result2 += eat1();
    }
    if (next() !== quote)
      syntaxError("parsing quoted string");
    result2 += eat1();
    return result2;
  }
  function readRegularExpression() {
    if (eat1() !== "/")
      syntaxError("parsing regular expression");
    let source = "";
    let inClass = false;
    while (!EOL) {
      if (next() === "\\") {
        source += eat1();
        if (EOL)
          syntaxError("parsing regular expression");
      } else if (inClass && next() === "]") {
        inClass = false;
      } else if (!inClass && next() === "[") {
        inClass = true;
      } else if (!inClass && next() === "/") {
        break;
      }
      source += eat1();
    }
    if (eat1() !== "/")
      syntaxError("parsing regular expression");
    let flags = "";
    while (!EOL && next().match(/[dgimsuvy]/))
      flags += eat1();
    try {
      return new RegExp(source, flags);
    } catch (e) {
      throw new InvalidSelectorError(`Error while parsing selector \`${selector}\`: ${e.message}`);
    }
  }
  function readAttributeToken() {
    let token = "";
    skipSpaces();
    if (next() === `'` || next() === `"`)
      token = readQuotedString(next()).slice(1, -1);
    else
      token = readIdentifier();
    if (!token)
      syntaxError("parsing property path");
    return token;
  }
  function readOperator() {
    skipSpaces();
    let op = "";
    if (!EOL)
      op += eat1();
    if (!EOL && op !== "=")
      op += eat1();
    if (!["=", "*=", "^=", "$=", "|=", "~="].includes(op))
      syntaxError("parsing operator");
    return op;
  }
  function readAttribute() {
    eat1();
    const jsonPath = [];
    jsonPath.push(readAttributeToken());
    skipSpaces();
    while (next() === ".") {
      eat1();
      jsonPath.push(readAttributeToken());
      skipSpaces();
    }
    if (next() === "]") {
      eat1();
      return { name: jsonPath.join("."), jsonPath, op: "<truthy>", value: null, caseSensitive: false };
    }
    const operator = readOperator();
    let value = void 0;
    let caseSensitive = true;
    skipSpaces();
    if (next() === "/") {
      if (operator !== "=")
        throw new InvalidSelectorError(`Error while parsing selector \`${selector}\` - cannot use ${operator} in attribute with regular expression`);
      value = readRegularExpression();
    } else if (next() === `'` || next() === `"`) {
      value = readQuotedString(next()).slice(1, -1);
      skipSpaces();
      if (next() === "i" || next() === "I") {
        caseSensitive = false;
        eat1();
      } else if (next() === "s" || next() === "S") {
        caseSensitive = true;
        eat1();
      }
    } else {
      value = "";
      while (!EOL && (isCSSNameChar(next()) || next() === "+" || next() === "."))
        value += eat1();
      if (value === "true") {
        value = true;
      } else if (value === "false") {
        value = false;
      } else {
        if (!allowUnquotedStrings) {
          value = +value;
          if (Number.isNaN(value))
            syntaxError("parsing attribute value");
        }
      }
    }
    skipSpaces();
    if (next() !== "]")
      syntaxError("parsing attribute value");
    eat1();
    if (operator !== "=" && typeof value !== "string")
      throw new InvalidSelectorError(`Error while parsing selector \`${selector}\` - cannot use ${operator} in attribute with non-string matching value - ${value}`);
    return { name: jsonPath.join("."), jsonPath, op: operator, value, caseSensitive };
  }
  const result = {
    name: "",
    attributes: []
  };
  result.name = readIdentifier();
  skipSpaces();
  while (next() === "[") {
    result.attributes.push(readAttribute());
    skipSpaces();
  }
  if (!EOL)
    syntaxError(void 0);
  if (!result.name && !result.attributes.length)
    throw new InvalidSelectorError(`Error while parsing selector \`${selector}\` - selector cannot be empty`);
  return result;
}

// packages/isomorphic/locatorGenerators.ts
function asLocator(lang, selector, isFrameLocator = false) {
  return asLocators(lang, selector, isFrameLocator, 1)[0];
}
function asLocators(lang, selector, isFrameLocator = false, maxOutputSize = 20, preferredQuote) {
  try {
    return innerAsLocators(new generators[lang](preferredQuote), parseSelector(selector), isFrameLocator, maxOutputSize);
  } catch (e) {
    return [selector];
  }
}
function innerAsLocators(factory, parsed, isFrameLocator = false, maxOutputSize = 20) {
  const parts = [...parsed.parts];
  const tokens = [];
  let nextBase = isFrameLocator ? "frame-locator" : "page";
  for (let index = 0; index < parts.length; index++) {
    const part = parts[index];
    const base = nextBase;
    nextBase = "locator";
    if (part.name === "internal:describe")
      continue;
    if (part.name === "nth") {
      if (part.body === "0")
        tokens.push([factory.generateLocator(base, "first", ""), factory.generateLocator(base, "nth", "0")]);
      else if (part.body === "-1")
        tokens.push([factory.generateLocator(base, "last", ""), factory.generateLocator(base, "nth", "-1")]);
      else
        tokens.push([factory.generateLocator(base, "nth", part.body)]);
      continue;
    }
    if (part.name === "visible") {
      const tokenList = [];
      if (part.body === "true")
        tokenList.push(factory.generateLocator(base, "visible", ""));
      tokenList.push(factory.generateLocator(base, "filter-visible", part.body), factory.generateLocator(base, "default", `visible=${part.body}`));
      tokens.push(tokenList);
      continue;
    }
    if (part.name === "internal:text") {
      const { exact, text } = detectExact(part.body);
      tokens.push([factory.generateLocator(base, "text", text, { exact })]);
      continue;
    }
    if (part.name === "internal:has-text") {
      const { exact, text } = detectExact(part.body);
      if (!exact) {
        tokens.push([factory.generateLocator(base, "has-text", text, { exact })]);
        continue;
      }
    }
    if (part.name === "internal:has-not-text") {
      const { exact, text } = detectExact(part.body);
      if (!exact) {
        tokens.push([factory.generateLocator(base, "has-not-text", text, { exact })]);
        continue;
      }
    }
    if (part.name === "internal:has") {
      const inners = innerAsLocators(factory, part.body.parsed, false, maxOutputSize);
      tokens.push(inners.map((inner) => factory.generateLocator(base, "has", inner)));
      continue;
    }
    if (part.name === "internal:has-not") {
      const inners = innerAsLocators(factory, part.body.parsed, false, maxOutputSize);
      tokens.push(inners.map((inner) => factory.generateLocator(base, "hasNot", inner)));
      continue;
    }
    if (part.name === "internal:and") {
      const inners = innerAsLocators(factory, part.body.parsed, false, maxOutputSize);
      tokens.push(inners.map((inner) => factory.generateLocator(base, "and", inner)));
      continue;
    }
    if (part.name === "internal:or") {
      const inners = innerAsLocators(factory, part.body.parsed, false, maxOutputSize);
      tokens.push(inners.map((inner) => factory.generateLocator(base, "or", inner)));
      continue;
    }
    if (part.name === "internal:chain") {
      const inners = innerAsLocators(factory, part.body.parsed, false, maxOutputSize);
      tokens.push(inners.map((inner) => factory.generateLocator(base, "chain", inner)));
      continue;
    }
    if (part.name === "internal:label") {
      const { exact, text } = detectExact(part.body);
      tokens.push([factory.generateLocator(base, "label", text, { exact })]);
      continue;
    }
    if (part.name === "internal:role") {
      const attrSelector = parseAttributeSelector(part.body, true);
      const options = { attrs: [] };
      for (const attr of attrSelector.attributes) {
        if (attr.name === "name" || attr.name === "description") {
          if (typeof attr.value === "string") {
            if (options.exact !== void 0 && options.exact !== attr.caseSensitive)
              throw new Error(`Conflicting exactness in internal:role selector: ${stringifySelector({ parts: [part] })}`);
            options.exact = attr.caseSensitive;
          }
          options[attr.name] = attr.value;
        } else {
          if (attr.name === "level" && typeof attr.value === "string")
            attr.value = +attr.value;
          options.attrs.push({ name: attr.name === "include-hidden" ? "includeHidden" : attr.name, value: attr.value });
        }
      }
      tokens.push([factory.generateLocator(base, "role", attrSelector.name, options)]);
      continue;
    }
    if (part.name === "internal:testid") {
      const attrSelector = parseAttributeSelector(part.body, true);
      const { value } = attrSelector.attributes[0];
      tokens.push([factory.generateLocator(base, "test-id", value)]);
      continue;
    }
    if (part.name === "internal:attr") {
      const attrSelector = parseAttributeSelector(part.body, true);
      const { name, value, caseSensitive } = attrSelector.attributes[0];
      const text = value;
      const exact = !!caseSensitive;
      if (name === "placeholder") {
        tokens.push([factory.generateLocator(base, "placeholder", text, { exact })]);
        continue;
      }
      if (name === "alt") {
        tokens.push([factory.generateLocator(base, "alt", text, { exact })]);
        continue;
      }
      if (name === "title") {
        tokens.push([factory.generateLocator(base, "title", text, { exact })]);
        continue;
      }
    }
    if (part.name === "internal:control" && part.body === "any-frame") {
      tokens.push([factory.generateLocator(base, "any-frame", "")]);
      nextBase = "frame-locator";
      continue;
    }
    if (part.name === "internal:control" && part.body === "enter-frame") {
      const lastTokens = tokens[tokens.length - 1];
      const lastPart = parts[index - 1];
      const transformed = lastTokens.map((token) => factory.chainLocators([token, factory.generateLocator(base, "frame", "")]));
      if (["xpath", "css"].includes(lastPart.name)) {
        transformed.push(
          factory.generateLocator(base, "frame-locator", stringifySelector({ parts: [lastPart] })),
          factory.generateLocator(base, "frame-locator", stringifySelector({ parts: [lastPart] }, true))
        );
      }
      lastTokens.splice(0, lastTokens.length, ...transformed);
      nextBase = "frame-locator";
      continue;
    }
    const nextPart = parts[index + 1];
    const selectorPart = stringifySelector({ parts: [part] });
    const locatorPart = factory.generateLocator(base, "default", selectorPart);
    if (nextPart && ["internal:has-text", "internal:has-not-text"].includes(nextPart.name)) {
      const { exact, text } = detectExact(nextPart.body);
      if (!exact) {
        const nextLocatorPart = factory.generateLocator("locator", nextPart.name === "internal:has-text" ? "has-text" : "has-not-text", text, { exact });
        const options = {};
        if (nextPart.name === "internal:has-text")
          options.hasText = text;
        else
          options.hasNotText = text;
        const combinedPart = factory.generateLocator(base, "default", selectorPart, options);
        tokens.push([factory.chainLocators([locatorPart, nextLocatorPart]), combinedPart]);
        index++;
        continue;
      }
    }
    let locatorPartWithEngine;
    if (["xpath", "css"].includes(part.name)) {
      const selectorPart2 = stringifySelector(
        { parts: [part] },
        /* forceEngineName */
        true
      );
      locatorPartWithEngine = factory.generateLocator(base, "default", selectorPart2);
    }
    tokens.push([locatorPart, locatorPartWithEngine].filter(Boolean));
  }
  return combineTokens(factory, tokens, maxOutputSize);
}
function combineTokens(factory, tokens, maxOutputSize) {
  const currentTokens = tokens.map(() => "");
  const result = [];
  const visit3 = (index) => {
    if (index === tokens.length) {
      result.push(factory.chainLocators(currentTokens));
      return result.length < maxOutputSize;
    }
    for (const taken of tokens[index]) {
      currentTokens[index] = taken;
      if (!visit3(index + 1))
        return false;
    }
    return true;
  };
  visit3(0);
  return result;
}
function detectExact(text) {
  let exact = false;
  const match = text.match(/^\/(.*)\/([dgimsuvy]*)$/);
  if (match)
    return { text: new RegExp(match[1], match[2]) };
  if (text.endsWith('"')) {
    text = JSON.parse(text);
    exact = true;
  } else if (text.endsWith('"s')) {
    text = JSON.parse(text.substring(0, text.length - 1));
    exact = true;
  } else if (text.endsWith('"i')) {
    text = JSON.parse(text.substring(0, text.length - 1));
    exact = false;
  }
  return { exact, text };
}
var JavaScriptLocatorFactory = class {
  constructor(preferredQuote) {
    this.preferredQuote = preferredQuote;
  }
  generateLocator(base, kind, body, options = {}) {
    switch (kind) {
      case "default":
        if (options.hasText !== void 0)
          return `locator(${this.quote(body)}, { hasText: ${this.toHasText(options.hasText)} })`;
        if (options.hasNotText !== void 0)
          return `locator(${this.quote(body)}, { hasNotText: ${this.toHasText(options.hasNotText)} })`;
        return `locator(${this.quote(body)})`;
      case "frame-locator":
        return `frameLocator(${this.quote(body)})`;
      case "frame":
        return `contentFrame()`;
      case "any-frame":
        return `frameLocator()`;
      case "nth":
        return `nth(${body})`;
      case "first":
        return `first()`;
      case "last":
        return `last()`;
      case "visible":
        return `visible()`;
      case "filter-visible":
        return `filter({ visible: ${body === "true" ? "true" : "false"} })`;
      case "role":
        const attrs = [];
        if (isRegExp(options.name))
          attrs.push(`name: ${this.regexToSourceString(options.name)}`);
        else if (typeof options.name === "string")
          attrs.push(`name: ${this.quote(options.name)}`);
        if (isRegExp(options.description))
          attrs.push(`description: ${this.regexToSourceString(options.description)}`);
        else if (typeof options.description === "string")
          attrs.push(`description: ${this.quote(options.description)}`);
        if (options.exact && (typeof options.name === "string" || typeof options.description === "string"))
          attrs.push(`exact: true`);
        for (const { name, value } of options.attrs)
          attrs.push(`${name}: ${typeof value === "string" ? this.quote(value) : value}`);
        const attrString = attrs.length ? `, { ${attrs.join(", ")} }` : "";
        return `getByRole(${this.quote(body)}${attrString})`;
      case "has-text":
        return `filter({ hasText: ${this.toHasText(body)} })`;
      case "has-not-text":
        return `filter({ hasNotText: ${this.toHasText(body)} })`;
      case "has":
        return `filter({ has: ${body} })`;
      case "hasNot":
        return `filter({ hasNot: ${body} })`;
      case "and":
        return `and(${body})`;
      case "or":
        return `or(${body})`;
      case "chain":
        return `locator(${body})`;
      case "test-id":
        return `getByTestId(${this.toTestIdValue(body)})`;
      case "text":
        return this.toCallWithExact("getByText", body, !!options.exact);
      case "alt":
        return this.toCallWithExact("getByAltText", body, !!options.exact);
      case "placeholder":
        return this.toCallWithExact("getByPlaceholder", body, !!options.exact);
      case "label":
        return this.toCallWithExact("getByLabel", body, !!options.exact);
      case "title":
        return this.toCallWithExact("getByTitle", body, !!options.exact);
      default:
        throw new Error("Unknown selector kind " + kind);
    }
  }
  chainLocators(locators) {
    return locators.join(".");
  }
  regexToSourceString(re) {
    return normalizeEscapedRegexQuotes(String(re));
  }
  toCallWithExact(method, body, exact) {
    if (isRegExp(body))
      return `${method}(${this.regexToSourceString(body)})`;
    return exact ? `${method}(${this.quote(body)}, { exact: true })` : `${method}(${this.quote(body)})`;
  }
  toHasText(body) {
    if (isRegExp(body))
      return this.regexToSourceString(body);
    return this.quote(body);
  }
  toTestIdValue(value) {
    if (isRegExp(value))
      return this.regexToSourceString(value);
    return this.quote(value);
  }
  quote(text) {
    var _a;
    return escapeWithQuotes(text, (_a = this.preferredQuote) != null ? _a : "'");
  }
};
var PythonLocatorFactory = class {
  generateLocator(base, kind, body, options = {}) {
    switch (kind) {
      case "default":
        if (options.hasText !== void 0)
          return `locator(${this.quote(body)}, has_text=${this.toHasText(options.hasText)})`;
        if (options.hasNotText !== void 0)
          return `locator(${this.quote(body)}, has_not_text=${this.toHasText(options.hasNotText)})`;
        return `locator(${this.quote(body)})`;
      case "frame-locator":
        return `frame_locator(${this.quote(body)})`;
      case "frame":
        return `content_frame`;
      case "any-frame":
        return `frame_locator()`;
      case "nth":
        return `nth(${body})`;
      case "first":
        return `first`;
      case "last":
        return `last`;
      case "visible":
        return `visible`;
      case "filter-visible":
        return `filter(visible=${body === "true" ? "True" : "False"})`;
      case "role":
        const attrs = [];
        if (isRegExp(options.name))
          attrs.push(`name=${this.regexToString(options.name)}`);
        else if (typeof options.name === "string")
          attrs.push(`name=${this.quote(options.name)}`);
        if (isRegExp(options.description))
          attrs.push(`description=${this.regexToString(options.description)}`);
        else if (typeof options.description === "string")
          attrs.push(`description=${this.quote(options.description)}`);
        if (options.exact && (typeof options.name === "string" || typeof options.description === "string"))
          attrs.push(`exact=True`);
        for (const { name, value } of options.attrs) {
          let valueString = typeof value === "string" ? this.quote(value) : value;
          if (typeof value === "boolean")
            valueString = value ? "True" : "False";
          attrs.push(`${toSnakeCase(name)}=${valueString}`);
        }
        const attrString = attrs.length ? `, ${attrs.join(", ")}` : "";
        return `get_by_role(${this.quote(body)}${attrString})`;
      case "has-text":
        return `filter(has_text=${this.toHasText(body)})`;
      case "has-not-text":
        return `filter(has_not_text=${this.toHasText(body)})`;
      case "has":
        return `filter(has=${body})`;
      case "hasNot":
        return `filter(has_not=${body})`;
      case "and":
        return `and_(${body})`;
      case "or":
        return `or_(${body})`;
      case "chain":
        return `locator(${body})`;
      case "test-id":
        return `get_by_test_id(${this.toTestIdValue(body)})`;
      case "text":
        return this.toCallWithExact("get_by_text", body, !!options.exact);
      case "alt":
        return this.toCallWithExact("get_by_alt_text", body, !!options.exact);
      case "placeholder":
        return this.toCallWithExact("get_by_placeholder", body, !!options.exact);
      case "label":
        return this.toCallWithExact("get_by_label", body, !!options.exact);
      case "title":
        return this.toCallWithExact("get_by_title", body, !!options.exact);
      default:
        throw new Error("Unknown selector kind " + kind);
    }
  }
  chainLocators(locators) {
    return locators.join(".");
  }
  regexToString(body) {
    const suffix = regexFlagsSuffix(body, "re", regexFlagNames.python);
    return `re.compile(r"${normalizeEscapedRegexQuotes(body.source).replace(/\\\//, "/").replace(/"/g, '\\"')}"${suffix})`;
  }
  toCallWithExact(method, body, exact) {
    if (isRegExp(body))
      return `${method}(${this.regexToString(body)})`;
    if (exact)
      return `${method}(${this.quote(body)}, exact=True)`;
    return `${method}(${this.quote(body)})`;
  }
  toHasText(body) {
    if (isRegExp(body))
      return this.regexToString(body);
    return `${this.quote(body)}`;
  }
  toTestIdValue(value) {
    if (isRegExp(value))
      return this.regexToString(value);
    return this.quote(value);
  }
  quote(text) {
    return escapeWithQuotes(text, '"');
  }
};
var JavaLocatorFactory = class {
  generateLocator(base, kind, body, options = {}) {
    let clazz;
    switch (base) {
      case "page":
        clazz = "Page";
        break;
      case "frame-locator":
        clazz = "FrameLocator";
        break;
      case "locator":
        clazz = "Locator";
        break;
    }
    switch (kind) {
      case "default":
        if (options.hasText !== void 0)
          return `locator(${this.quote(body)}, new ${clazz}.LocatorOptions().setHasText(${this.toHasText(options.hasText)}))`;
        if (options.hasNotText !== void 0)
          return `locator(${this.quote(body)}, new ${clazz}.LocatorOptions().setHasNotText(${this.toHasText(options.hasNotText)}))`;
        return `locator(${this.quote(body)})`;
      case "frame-locator":
        return `frameLocator(${this.quote(body)})`;
      case "frame":
        return `contentFrame()`;
      case "any-frame":
        return `frameLocator()`;
      case "nth":
        return `nth(${body})`;
      case "first":
        return `first()`;
      case "last":
        return `last()`;
      case "visible":
        return `visible()`;
      case "filter-visible":
        return `filter(new ${clazz}.FilterOptions().setVisible(${body === "true" ? "true" : "false"}))`;
      case "role":
        const attrs = [];
        if (isRegExp(options.name))
          attrs.push(`.setName(${this.regexToString(options.name)})`);
        else if (typeof options.name === "string")
          attrs.push(`.setName(${this.quote(options.name)})`);
        if (isRegExp(options.description))
          attrs.push(`.setDescription(${this.regexToString(options.description)})`);
        else if (typeof options.description === "string")
          attrs.push(`.setDescription(${this.quote(options.description)})`);
        if (options.exact && (typeof options.name === "string" || typeof options.description === "string"))
          attrs.push(`.setExact(true)`);
        for (const { name, value } of options.attrs)
          attrs.push(`.set${toTitleCase(name)}(${typeof value === "string" ? this.quote(value) : value})`);
        const attrString = attrs.length ? `, new ${clazz}.GetByRoleOptions()${attrs.join("")}` : "";
        return `getByRole(AriaRole.${toSnakeCase(body).toUpperCase()}${attrString})`;
      case "has-text":
        return `filter(new ${clazz}.FilterOptions().setHasText(${this.toHasText(body)}))`;
      case "has-not-text":
        return `filter(new ${clazz}.FilterOptions().setHasNotText(${this.toHasText(body)}))`;
      case "has":
        return `filter(new ${clazz}.FilterOptions().setHas(${body}))`;
      case "hasNot":
        return `filter(new ${clazz}.FilterOptions().setHasNot(${body}))`;
      case "and":
        return `and(${body})`;
      case "or":
        return `or(${body})`;
      case "chain":
        return `locator(${body})`;
      case "test-id":
        return `getByTestId(${this.toTestIdValue(body)})`;
      case "text":
        return this.toCallWithExact(clazz, "getByText", body, !!options.exact);
      case "alt":
        return this.toCallWithExact(clazz, "getByAltText", body, !!options.exact);
      case "placeholder":
        return this.toCallWithExact(clazz, "getByPlaceholder", body, !!options.exact);
      case "label":
        return this.toCallWithExact(clazz, "getByLabel", body, !!options.exact);
      case "title":
        return this.toCallWithExact(clazz, "getByTitle", body, !!options.exact);
      default:
        throw new Error("Unknown selector kind " + kind);
    }
  }
  chainLocators(locators) {
    return locators.join(".");
  }
  regexToString(body) {
    const suffix = regexFlagsSuffix(body, "Pattern", regexFlagNames.java);
    return `Pattern.compile(${this.quote(normalizeEscapedRegexQuotes(body.source))}${suffix})`;
  }
  toCallWithExact(clazz, method, body, exact) {
    if (isRegExp(body))
      return `${method}(${this.regexToString(body)})`;
    if (exact)
      return `${method}(${this.quote(body)}, new ${clazz}.${toTitleCase(method)}Options().setExact(true))`;
    return `${method}(${this.quote(body)})`;
  }
  toHasText(body) {
    if (isRegExp(body))
      return this.regexToString(body);
    return this.quote(body);
  }
  toTestIdValue(value) {
    if (isRegExp(value))
      return this.regexToString(value);
    return this.quote(value);
  }
  quote(text) {
    return escapeWithQuotes(text, '"');
  }
};
var CSharpLocatorFactory = class {
  generateLocator(base, kind, body, options = {}) {
    switch (kind) {
      case "default":
        if (options.hasText !== void 0)
          return `Locator(${this.quote(body)}, new() { ${this.toHasText(options.hasText)} })`;
        if (options.hasNotText !== void 0)
          return `Locator(${this.quote(body)}, new() { ${this.toHasNotText(options.hasNotText)} })`;
        return `Locator(${this.quote(body)})`;
      case "frame-locator":
        return `FrameLocator(${this.quote(body)})`;
      case "frame":
        return `ContentFrame`;
      case "any-frame":
        return `FrameLocator()`;
      case "nth":
        return `Nth(${body})`;
      case "first":
        return `First`;
      case "last":
        return `Last`;
      case "visible":
        return `Visible`;
      case "filter-visible":
        return `Filter(new() { Visible = ${body === "true" ? "true" : "false"} })`;
      case "role":
        const attrs = [];
        if (isRegExp(options.name))
          attrs.push(`NameRegex = ${this.regexToString(options.name)}`);
        else if (typeof options.name === "string")
          attrs.push(`Name = ${this.quote(options.name)}`);
        if (isRegExp(options.description))
          attrs.push(`DescriptionRegex = ${this.regexToString(options.description)}`);
        else if (typeof options.description === "string")
          attrs.push(`Description = ${this.quote(options.description)}`);
        if (options.exact && (typeof options.name === "string" || typeof options.description === "string"))
          attrs.push(`Exact = true`);
        for (const { name, value } of options.attrs)
          attrs.push(`${toTitleCase(name)} = ${typeof value === "string" ? this.quote(value) : value}`);
        const attrString = attrs.length ? `, new() { ${attrs.join(", ")} }` : "";
        return `GetByRole(AriaRole.${toTitleCase(body)}${attrString})`;
      case "has-text":
        return `Filter(new() { ${this.toHasText(body)} })`;
      case "has-not-text":
        return `Filter(new() { ${this.toHasNotText(body)} })`;
      case "has":
        return `Filter(new() { Has = ${body} })`;
      case "hasNot":
        return `Filter(new() { HasNot = ${body} })`;
      case "and":
        return `And(${body})`;
      case "or":
        return `Or(${body})`;
      case "chain":
        return `Locator(${body})`;
      case "test-id":
        return `GetByTestId(${this.toTestIdValue(body)})`;
      case "text":
        return this.toCallWithExact("GetByText", body, !!options.exact);
      case "alt":
        return this.toCallWithExact("GetByAltText", body, !!options.exact);
      case "placeholder":
        return this.toCallWithExact("GetByPlaceholder", body, !!options.exact);
      case "label":
        return this.toCallWithExact("GetByLabel", body, !!options.exact);
      case "title":
        return this.toCallWithExact("GetByTitle", body, !!options.exact);
      default:
        throw new Error("Unknown selector kind " + kind);
    }
  }
  chainLocators(locators) {
    return locators.join(".");
  }
  regexToString(body) {
    const suffix = regexFlagsSuffix(body, "RegexOptions", regexFlagNames.csharp);
    return `new Regex(${this.quote(normalizeEscapedRegexQuotes(body.source))}${suffix})`;
  }
  toCallWithExact(method, body, exact) {
    if (isRegExp(body))
      return `${method}(${this.regexToString(body)})`;
    if (exact)
      return `${method}(${this.quote(body)}, new() { Exact = true })`;
    return `${method}(${this.quote(body)})`;
  }
  toHasText(body) {
    if (isRegExp(body))
      return `HasTextRegex = ${this.regexToString(body)}`;
    return `HasText = ${this.quote(body)}`;
  }
  toTestIdValue(value) {
    if (isRegExp(value))
      return this.regexToString(value);
    return this.quote(value);
  }
  toHasNotText(body) {
    if (isRegExp(body))
      return `HasNotTextRegex = ${this.regexToString(body)}`;
    return `HasNotText = ${this.quote(body)}`;
  }
  quote(text) {
    return escapeWithQuotes(text, '"');
  }
};
var JsonlLocatorFactory = class {
  generateLocator(base, kind, body, options = {}) {
    return JSON.stringify({
      kind,
      body,
      options
    });
  }
  chainLocators(locators) {
    const objects = locators.map((l) => JSON.parse(l));
    for (let i = 0; i < objects.length - 1; ++i) {
      let tail = objects[i];
      while (tail.next)
        tail = tail.next;
      tail.next = objects[i + 1];
    }
    return JSON.stringify(objects[0]);
  }
};
var generators = {
  javascript: JavaScriptLocatorFactory,
  python: PythonLocatorFactory,
  java: JavaLocatorFactory,
  csharp: CSharpLocatorFactory,
  jsonl: JsonlLocatorFactory
};
var regexFlagNames = {
  python: { i: "IGNORECASE", m: "MULTILINE", s: "DOTALL" },
  java: { i: "CASE_INSENSITIVE", m: "MULTILINE", s: "DOTALL" },
  csharp: { i: "IgnoreCase", m: "Multiline", s: "Singleline" }
};
function regexFlagsSuffix(body, flagsEnum, names) {
  const flags = [...body.flags].filter((flag) => flag in names).map((flag) => `${flagsEnum}.${names[flag]}`);
  return flags.length ? `, ${flags.join(" | ")}` : "";
}
function isRegExp(obj) {
  return obj instanceof RegExp;
}

// packages/isomorphic/locatorUtils.ts
function getByAttributeTextSelector(attrName, text, options) {
  return `internal:attr=[${attrName}=${escapeForAttributeSelector(text, (options == null ? void 0 : options.exact) || false)}]`;
}
function splitTestIdAttributeNames(testIdAttributeName) {
  return testIdAttributeName.split(",");
}
function encodeTestIdAttributeName(testIdAttributeName) {
  return testIdAttributeName.includes(",") ? JSON.stringify(testIdAttributeName) : testIdAttributeName;
}
function getByTestIdSelector(testIdAttributeName, testId) {
  return `internal:testid=[${encodeTestIdAttributeName(testIdAttributeName)}=${escapeForAttributeSelector(testId, true)}]`;
}
function getByLabelSelector(text, options) {
  return "internal:label=" + escapeForTextSelector(text, !!(options == null ? void 0 : options.exact));
}
function getByAltTextSelector(text, options) {
  return getByAttributeTextSelector("alt", text, options);
}
function getByTitleSelector(text, options) {
  return getByAttributeTextSelector("title", text, options);
}
function getByPlaceholderSelector(text, options) {
  return getByAttributeTextSelector("placeholder", text, options);
}
function getByTextSelector(text, options) {
  return "internal:text=" + escapeForTextSelector(text, !!(options == null ? void 0 : options.exact));
}
function getByRoleSelector(role, options = {}) {
  const props = [];
  if (options.checked !== void 0)
    props.push(["checked", String(options.checked)]);
  if (options.disabled !== void 0)
    props.push(["disabled", String(options.disabled)]);
  if (options.selected !== void 0)
    props.push(["selected", String(options.selected)]);
  if (options.expanded !== void 0)
    props.push(["expanded", String(options.expanded)]);
  if (options.includeHidden !== void 0)
    props.push(["include-hidden", String(options.includeHidden)]);
  if (options.level !== void 0)
    props.push(["level", String(options.level)]);
  if (options.name !== void 0)
    props.push(["name", escapeForAttributeSelector(options.name, !!options.exact)]);
  if (options.description !== void 0)
    props.push(["description", escapeForAttributeSelector(options.description, !!options.exact)]);
  if (options.pressed !== void 0)
    props.push(["pressed", String(options.pressed)]);
  return `internal:role=${role}${props.map(([n, v]) => `[${n}=${v}]`).join("")}`;
}

// packages/injected/src/ariaSnapshotDistiller.ts
function distillAriaSnapshot(snapshot, options) {
  runPlugins(snapshot, options.mode === "ai" ? aiPlugins : normalizePlugins, options);
}
function runPlugins(snapshot, plugins, options) {
  var _a, _b;
  const ctx = { snapshot, depth: -1, maxDepth: options.depth, ancestors: [], pendingContentRefs: /* @__PURE__ */ new Set() };
  const traverse = (node, depth) => {
    const children = [];
    const visitChild = (child) => {
      var _a2, _b2;
      if (typeof child === "string") {
        children.push(child);
        return;
      }
      ctx.depth = depth + 1;
      for (const plugin of plugins) {
        const result = (_a2 = plugin.enter) == null ? void 0 : _a2.call(plugin, child, ctx);
        if (result === "remove")
          return;
        if (result === "unwrap") {
          child.children.forEach(visitChild);
          return;
        }
      }
      traverse(child, depth + 1);
      ctx.depth = depth + 1;
      for (const plugin of plugins) {
        const result = (_b2 = plugin.exit) == null ? void 0 : _b2.call(plugin, child, ctx);
        if (result === "remove")
          return;
        if (result === "unwrap") {
          children.push(...child.children);
          return;
        }
      }
      children.push(child);
    };
    ctx.ancestors.push(node);
    node.children.forEach(visitChild);
    ctx.ancestors.pop();
    node.children = children;
  };
  for (const plugin of plugins)
    (_a = plugin.enter) == null ? void 0 : _a.call(plugin, snapshot.root, ctx);
  traverse(snapshot.root, -1);
  ctx.depth = -1;
  for (const plugin of plugins)
    (_b = plugin.exit) == null ? void 0 : _b.call(plugin, snapshot.root, ctx);
}
function isLeafGeneric(node) {
  return node.role === "generic" && node.children.every((child) => typeof child === "string");
}
function isClickTargetRoot(node, ctx) {
  return !!node.ref && hasPointerCursor(node) && !ctx.ancestors.some((ancestor) => !!ancestor.ref && hasPointerCursor(ancestor));
}
var mergeStringChildren = {
  name: "mergeStringChildren",
  exit(node) {
    const children = [];
    const buffer = [];
    const flush = () => {
      if (!buffer.length)
        return;
      const text = normalizeWhiteSpace(buffer.join(""));
      if (text)
        children.push(text);
      buffer.length = 0;
    };
    for (const child of node.children) {
      if (typeof child === "string") {
        buffer.push(child);
      } else {
        flush();
        children.push(child);
      }
    }
    flush();
    node.children = children;
    if (node.children.length === 1 && node.children[0] === node.name)
      node.children = [];
  }
};
var unwrapSingleChildGenerics = {
  name: "unwrapSingleChildGenerics",
  exit(node, ctx) {
    if (node.role !== "generic" || node.name || node.children.length > 1 || !node.children.every((child) => typeof child !== "string" && !!child.ref))
      return;
    if (!node.children.length && isClickTargetRoot(node, ctx))
      return;
    return "unwrap";
  }
};
var removeNamelessImages = {
  name: "removeNamelessImages",
  exit(node, ctx) {
    if (node.role === "img" && !node.name && !node.children.length && !isClickTargetRoot(node, ctx))
      return "remove";
  }
};
var removeRedundantNames = {
  name: "removeRedundantNames",
  enter(node, ctx) {
    var _a;
    if (!node.ref)
      return;
    for (const ref of ((_a = ctx.snapshot.info.get(node.ref)) == null ? void 0 : _a.nameFromContentRefs) || [])
      ctx.pendingContentRefs.add(ref);
    const beyondDepth = !!ctx.maxDepth && ctx.depth > ctx.maxDepth;
    if (!beyondDepth && !isLeafGeneric(node))
      ctx.pendingContentRefs.delete(node.ref);
  },
  exit(node, ctx) {
    var _a;
    if (!node.ref)
      return;
    const nameFromContentRefs = (_a = ctx.snapshot.info.get(node.ref)) == null ? void 0 : _a.nameFromContentRefs;
    if (!(nameFromContentRefs == null ? void 0 : nameFromContentRefs.length))
      return;
    if (nameFromContentRefs.every((ref) => !ctx.pendingContentRefs.has(ref))) {
      node.name = "";
    } else {
      for (const ref of nameFromContentRefs)
        ctx.pendingContentRefs.delete(ref);
    }
  }
};
var removeNameRepeatingChild = {
  name: "removeNameRepeatingChild",
  exit(node, ctx) {
    const parent = ctx.ancestors[ctx.ancestors.length - 1];
    if (!(parent == null ? void 0 : parent.name) || node.role !== "generic" || node.active || Object.keys(node.props).length)
      return;
    const singleTextChild = node.children.length === 1 && typeof node.children[0] === "string" ? node.children[0] : void 0;
    const text = node.name ? node.children.length ? void 0 : node.name : singleTextChild;
    if (text && text === parent.name) {
      if (node.ref)
        ctx.pendingContentRefs.add(node.ref);
      return "remove";
    }
  }
};
var inlineTextIntoGeneric = {
  name: "inlineTextIntoGeneric",
  exit(node) {
    if (node.role !== "generic" || Object.keys(node.props).length || node.children.length !== 1)
      return;
    const child = node.children[0];
    if (typeof child === "string")
      return;
    if (child.role !== "generic" || child.name || child.active || Object.keys(child.props).length)
      return;
    if (child.children.length === 1 && typeof child.children[0] === "string")
      node.children = [child.children[0]];
  }
};
var normalizePlugins = [
  mergeStringChildren,
  unwrapSingleChildGenerics
];
var aiPlugins = [
  mergeStringChildren,
  removeNamelessImages,
  removeRedundantNames,
  inlineTextIntoGeneric,
  removeNameRepeatingChild,
  unwrapSingleChildGenerics
];

// packages/injected/src/domUtils.ts
var globalOptions = {};
function setGlobalOptions(options) {
  globalOptions = options;
}
function isInsideScope(scope, element) {
  while (element) {
    if (scope.contains(element))
      return true;
    element = enclosingShadowHost(element);
  }
  return false;
}
function parentElementOrShadowHost(element) {
  if (element.parentElement)
    return element.parentElement;
  if (!element.parentNode)
    return;
  if (element.parentNode.nodeType === 11 && element.parentNode.host)
    return element.parentNode.host;
}
function enclosingShadowRootOrDocument(element) {
  let node = element;
  while (node.parentNode)
    node = node.parentNode;
  if (node.nodeType === 11 || node.nodeType === 9)
    return node;
}
function enclosingShadowHost(element) {
  while (element.parentElement)
    element = element.parentElement;
  return parentElementOrShadowHost(element);
}
function closestCrossShadow(element, css, scope) {
  while (element) {
    const closest = element.closest(css);
    if (scope && closest !== scope && (closest == null ? void 0 : closest.contains(scope)))
      return;
    if (closest)
      return closest;
    element = enclosingShadowHost(element);
  }
}
function getElementComputedStyle(element, pseudo) {
  const cache = pseudo === "::before" ? cacheStyleBefore : pseudo === "::after" ? cacheStyleAfter : cacheStyle;
  if (cache && cache.has(element))
    return cache.get(element);
  const style = element.ownerDocument && element.ownerDocument.defaultView ? element.ownerDocument.defaultView.getComputedStyle(element, pseudo) : void 0;
  cache == null ? void 0 : cache.set(element, style);
  return style;
}
function isElementStyleVisibilityVisible(element, style) {
  const cached = cacheStyleVisibility == null ? void 0 : cacheStyleVisibility.get(element);
  if (cached !== void 0)
    return cached;
  const result = computeElementStyleVisibilityVisible(element, style);
  cacheStyleVisibility == null ? void 0 : cacheStyleVisibility.set(element, result);
  return result;
}
function computeElementStyleVisibilityVisible(element, style) {
  style = style != null ? style : getElementComputedStyle(element);
  if (!style)
    return true;
  if (!element.checkVisibility() && !isWebKitListBoxOptionVisible(element, style))
    return false;
  if (style.visibility !== "visible")
    return false;
  return true;
}
function isListBoxSelect(select) {
  return select.multiple || select.size > 1;
}
function isWebKitListBoxOptionVisible(element, style) {
  var _a;
  if (globalOptions.browserNameForWorkarounds !== "webkit" || element.nodeName !== "OPTION" || style.display === "none")
    return false;
  const select = element.closest("select");
  if (!select || !isListBoxSelect(select))
    return false;
  for (let e = element.parentElement; e && e !== select; e = e.parentElement) {
    if (((_a = getElementComputedStyle(e)) == null ? void 0 : _a.display) === "none")
      return false;
  }
  return isElementStyleVisibilityVisible(select);
}
function computeBox(element) {
  const style = getElementComputedStyle(element);
  if (!style)
    return { visible: true, inline: false };
  const cursor = style.cursor;
  if (style.display === "contents") {
    for (let child = element.firstChild; child; child = child.nextSibling) {
      if (child.nodeType === 1 && isElementVisible(child))
        return { visible: true, inline: false, cursor };
      if (child.nodeType === 3 && isVisibleTextNode(child))
        return { visible: true, inline: true, cursor };
    }
    return { visible: false, inline: false, cursor };
  }
  if (!isElementStyleVisibilityVisible(element, style))
    return { cursor, visible: false, inline: false };
  const rect = element.getBoundingClientRect();
  return { cursor, visible: rect.width > 0 && rect.height > 0, inline: style.display === "inline" };
}
function isElementVisible(element) {
  return computeBox(element).visible;
}
var kTextNodeRange = /* @__PURE__ */ Symbol("playwrightTextNodeRange");
function isVisibleTextNode(node) {
  var _a;
  const document = node.ownerDocument;
  const range = (_a = document[kTextNodeRange]) != null ? _a : document[kTextNodeRange] = document.createRange();
  range.selectNode(node);
  const rect = range.getBoundingClientRect();
  const result = rect.width > 0 && rect.height > 0;
  range.setStart(document, 0);
  range.collapse(true);
  return result;
}
function elementSafeTagName(element) {
  const tagName = element.tagName;
  if (typeof tagName === "string") {
    const firstCharCode = tagName.charCodeAt(0);
    if (firstCharCode >= 97 && firstCharCode <= 122)
      return tagName.toUpperCase();
    return tagName;
  }
  if (element instanceof HTMLFormElement)
    return "FORM";
  return element.tagName.toUpperCase();
}
var cacheStyle;
var cacheStyleBefore;
var cacheStyleAfter;
var cacheStyleVisibility;
var cachesCounter = 0;
function beginDOMCaches() {
  ++cachesCounter;
  cacheStyle != null ? cacheStyle : cacheStyle = /* @__PURE__ */ new Map();
  cacheStyleBefore != null ? cacheStyleBefore : cacheStyleBefore = /* @__PURE__ */ new Map();
  cacheStyleAfter != null ? cacheStyleAfter : cacheStyleAfter = /* @__PURE__ */ new Map();
  cacheStyleVisibility != null ? cacheStyleVisibility : cacheStyleVisibility = /* @__PURE__ */ new Map();
}
function endDOMCaches() {
  if (!--cachesCounter) {
    cacheStyle = void 0;
    cacheStyleBefore = void 0;
    cacheStyleAfter = void 0;
    cacheStyleVisibility = void 0;
  }
}

// packages/injected/src/roleUtils.ts
function hasExplicitAccessibleName(e) {
  return e.hasAttribute("aria-label") || e.hasAttribute("aria-labelledby");
}
var kAncestorPreventingLandmark = "article:not([role]), aside:not([role]), main:not([role]), nav:not([role]), section:not([role]), [role=article], [role=complementary], [role=main], [role=navigation], [role=region]";
var kGlobalAriaAttributes = [
  ["aria-atomic", void 0],
  ["aria-busy", void 0],
  ["aria-controls", void 0],
  ["aria-current", void 0],
  ["aria-describedby", void 0],
  ["aria-details", void 0],
  // Global use deprecated in ARIA 1.2
  // ['aria-disabled', undefined],
  ["aria-dropeffect", void 0],
  // Global use deprecated in ARIA 1.2
  // ['aria-errormessage', undefined],
  ["aria-flowto", void 0],
  ["aria-grabbed", void 0],
  // Global use deprecated in ARIA 1.2
  // ['aria-haspopup', undefined],
  ["aria-hidden", void 0],
  // Global use deprecated in ARIA 1.2
  // ['aria-invalid', undefined],
  ["aria-keyshortcuts", void 0],
  ["aria-label", ["caption", "code", "deletion", "emphasis", "generic", "insertion", "paragraph", "presentation", "strong", "subscript", "superscript"]],
  ["aria-labelledby", ["caption", "code", "deletion", "emphasis", "generic", "insertion", "paragraph", "presentation", "strong", "subscript", "superscript"]],
  ["aria-live", void 0],
  ["aria-owns", void 0],
  ["aria-relevant", void 0],
  ["aria-roledescription", ["generic"]]
];
function hasGlobalAriaAttribute(element, forRole) {
  return kGlobalAriaAttributes.some(([attr, prohibited]) => {
    return !(prohibited == null ? void 0 : prohibited.includes(forRole || "")) && element.hasAttribute(attr);
  });
}
function hasTabIndex(element) {
  return !Number.isNaN(Number(String(element.getAttribute("tabindex"))));
}
function isFocusable(element) {
  return !isNativelyDisabled(element) && (isNativelyFocusable(element) || hasTabIndex(element));
}
function isNativelyFocusable(element) {
  const tagName = elementSafeTagName(element);
  if (["BUTTON", "DETAILS", "SELECT", "TEXTAREA"].includes(tagName))
    return true;
  if (tagName === "A" || tagName === "AREA")
    return element.hasAttribute("href");
  if (tagName === "INPUT")
    return !element.hidden;
  return false;
}
var kImplicitRoleByTagName = {
  "A": (e) => {
    return e.hasAttribute("href") ? "link" : null;
  },
  "AREA": (e) => {
    return e.hasAttribute("href") ? "link" : null;
  },
  "ARTICLE": () => "article",
  "ASIDE": () => "complementary",
  "BLOCKQUOTE": () => "blockquote",
  "BUTTON": () => "button",
  "CAPTION": () => "caption",
  "CODE": () => "code",
  "DATALIST": () => "listbox",
  "DD": () => "definition",
  "DEL": () => "deletion",
  "DETAILS": () => "group",
  "DFN": () => "term",
  "DIALOG": () => "dialog",
  "DT": () => "term",
  "EM": () => "emphasis",
  "FIELDSET": () => "group",
  "FIGURE": () => "figure",
  "FOOTER": (e) => closestCrossShadow(e, kAncestorPreventingLandmark) ? null : "contentinfo",
  "FORM": (e) => hasExplicitAccessibleName(e) ? "form" : null,
  "H1": () => "heading",
  "H2": () => "heading",
  "H3": () => "heading",
  "H4": () => "heading",
  "H5": () => "heading",
  "H6": () => "heading",
  "HEADER": (e) => closestCrossShadow(e, kAncestorPreventingLandmark) ? null : "banner",
  "HR": () => "separator",
  "HTML": () => "document",
  "IMG": (e) => e.getAttribute("alt") === "" && !e.getAttribute("title") && !hasGlobalAriaAttribute(e) && !hasTabIndex(e) ? "presentation" : "img",
  "INPUT": (e) => {
    const type = e.type.toLowerCase();
    if (["email", "search", "tel", "text", "url", ""].includes(type)) {
      const list = getIdRefs(e, e.getAttribute("list"))[0];
      if (list && elementSafeTagName(list) === "DATALIST")
        return "combobox";
      return type === "search" ? "searchbox" : "textbox";
    }
    if (type === "hidden")
      return null;
    if (type === "file")
      return "button";
    return inputTypeToRole[type] || "textbox";
  },
  "INS": () => "insertion",
  "LI": () => "listitem",
  "MAIN": () => "main",
  "MARK": () => "mark",
  "MATH": () => "math",
  "MENU": () => "list",
  "METER": () => "meter",
  "NAV": () => "navigation",
  "OL": () => "list",
  "OPTGROUP": () => "group",
  "OPTION": () => "option",
  "OUTPUT": () => "status",
  "P": () => "paragraph",
  "PROGRESS": () => "progressbar",
  "SEARCH": () => "search",
  "SECTION": (e) => hasExplicitAccessibleName(e) ? "region" : null,
  "SELECT": (e) => isListBoxSelect(e) ? "listbox" : "combobox",
  "STRONG": () => "strong",
  "SUB": () => "subscript",
  "SUP": () => "superscript",
  // For <svg> we default to Chrome behavior:
  // - Chrome reports 'img'.
  // - Firefox reports 'diagram' that is not in official ARIA spec yet.
  // - Safari reports 'no role', but still computes accessible name.
  "SVG": () => "img",
  "TABLE": () => "table",
  "TBODY": () => "rowgroup",
  "TD": (e) => {
    const table = closestCrossShadow(e, "table");
    const role = table ? getExplicitAriaRole(table) : "";
    return role === "grid" || role === "treegrid" ? "gridcell" : "cell";
  },
  "TEXTAREA": () => "textbox",
  "TFOOT": () => "rowgroup",
  "TH": (e) => {
    const scope = e.getAttribute("scope");
    if (scope === "col" || scope === "colgroup")
      return "columnheader";
    if (scope === "row" || scope === "rowgroup")
      return "rowheader";
    const nextSibling = e.nextElementSibling;
    const prevSibling = e.previousElementSibling;
    const row = !!e.parentElement && elementSafeTagName(e.parentElement) === "TR" ? e.parentElement : void 0;
    if (!nextSibling && !prevSibling) {
      if (row) {
        const table = closestCrossShadow(row, "table");
        if (table && table.rows.length <= 1)
          return null;
      }
      return "columnheader";
    }
    if (isHeaderCell(nextSibling) && isHeaderCell(prevSibling))
      return "columnheader";
    if (isNonEmptyDataCell(nextSibling) || isNonEmptyDataCell(prevSibling))
      return "rowheader";
    return "columnheader";
  },
  "THEAD": () => "rowgroup",
  "TIME": () => "time",
  "TR": () => "row",
  "UL": () => "list"
};
function isHeaderCell(element) {
  return !!element && elementSafeTagName(element) === "TH";
}
function isNonEmptyDataCell(element) {
  var _a;
  if (!element || elementSafeTagName(element) !== "TD")
    return false;
  return !!(((_a = element.textContent) == null ? void 0 : _a.trim()) || element.children.length > 0);
}
var kPresentationInheritanceParents = {
  "DD": ["DL", "DIV"],
  "DIV": ["DL"],
  "DT": ["DL", "DIV"],
  "LI": ["OL", "UL"],
  "TBODY": ["TABLE"],
  "TD": ["TR"],
  "TFOOT": ["TABLE"],
  "TH": ["TR"],
  "THEAD": ["TABLE"],
  "TR": ["THEAD", "TBODY", "TFOOT", "TABLE"]
};
function getImplicitAriaRole(element) {
  var _a;
  const implicitRole = ((_a = kImplicitRoleByTagName[elementSafeTagName(element)]) == null ? void 0 : _a.call(kImplicitRoleByTagName, element)) || "";
  if (!implicitRole)
    return null;
  let ancestor = element;
  while (ancestor) {
    const parent = parentElementOrShadowHost(ancestor);
    const parents = kPresentationInheritanceParents[elementSafeTagName(ancestor)];
    if (!parents || !parent || !parents.includes(elementSafeTagName(parent)))
      break;
    const parentExplicitRole = getExplicitAriaRole(parent);
    if ((parentExplicitRole === "none" || parentExplicitRole === "presentation") && !hasPresentationConflictResolution(parent, parentExplicitRole))
      return parentExplicitRole;
    ancestor = parent;
  }
  return implicitRole;
}
var validRoles = [
  "alert",
  "alertdialog",
  "application",
  "article",
  "banner",
  "blockquote",
  "button",
  "caption",
  "cell",
  "checkbox",
  "code",
  "columnheader",
  "combobox",
  "complementary",
  "contentinfo",
  "definition",
  "deletion",
  "dialog",
  "directory",
  "document",
  "emphasis",
  "feed",
  "figure",
  "form",
  "generic",
  "grid",
  "gridcell",
  "group",
  "heading",
  "img",
  "insertion",
  "link",
  "list",
  "listbox",
  "listitem",
  "log",
  "main",
  "mark",
  "marquee",
  "math",
  "meter",
  "menu",
  "menubar",
  "menuitem",
  "menuitemcheckbox",
  "menuitemradio",
  "navigation",
  "none",
  "note",
  "option",
  "paragraph",
  "presentation",
  "progressbar",
  "radio",
  "radiogroup",
  "region",
  "row",
  "rowgroup",
  "rowheader",
  "scrollbar",
  "search",
  "searchbox",
  "separator",
  "slider",
  "spinbutton",
  "status",
  "strong",
  "subscript",
  "superscript",
  "switch",
  "tab",
  "table",
  "tablist",
  "tabpanel",
  "term",
  "textbox",
  "time",
  "timer",
  "toolbar",
  "tooltip",
  "tree",
  "treegrid",
  "treeitem"
];
function getExplicitAriaRole(element) {
  const roles = (element.getAttribute("role") || "").split(" ").map((role) => role.trim());
  return roles.find((role) => validRoles.includes(role)) || null;
}
function hasPresentationConflictResolution(element, role) {
  return hasGlobalAriaAttribute(element, role) || isFocusable(element);
}
function getAriaRole(element) {
  const cached = cacheAriaRole == null ? void 0 : cacheAriaRole.get(element);
  if (cached !== void 0)
    return cached;
  const role = computeAriaRole(element);
  cacheAriaRole == null ? void 0 : cacheAriaRole.set(element, role);
  return role;
}
function computeAriaRole(element) {
  const explicitRole = getExplicitAriaRole(element);
  if (!explicitRole)
    return getImplicitAriaRole(element);
  if (explicitRole === "none" || explicitRole === "presentation") {
    const implicitRole = getImplicitAriaRole(element);
    if (hasPresentationConflictResolution(element, implicitRole))
      return implicitRole;
  }
  return explicitRole;
}
function getAriaBoolean(attr) {
  return attr === null ? void 0 : attr.toLowerCase() === "true";
}
function isElementIgnoredForAria(element) {
  return ["STYLE", "SCRIPT", "NOSCRIPT", "TEMPLATE"].includes(elementSafeTagName(element));
}
function isElementHiddenForAria(element) {
  if (isElementIgnoredForAria(element))
    return true;
  const style = getElementComputedStyle(element);
  const isSlot = element.nodeName === "SLOT";
  if ((style == null ? void 0 : style.display) === "contents" && !isSlot) {
    for (let child = element.firstChild; child; child = child.nextSibling) {
      if (child.nodeType === 1 && !isElementHiddenForAria(child))
        return false;
      if (child.nodeType === 3 && isVisibleTextNode(child))
        return false;
    }
    return true;
  }
  const isOptionInsideSelect = element.nodeName === "OPTION" && !!element.closest("select");
  if (!isOptionInsideSelect && !isSlot && !isElementStyleVisibilityVisible(element, style))
    return true;
  return belongsToDisplayNoneOrAriaHiddenOrNonSlotted(element);
}
function belongsToDisplayNoneOrAriaHiddenOrNonSlotted(element) {
  let hidden = cacheIsHidden == null ? void 0 : cacheIsHidden.get(element);
  if (hidden === void 0) {
    hidden = false;
    if (element.parentElement && element.parentElement.shadowRoot && !element.assignedSlot)
      hidden = true;
    if (!hidden) {
      const style = getElementComputedStyle(element);
      hidden = !style || style.display === "none" || getAriaBoolean(element.getAttribute("aria-hidden")) === true;
    }
    if (!hidden) {
      const parent = parentElementOrShadowHost(element);
      if (parent)
        hidden = belongsToDisplayNoneOrAriaHiddenOrNonSlotted(parent);
    }
    cacheIsHidden == null ? void 0 : cacheIsHidden.set(element, hidden);
  }
  return hidden;
}
function getIdRefs(element, ref) {
  if (!ref)
    return [];
  const root = enclosingShadowRootOrDocument(element);
  if (!root)
    return [];
  try {
    const ids = ref.split(" ").filter((id) => !!id);
    const result = [];
    for (const id of ids) {
      const firstElement = root.querySelector("#" + CSS.escape(id));
      if (firstElement && !result.includes(firstElement))
        result.push(firstElement);
    }
    return result;
  } catch (e) {
    return [];
  }
}
function trimFlatString(s) {
  return s.trim();
}
function asFlatString(s) {
  return s.split("\xA0").map((chunk) => chunk.replace(/\r\n/g, "\n").replace(/[\u200b\u00ad]/g, "").replace(/\s\s*/g, " ")).join("\xA0").trim();
}
function queryInAriaOwned(element, selector) {
  const result = [...element.querySelectorAll(selector)];
  for (const owned of getIdRefs(element, element.getAttribute("aria-owns"))) {
    if (owned.matches(selector))
      result.push(owned);
    result.push(...owned.querySelectorAll(selector));
  }
  return result;
}
function getCSSContent(element, pseudo) {
  const cache = pseudo === "::before" ? cachePseudoContentBefore : pseudo === "::after" ? cachePseudoContentAfter : cachePseudoContent;
  if (cache == null ? void 0 : cache.has(element))
    return cache == null ? void 0 : cache.get(element);
  const style = getElementComputedStyle(element, pseudo);
  let content;
  if (style) {
    const contentValue = style.content;
    if (contentValue && contentValue !== "none" && contentValue !== "normal") {
      if (style.display !== "none" && style.visibility !== "hidden") {
        content = parseCSSContentPropertyAsString(element, contentValue, !!pseudo);
      }
    }
  }
  if (pseudo && content !== void 0) {
    const display = (style == null ? void 0 : style.display) || "inline";
    if (display !== "inline")
      content = " " + content + " ";
  }
  if (cache)
    cache.set(element, content);
  return content;
}
function parseCSSContentPropertyAsString(element, content, isPseudo) {
  if (!content || content === "none" || content === "normal") {
    return;
  }
  try {
    let tokens = tokenize(content).filter((token) => !(token instanceof WhitespaceToken));
    const delimIndex = tokens.findIndex((token) => token instanceof DelimToken && token.value === "/");
    if (delimIndex !== -1) {
      tokens = tokens.slice(delimIndex + 1);
    } else if (!isPseudo) {
      return;
    }
    const accumulated = [];
    let index = 0;
    while (index < tokens.length) {
      if (tokens[index] instanceof StringToken) {
        accumulated.push(tokens[index].value);
        index++;
      } else if (index + 2 < tokens.length && tokens[index] instanceof FunctionToken && tokens[index].value === "attr" && tokens[index + 1] instanceof IdentToken && tokens[index + 2] instanceof CloseParenToken) {
        const attrName = tokens[index + 1].value;
        accumulated.push(element.getAttribute(attrName) || "");
        index += 3;
      } else {
        return;
      }
    }
    return accumulated.join("");
  } catch {
  }
}
function getAriaLabelledByElements(element) {
  const ref = element.getAttribute("aria-labelledby");
  if (ref === null)
    return null;
  const refs = getIdRefs(element, ref);
  return refs.length ? refs : null;
}
function allowsNameFromContent(role, targetDescendant) {
  const alwaysAllowsNameFromContent = ["button", "cell", "checkbox", "columnheader", "gridcell", "heading", "link", "menuitem", "menuitemcheckbox", "menuitemradio", "option", "radio", "row", "rowheader", "switch", "tab", "tooltip", "treeitem"].includes(role);
  const descendantAllowsNameFromContent = targetDescendant && ["", "caption", "code", "contentinfo", "definition", "deletion", "emphasis", "insertion", "list", "listitem", "mark", "none", "paragraph", "presentation", "region", "row", "rowgroup", "section", "strong", "subscript", "superscript", "table", "term", "time"].includes(role);
  return alwaysAllowsNameFromContent || descendantAllowsNameFromContent;
}
function computeAccessibleNameComposite(element, includeHidden, collectElements) {
  const elementProhibitsNaming = ["caption", "code", "definition", "deletion", "emphasis", "generic", "insertion", "mark", "paragraph", "presentation", "strong", "subscript", "suggestion", "superscript", "term", "time"].includes(getAriaRole(element) || "");
  if (elementProhibitsNaming)
    return { ...emptyCompositeString(), derivedFromContent: false };
  const outDerivedFromContent = { value: false };
  const result = getTextAlternativeInternal(element, {
    includeHidden,
    collectElements,
    outDerivedFromContent,
    visitedElements: /* @__PURE__ */ new Set(),
    embeddedInTargetElement: "self"
  });
  return { text: asFlatString(result.text), elements: result.elements, derivedFromContent: outDerivedFromContent.value };
}
function getElementAccessibleName(element, includeHidden) {
  const cache = includeHidden ? cacheAccessibleNameHidden : cacheAccessibleName;
  let accessibleName = cache == null ? void 0 : cache.get(element);
  if (accessibleName === void 0) {
    accessibleName = computeAccessibleNameComposite(
      element,
      includeHidden,
      true
      /* collectElements */
    );
    cache == null ? void 0 : cache.set(element, accessibleName);
  }
  return accessibleName;
}
function getElementAccessibleNameText(element, includeHidden) {
  var _a;
  const composite = (_a = includeHidden ? cacheAccessibleNameHidden : cacheAccessibleName) == null ? void 0 : _a.get(element);
  if (composite !== void 0)
    return composite.text;
  const cache = includeHidden ? cacheAccessibleNameTextHidden : cacheAccessibleNameText;
  let text = cache == null ? void 0 : cache.get(element);
  if (text === void 0) {
    text = computeAccessibleNameComposite(
      element,
      includeHidden,
      false
      /* collectElements */
    ).text;
    cache == null ? void 0 : cache.set(element, text);
  }
  return text;
}
function getElementAccessibleDescription(element, includeHidden) {
  const cache = includeHidden ? cacheAccessibleDescriptionHidden : cacheAccessibleDescription;
  let accessibleDescription = cache == null ? void 0 : cache.get(element);
  if (accessibleDescription === void 0) {
    accessibleDescription = { text: "", derivedFromContent: false };
    if (element.hasAttribute("aria-describedby")) {
      const describedBy = getIdRefs(element, element.getAttribute("aria-describedby"));
      accessibleDescription.text = asFlatString(describedBy.map((ref) => getTextAlternativeInternal(ref, {
        includeHidden,
        visitedElements: /* @__PURE__ */ new Set(),
        embeddedInDescribedBy: { element: ref, hidden: isElementHiddenForAria(ref) }
      }).text).join(" "));
      accessibleDescription.derivedFromContent = describedBy.some((ref) => ref === element || element.contains(ref));
    } else if (element.hasAttribute("aria-description")) {
      accessibleDescription.text = asFlatString(element.getAttribute("aria-description") || "");
    } else {
      accessibleDescription.text = asFlatString(element.getAttribute("title") || "");
    }
    cache == null ? void 0 : cache.set(element, accessibleDescription);
  }
  return accessibleDescription;
}
var kAriaInvalidRoles = [
  "application",
  "checkbox",
  "columnheader",
  "combobox",
  "gridcell",
  "listbox",
  "radiogroup",
  "rowheader",
  "searchbox",
  "slider",
  "spinbutton",
  "switch",
  "textbox",
  "tree"
];
function getAriaInvalid(element) {
  const ariaInvalid = element.getAttribute("aria-invalid");
  if (!ariaInvalid || ariaInvalid.trim() === "" || ariaInvalid.toLocaleLowerCase() === "false")
    return "false";
  if (ariaInvalid === "true" || ariaInvalid === "grammar" || ariaInvalid === "spelling")
    return ariaInvalid;
  return "true";
}
function getValidityInvalid(element) {
  if ("validity" in element) {
    const validity = element.validity;
    return (validity == null ? void 0 : validity.valid) === false;
  }
  return false;
}
function getElementAccessibleErrorMessage(element) {
  const cache = cacheAccessibleErrorMessage;
  let accessibleErrorMessage = cacheAccessibleErrorMessage == null ? void 0 : cacheAccessibleErrorMessage.get(element);
  if (accessibleErrorMessage === void 0) {
    accessibleErrorMessage = "";
    const isAriaInvalid = getAriaInvalid(element) !== "false";
    const isValidityInvalid = getValidityInvalid(element);
    if (isAriaInvalid || isValidityInvalid) {
      const errorMessageId = element.getAttribute("aria-errormessage");
      const errorMessages = getIdRefs(element, errorMessageId);
      const parts = errorMessages.map((errorMessage) => asFlatString(
        getTextAlternativeInternal(errorMessage, {
          visitedElements: /* @__PURE__ */ new Set(),
          embeddedInDescribedBy: { element: errorMessage, hidden: isElementHiddenForAria(errorMessage) }
        }).text
      ));
      accessibleErrorMessage = parts.join(" ").trim();
    }
    cache == null ? void 0 : cache.set(element, accessibleErrorMessage);
  }
  return accessibleErrorMessage;
}
function insideTargetElement(options) {
  return options.embeddedInTargetElement === "self" || options.embeddedInTargetElement === "descendant";
}
function getTextAlternativeInternal(element, options) {
  var _a, _b, _c, _d, _e;
  if (options.visitedElements.has(element))
    return emptyCompositeString();
  const childOptions = {
    ...options,
    embeddedInTargetElement: options.embeddedInTargetElement === "self" ? "descendant" : options.embeddedInTargetElement
  };
  if (!options.includeHidden) {
    const isEmbeddedInHiddenReferenceTraversal = !!((_a = options.embeddedInLabelledBy) == null ? void 0 : _a.hidden) || !!((_b = options.embeddedInDescribedBy) == null ? void 0 : _b.hidden) || !!((_c = options.embeddedInNativeTextAlternative) == null ? void 0 : _c.hidden) || !!((_d = options.embeddedInLabel) == null ? void 0 : _d.hidden);
    if (isElementIgnoredForAria(element) || !isEmbeddedInHiddenReferenceTraversal && isElementHiddenForAria(element)) {
      options.visitedElements.add(element);
      return emptyCompositeString();
    }
  }
  const labelledBy = getAriaLabelledByElements(element);
  if (!options.embeddedInLabelledBy) {
    const accessibleName = joinCompositeString((labelledBy || []).map((ref) => getTextAlternativeInternal(ref, {
      ...options,
      embeddedInLabelledBy: { element: ref, hidden: isElementHiddenForAria(ref) },
      embeddedInDescribedBy: void 0,
      embeddedInTargetElement: void 0,
      embeddedInLabel: void 0,
      embeddedInNativeTextAlternative: void 0
    })), " ", options.collectElements);
    if (accessibleName.text) {
      if (options.outDerivedFromContent && insideTargetElement(options) && (labelledBy || []).some((ref) => ref === element || element.contains(ref)))
        options.outDerivedFromContent.value = true;
      return accessibleName;
    }
  }
  const role = getAriaRole(element) || "";
  const tagName = elementSafeTagName(element);
  if (!!options.embeddedInLabel || !!options.embeddedInLabelledBy || options.embeddedInTargetElement === "descendant") {
    const isOwnLabel = [...element.labels || []].includes(element);
    const isOwnLabelledBy = (labelledBy || []).includes(element);
    if (!isOwnLabel && !isOwnLabelledBy) {
      if (role === "textbox" || role === "searchbox") {
        options.visitedElements.add(element);
        if (tagName === "INPUT" || tagName === "TEXTAREA")
          return compositeString(element.value, element, options.collectElements);
        return compositeString(element.textContent, element, options.collectElements);
      }
      if (["combobox", "listbox"].includes(role)) {
        options.visitedElements.add(element);
        let selectedOptions;
        if (tagName === "SELECT") {
          selectedOptions = [...element.selectedOptions];
          if (!selectedOptions.length && element.options.length)
            selectedOptions.push(element.options[0]);
        } else {
          const listbox = role === "combobox" ? queryInAriaOwned(element, "*").find((e) => getAriaRole(e) === "listbox") : element;
          selectedOptions = listbox ? queryInAriaOwned(listbox, '[aria-selected="true"]').filter((e) => getAriaRole(e) === "option") : [];
        }
        if (!selectedOptions.length && tagName === "INPUT") {
          return compositeString(element.value, element, options.collectElements);
        }
        return joinCompositeString(selectedOptions.map((option) => getTextAlternativeInternal(option, childOptions)), " ", options.collectElements);
      }
      if (["progressbar", "scrollbar", "slider", "spinbutton", "meter"].includes(role)) {
        options.visitedElements.add(element);
        if (element.hasAttribute("aria-valuetext"))
          return compositeString(element.getAttribute("aria-valuetext"), element, options.collectElements);
        if (element.hasAttribute("aria-valuenow"))
          return compositeString(element.getAttribute("aria-valuenow"), element, options.collectElements);
        return compositeString(element.getAttribute("value"), element, options.collectElements);
      }
      if (["menu"].includes(role)) {
        options.visitedElements.add(element);
        return emptyCompositeString();
      }
    }
  }
  const ariaLabel = element.getAttribute("aria-label") || "";
  if (trimFlatString(ariaLabel)) {
    options.visitedElements.add(element);
    return compositeString(ariaLabel, element, options.collectElements);
  }
  if (!["presentation", "none"].includes(role)) {
    if (tagName === "INPUT" && ["button", "submit", "reset"].includes(element.type)) {
      options.visitedElements.add(element);
      const value = element.value || "";
      if (trimFlatString(value))
        return compositeString(value, element, options.collectElements);
      if (element.type === "submit")
        return compositeString("Submit", element, options.collectElements);
      if (element.type === "reset")
        return compositeString("Reset", element, options.collectElements);
      const title = element.getAttribute("title") || "";
      return compositeString(title, element, options.collectElements);
    }
    if (tagName === "INPUT" && element.type === "file") {
      options.visitedElements.add(element);
      const labels = element.labels || [];
      if (labels.length && !options.embeddedInLabelledBy)
        return getAccessibleNameFromAssociatedLabels(labels, options);
      return compositeString("Choose File", element, options.collectElements);
    }
    if (tagName === "INPUT" && element.type === "image") {
      options.visitedElements.add(element);
      const labels = element.labels || [];
      if (labels.length && !options.embeddedInLabelledBy)
        return getAccessibleNameFromAssociatedLabels(labels, options);
      const alt = element.getAttribute("alt") || "";
      if (trimFlatString(alt))
        return compositeString(alt, element, options.collectElements);
      const title = element.getAttribute("title") || "";
      if (trimFlatString(title))
        return compositeString(title, element, options.collectElements);
      return compositeString("Submit", element, options.collectElements);
    }
    if (!labelledBy && tagName === "BUTTON") {
      options.visitedElements.add(element);
      const labels = element.labels || [];
      if (labels.length)
        return getAccessibleNameFromAssociatedLabels(labels, options);
    }
    if (!labelledBy && tagName === "OUTPUT") {
      options.visitedElements.add(element);
      const labels = element.labels || [];
      if (labels.length)
        return getAccessibleNameFromAssociatedLabels(labels, options);
      return compositeString(element.getAttribute("title") || "", element, options.collectElements);
    }
    if (!labelledBy && (tagName === "TEXTAREA" || tagName === "SELECT" || tagName === "INPUT" || tagName === "METER" || tagName === "PROGRESS")) {
      options.visitedElements.add(element);
      const labels = element.labels || [];
      if (labels.length)
        return getAccessibleNameFromAssociatedLabels(labels, options);
      const usePlaceholder = tagName === "INPUT" && ["text", "password", "number", "search", "tel", "email", "url"].includes(element.type) || tagName === "TEXTAREA";
      const placeholder = element.getAttribute("placeholder") || "";
      const title = element.getAttribute("title") || "";
      if (!usePlaceholder || title)
        return compositeString(title, element, options.collectElements);
      return compositeString(placeholder, element, options.collectElements);
    }
    if (!labelledBy && tagName === "FIELDSET") {
      options.visitedElements.add(element);
      for (let child = element.firstElementChild; child; child = child.nextElementSibling) {
        if (elementSafeTagName(child) === "LEGEND") {
          return getTextAlternativeInternal(child, {
            ...childOptions,
            embeddedInNativeTextAlternative: { element: child, hidden: isElementHiddenForAria(child) }
          });
        }
      }
      const title = element.getAttribute("title") || "";
      return compositeString(title, element, options.collectElements);
    }
    if (!labelledBy && tagName === "FIGURE") {
      options.visitedElements.add(element);
      for (let child = element.firstElementChild; child; child = child.nextElementSibling) {
        if (elementSafeTagName(child) === "FIGCAPTION") {
          return getTextAlternativeInternal(child, {
            ...childOptions,
            embeddedInNativeTextAlternative: { element: child, hidden: isElementHiddenForAria(child) }
          });
        }
      }
      const title = element.getAttribute("title") || "";
      return compositeString(title, element, options.collectElements);
    }
    if (tagName === "IMG") {
      options.visitedElements.add(element);
      const alt = element.getAttribute("alt") || "";
      if (trimFlatString(alt))
        return compositeString(alt, element, options.collectElements);
      const title = element.getAttribute("title") || "";
      return compositeString(title, element, options.collectElements);
    }
    if (tagName === "TABLE") {
      options.visitedElements.add(element);
      for (let child = element.firstElementChild; child; child = child.nextElementSibling) {
        if (elementSafeTagName(child) === "CAPTION") {
          return getTextAlternativeInternal(child, {
            ...childOptions,
            embeddedInNativeTextAlternative: { element: child, hidden: isElementHiddenForAria(child) }
          });
        }
      }
      const summary = element.getAttribute("summary") || "";
      if (summary)
        return compositeString(summary, element, options.collectElements);
    }
    if (tagName === "AREA") {
      options.visitedElements.add(element);
      const alt = element.getAttribute("alt") || "";
      if (trimFlatString(alt))
        return compositeString(alt, element, options.collectElements);
      const title = element.getAttribute("title") || "";
      return compositeString(title, element, options.collectElements);
    }
    if (tagName === "SVG" || element.ownerSVGElement) {
      options.visitedElements.add(element);
      for (let child = element.firstElementChild; child; child = child.nextElementSibling) {
        if (elementSafeTagName(child) === "TITLE" && child.ownerSVGElement) {
          return getTextAlternativeInternal(child, {
            ...childOptions,
            embeddedInLabelledBy: { element: child, hidden: isElementHiddenForAria(child) }
          });
        }
      }
    }
    if (element.ownerSVGElement && tagName === "A") {
      const title = element.getAttribute("xlink:title") || "";
      if (trimFlatString(title)) {
        options.visitedElements.add(element);
        return compositeString(title, element, options.collectElements);
      }
    }
  }
  const shouldNameFromContentForSummary = tagName === "SUMMARY" && !["presentation", "none"].includes(role);
  if (allowsNameFromContent(role, options.embeddedInTargetElement === "descendant") || shouldNameFromContentForSummary || !!options.embeddedInLabelledBy || !!options.embeddedInDescribedBy || !!options.embeddedInLabel || !!options.embeddedInNativeTextAlternative) {
    options.visitedElements.add(element);
    const accessibleName = innerAccumulatedElementText(element, childOptions);
    const maybeTrimmedAccessibleName = options.embeddedInTargetElement === "self" ? trimFlatString(accessibleName.text) : accessibleName.text;
    if (maybeTrimmedAccessibleName) {
      if (options.outDerivedFromContent && insideTargetElement(options) && trimFlatString(accessibleName.text))
        options.outDerivedFromContent.value = true;
      (_e = accessibleName.elements) == null ? void 0 : _e.add(element);
      return accessibleName;
    }
  }
  if (!["presentation", "none"].includes(role) || tagName === "IFRAME" || tagName === "FRAME") {
    options.visitedElements.add(element);
    const title = element.getAttribute("title") || "";
    if (trimFlatString(title))
      return compositeString(title, element, options.collectElements);
  }
  options.visitedElements.add(element);
  return emptyCompositeString();
}
function innerAccumulatedElementText(element, options) {
  const tokens = [];
  const elements = options.collectElements ? /* @__PURE__ */ new Set() : void 0;
  const visit3 = (node, skipSlotted) => {
    var _a;
    if (skipSlotted && node.assignedSlot)
      return;
    if (node.nodeType === 1) {
      const display = ((_a = getElementComputedStyle(node)) == null ? void 0 : _a.display) || "inline";
      const childComposite = getTextAlternativeInternal(node, options);
      let token = childComposite.text;
      for (const contributor of childComposite.elements || [])
        elements == null ? void 0 : elements.add(contributor);
      if (display !== "inline" || node.nodeName === "BR")
        token = " " + token + " ";
      tokens.push(token);
    } else if (node.nodeType === 3) {
      tokens.push(node.textContent || "");
    }
  };
  tokens.push(getCSSContent(element, "::before") || "");
  const content = getCSSContent(element);
  if (content !== void 0) {
    tokens.push(content);
  } else {
    const assignedNodes = element.nodeName === "SLOT" ? element.assignedNodes() : [];
    if (assignedNodes.length) {
      for (const child of assignedNodes)
        visit3(child, false);
    } else {
      for (let child = element.firstChild; child; child = child.nextSibling)
        visit3(child, true);
      if (element.shadowRoot) {
        for (let child = element.shadowRoot.firstChild; child; child = child.nextSibling)
          visit3(child, true);
      }
      for (const owned of getIdRefs(element, element.getAttribute("aria-owns")))
        visit3(owned, true);
    }
  }
  tokens.push(getCSSContent(element, "::after") || "");
  return { text: tokens.join(""), elements };
}
var kAriaSelectedRoles = ["gridcell", "option", "row", "tab", "rowheader", "columnheader", "treeitem"];
function getAriaSelected(element) {
  if (elementSafeTagName(element) === "OPTION")
    return element.selected;
  if (kAriaSelectedRoles.includes(getAriaRole(element) || ""))
    return getAriaBoolean(element.getAttribute("aria-selected")) === true;
  return false;
}
var kAriaCheckedRoles = ["checkbox", "menuitemcheckbox", "option", "radio", "switch", "menuitemradio", "treeitem"];
function getAriaChecked(element) {
  const result = getChecked(element, true);
  return result === "error" ? false : result;
}
function getCheckedAllowMixed(element) {
  return getChecked(element, true);
}
function getCheckedWithoutMixed(element) {
  const result = getChecked(element, false);
  return result;
}
function getChecked(element, allowMixed) {
  const tagName = elementSafeTagName(element);
  if (allowMixed && tagName === "INPUT" && element.indeterminate)
    return "mixed";
  if (tagName === "INPUT" && ["checkbox", "radio"].includes(element.type))
    return element.checked;
  if (kAriaCheckedRoles.includes(getAriaRole(element) || "")) {
    const checked = element.getAttribute("aria-checked");
    if (checked === "true")
      return true;
    if (allowMixed && checked === "mixed")
      return "mixed";
    return false;
  }
  return "error";
}
var kAriaReadonlyRoles = ["checkbox", "combobox", "grid", "gridcell", "listbox", "radiogroup", "slider", "spinbutton", "textbox", "columnheader", "rowheader", "searchbox", "switch", "treegrid"];
function getReadonly(element) {
  const tagName = elementSafeTagName(element);
  if (["INPUT", "TEXTAREA", "SELECT"].includes(tagName))
    return element.hasAttribute("readonly");
  if (kAriaReadonlyRoles.includes(getAriaRole(element) || ""))
    return element.getAttribute("aria-readonly") === "true";
  if (element.isContentEditable)
    return false;
  return "error";
}
var kAriaPressedRoles = ["button"];
function getAriaPressed(element) {
  if (kAriaPressedRoles.includes(getAriaRole(element) || "")) {
    const pressed = element.getAttribute("aria-pressed");
    if (pressed === "true")
      return true;
    if (pressed === "mixed")
      return "mixed";
  }
  return false;
}
var kAriaExpandedRoles = ["application", "button", "checkbox", "combobox", "gridcell", "link", "listbox", "menuitem", "row", "rowheader", "tab", "treeitem", "columnheader", "menuitemcheckbox", "menuitemradio", "rowheader", "switch"];
function getAriaExpanded(element) {
  if (elementSafeTagName(element) === "DETAILS")
    return element.open;
  if (kAriaExpandedRoles.includes(getAriaRole(element) || "")) {
    const expanded = element.getAttribute("aria-expanded");
    if (expanded === null)
      return void 0;
    if (expanded === "true")
      return true;
    return false;
  }
  return void 0;
}
var kAriaLevelRoles = ["heading", "listitem", "row", "treeitem"];
function getAriaLevel(element) {
  const native = { "H1": 1, "H2": 2, "H3": 3, "H4": 4, "H5": 5, "H6": 6 }[elementSafeTagName(element)];
  if (native)
    return native;
  if (kAriaLevelRoles.includes(getAriaRole(element) || "")) {
    const attr = element.getAttribute("aria-level");
    const value = attr === null ? Number.NaN : Number(attr);
    if (Number.isInteger(value) && value >= 1)
      return value;
  }
  return 0;
}
var kAriaDisabledRoles = ["application", "button", "composite", "gridcell", "group", "input", "link", "menuitem", "scrollbar", "separator", "tab", "checkbox", "columnheader", "combobox", "grid", "listbox", "menu", "menubar", "menuitemcheckbox", "menuitemradio", "option", "radio", "radiogroup", "row", "rowheader", "searchbox", "select", "slider", "spinbutton", "switch", "tablist", "textbox", "toolbar", "tree", "treegrid", "treeitem"];
function getAriaDisabled(element) {
  return isNativelyDisabled(element) || hasExplicitAriaDisabled(element);
}
function isNativelyDisabled(element) {
  const isNativeFormControl = ["BUTTON", "INPUT", "SELECT", "TEXTAREA", "OPTION", "OPTGROUP"].includes(elementSafeTagName(element));
  return isNativeFormControl && (element.hasAttribute("disabled") || belongsToDisabledOptGroup(element) || belongsToDisabledFieldSet(element));
}
function belongsToDisabledOptGroup(element) {
  return elementSafeTagName(element) === "OPTION" && !!element.closest("OPTGROUP[DISABLED]");
}
function belongsToDisabledFieldSet(element) {
  const fieldSetElement = element == null ? void 0 : element.closest("FIELDSET[DISABLED]");
  if (!fieldSetElement)
    return false;
  const legendElement = fieldSetElement.querySelector(":scope > LEGEND");
  return !legendElement || !legendElement.contains(element);
}
function hasExplicitAriaDisabled(element) {
  if (!kAriaDisabledRoles.includes(getAriaRole(element) || ""))
    return false;
  return hasAriaDisabledInChain(element);
}
function hasAriaDisabledInChain(element) {
  let result = cacheAriaDisabled == null ? void 0 : cacheAriaDisabled.get(element);
  if (result === void 0) {
    const attribute = (element.getAttribute("aria-disabled") || "").toLowerCase();
    if (attribute === "true") {
      result = true;
    } else if (attribute === "false") {
      result = false;
    } else {
      const parent = parentElementOrShadowHost(element);
      result = parent ? hasAriaDisabledInChain(parent) : false;
    }
    cacheAriaDisabled == null ? void 0 : cacheAriaDisabled.set(element, result);
  }
  return result;
}
function getAccessibleNameFromAssociatedLabels(labels, options) {
  return joinCompositeString([...labels].map((label) => getTextAlternativeInternal(label, {
    ...options,
    embeddedInLabel: { element: label, hidden: isElementHiddenForAria(label) },
    embeddedInNativeTextAlternative: void 0,
    embeddedInLabelledBy: void 0,
    embeddedInDescribedBy: void 0,
    embeddedInTargetElement: void 0
  })).filter((accessibleName) => !!accessibleName.text), " ", options.collectElements);
}
function receivesPointerEvents(element) {
  const cache = cachePointerEvents;
  let e = element;
  let result;
  const parents = [];
  for (; e; e = parentElementOrShadowHost(e)) {
    const cached = cache.get(e);
    if (cached !== void 0) {
      result = cached;
      break;
    }
    parents.push(e);
    const style = getElementComputedStyle(e);
    if (!style) {
      result = true;
      break;
    }
    const value = style.pointerEvents;
    if (value) {
      result = value !== "none";
      break;
    }
  }
  if (result === void 0)
    result = true;
  for (const parent of parents)
    cache.set(parent, result);
  return result;
}
var cacheAccessibleName;
var cacheAccessibleNameHidden;
var cacheAccessibleNameText;
var cacheAccessibleNameTextHidden;
var cacheAccessibleDescription;
var cacheAccessibleDescriptionHidden;
var cacheAccessibleErrorMessage;
var cacheIsHidden;
var cachePseudoContent;
var cachePseudoContentBefore;
var cachePseudoContentAfter;
var cachePointerEvents;
var cacheAriaRole;
var cacheAriaDisabled;
var cachesCounter2 = 0;
function beginAriaCaches() {
  beginDOMCaches();
  ++cachesCounter2;
  cacheAriaRole != null ? cacheAriaRole : cacheAriaRole = /* @__PURE__ */ new Map();
  cacheAriaDisabled != null ? cacheAriaDisabled : cacheAriaDisabled = /* @__PURE__ */ new Map();
  cacheAccessibleName != null ? cacheAccessibleName : cacheAccessibleName = /* @__PURE__ */ new Map();
  cacheAccessibleNameHidden != null ? cacheAccessibleNameHidden : cacheAccessibleNameHidden = /* @__PURE__ */ new Map();
  cacheAccessibleNameText != null ? cacheAccessibleNameText : cacheAccessibleNameText = /* @__PURE__ */ new Map();
  cacheAccessibleNameTextHidden != null ? cacheAccessibleNameTextHidden : cacheAccessibleNameTextHidden = /* @__PURE__ */ new Map();
  cacheAccessibleDescription != null ? cacheAccessibleDescription : cacheAccessibleDescription = /* @__PURE__ */ new Map();
  cacheAccessibleDescriptionHidden != null ? cacheAccessibleDescriptionHidden : cacheAccessibleDescriptionHidden = /* @__PURE__ */ new Map();
  cacheAccessibleErrorMessage != null ? cacheAccessibleErrorMessage : cacheAccessibleErrorMessage = /* @__PURE__ */ new Map();
  cacheIsHidden != null ? cacheIsHidden : cacheIsHidden = /* @__PURE__ */ new Map();
  cachePseudoContent != null ? cachePseudoContent : cachePseudoContent = /* @__PURE__ */ new Map();
  cachePseudoContentBefore != null ? cachePseudoContentBefore : cachePseudoContentBefore = /* @__PURE__ */ new Map();
  cachePseudoContentAfter != null ? cachePseudoContentAfter : cachePseudoContentAfter = /* @__PURE__ */ new Map();
  cachePointerEvents != null ? cachePointerEvents : cachePointerEvents = /* @__PURE__ */ new Map();
}
function endAriaCaches() {
  if (!--cachesCounter2) {
    cacheAccessibleName = void 0;
    cacheAccessibleNameHidden = void 0;
    cacheAccessibleNameText = void 0;
    cacheAccessibleNameTextHidden = void 0;
    cacheAccessibleDescription = void 0;
    cacheAccessibleDescriptionHidden = void 0;
    cacheAccessibleErrorMessage = void 0;
    cacheIsHidden = void 0;
    cachePseudoContent = void 0;
    cachePseudoContentBefore = void 0;
    cachePseudoContentAfter = void 0;
    cachePointerEvents = void 0;
    cacheAriaRole = void 0;
    cacheAriaDisabled = void 0;
  }
  endDOMCaches();
}
var inputTypeToRole = {
  "button": "button",
  "checkbox": "checkbox",
  "image": "button",
  "number": "spinbutton",
  "radio": "radio",
  "range": "slider",
  "reset": "button",
  "submit": "button"
};
function emptyCompositeString() {
  return { text: "" };
}
function compositeString(text, element, collectElements) {
  const elements = text && collectElements ? /* @__PURE__ */ new Set([element]) : void 0;
  return { text: text || "", elements };
}
function joinCompositeString(parts, separator, collectElements) {
  let elements;
  if (collectElements) {
    elements = /* @__PURE__ */ new Set();
    for (const part of parts) {
      for (const element of part.elements || [])
        elements.add(element);
    }
  }
  return { text: parts.map((part) => part.text).join(separator), elements };
}

// packages/injected/src/ariaSnapshot.ts
var lastRef = 0;
function toInternalOptions(options) {
  const renderBoxes = options.boxes;
  if (options.mode === "ai") {
    return {
      visibility: "ariaOrVisible",
      refs: "interactable",
      refPrefix: options.refPrefix,
      includeGenericRole: true,
      renderActive: !options.doNotRenderActive,
      renderCursorPointer: true,
      renderBoxes
    };
  }
  if (options.mode === "autoexpect") {
    return { visibility: "ariaAndVisible", refs: "none", renderBoxes };
  }
  return { visibility: "aria", refs: "none", renderBoxes };
}
function generateAriaTree(rootElement, publicOptions) {
  const options = toInternalOptions(publicOptions);
  const visited = /* @__PURE__ */ new Set();
  const nameSourceElements = /* @__PURE__ */ new Map();
  const snapshot = {
    root: { role: "fragment", name: "", children: [], props: {}, box: computeBox(rootElement), receivesPointerEvents: true },
    info: /* @__PURE__ */ new Map(),
    refs: /* @__PURE__ */ new Map(),
    iframeRefs: []
  };
  setAriaNodeElement(snapshot.root, rootElement);
  const visit3 = (ariaNode, node, parentElementVisible) => {
    var _a;
    if (visited.has(node))
      return;
    visited.add(node);
    if (node.nodeType === Node.TEXT_NODE && node.nodeValue) {
      if (!parentElementVisible)
        return;
      const text = node.nodeValue;
      if (ariaNode.role !== "textbox" && text)
        ariaNode.children.push(node.nodeValue || "");
      return;
    }
    if (node.nodeType !== Node.ELEMENT_NODE)
      return;
    const element = node;
    const isElementVisibleForAria = !isElementHiddenForAria(element);
    let visible = isElementVisibleForAria;
    if (options.visibility === "ariaOrVisible")
      visible = isElementVisibleForAria || isElementVisible(element);
    if (options.visibility === "ariaAndVisible")
      visible = isElementVisibleForAria && isElementVisible(element);
    if (options.visibility === "aria" && !visible)
      return;
    const ariaChildren = [];
    if (element.hasAttribute("aria-owns")) {
      const ids = element.getAttribute("aria-owns").split(/\s+/);
      for (const id of ids) {
        const ownedElement = rootElement.ownerDocument.getElementById(id);
        if (ownedElement)
          ariaChildren.push(ownedElement);
      }
    }
    const childAriaNode = visible ? toAriaNode(element, options, nameSourceElements) : null;
    if (childAriaNode && ((_a = element.getAttribute("aria-hidden")) == null ? void 0 : _a.toLowerCase()) === "true")
      childAriaNode.props["aria-hidden"] = "true";
    let elementInfo;
    if (childAriaNode) {
      if (childAriaNode.ref) {
        elementInfo = { element, nameFromContentRefs: [] };
        snapshot.info.set(childAriaNode.ref, elementInfo);
        snapshot.refs.set(element, childAriaNode.ref);
        if (childAriaNode.role === "iframe")
          snapshot.iframeRefs.push(childAriaNode.ref);
      }
      ariaNode.children.push(childAriaNode);
    }
    processElement(childAriaNode || ariaNode, element, ariaChildren, visible);
    if (elementInfo) {
      for (const contributor of nameSourceElements.get(childAriaNode) || []) {
        const ref = snapshot.refs.get(contributor);
        if (ref && ref !== childAriaNode.ref)
          elementInfo.nameFromContentRefs.push(ref);
      }
    }
  };
  function processElement(ariaNode, element, ariaChildren, parentElementVisible) {
    var _a;
    const display = ((_a = getElementComputedStyle(element)) == null ? void 0 : _a.display) || "inline";
    const treatAsBlock = display !== "inline" || element.nodeName === "BR" ? " " : "";
    if (treatAsBlock)
      ariaNode.children.push(treatAsBlock);
    ariaNode.children.push(getCSSContent(element, "::before") || "");
    const assignedNodes = element.nodeName === "SLOT" ? element.assignedNodes() : [];
    if (assignedNodes.length) {
      for (const child of assignedNodes)
        visit3(ariaNode, child, parentElementVisible);
    } else {
      for (let child = element.firstChild; child; child = child.nextSibling) {
        if (!child.assignedSlot)
          visit3(ariaNode, child, parentElementVisible);
      }
      if (element.shadowRoot) {
        for (let child = element.shadowRoot.firstChild; child; child = child.nextSibling)
          visit3(ariaNode, child, parentElementVisible);
      }
    }
    for (const child of ariaChildren)
      visit3(ariaNode, child, parentElementVisible);
    ariaNode.children.push(getCSSContent(element, "::after") || "");
    if (treatAsBlock)
      ariaNode.children.push(treatAsBlock);
    if (ariaNode.children.length === 1 && ariaNode.name === ariaNode.children[0])
      ariaNode.children = [];
    if (ariaNode.role === "link" && element.hasAttribute("href")) {
      const href = element.getAttribute("href");
      ariaNode.props["url"] = truncateDataUrl(href);
    }
    if (ariaNode.role === "textbox" && element.hasAttribute("placeholder") && element.getAttribute("placeholder") !== ariaNode.name) {
      const placeholder = element.getAttribute("placeholder");
      ariaNode.props["placeholder"] = placeholder;
    }
  }
  beginAriaCaches();
  try {
    visit3(snapshot.root, rootElement, true);
  } finally {
    endAriaCaches();
  }
  distillAriaSnapshot(snapshot, publicOptions);
  return snapshot;
}
function computeAriaRef(ariaNode, options) {
  var _a;
  if (options.refs === "none")
    return;
  if (options.refs === "interactable" && (!ariaNode.box.visible || !ariaNode.receivesPointerEvents))
    return;
  const element = ariaNodeElement(ariaNode);
  let ariaRef = element._ariaRef;
  if (!ariaRef || ariaRef.role !== ariaNode.role || ariaRef.name !== ariaNode.name) {
    ariaRef = { role: ariaNode.role, name: ariaNode.name, ref: ((_a = options.refPrefix) != null ? _a : "") + "e" + ++lastRef };
    element._ariaRef = ariaRef;
  }
  ariaNode.ref = ariaRef.ref;
}
function toAriaNode(element, options, nameSourceElements) {
  var _a;
  const active = element.ownerDocument.activeElement === element && element.ownerDocument.hasFocus();
  if (element.nodeName === "IFRAME" || element.nodeName === "FRAME") {
    const ariaNode = {
      role: "iframe",
      name: "",
      children: [],
      props: {},
      box: computeBox(element),
      receivesPointerEvents: true,
      active
    };
    setAriaNodeElement(ariaNode, element);
    computeAriaRef(ariaNode, options);
    return ariaNode;
  }
  const defaultRole = options.includeGenericRole ? "generic" : null;
  const role = (_a = getAriaRole(element)) != null ? _a : defaultRole;
  if (!role || role === "presentation" || role === "none")
    return null;
  const name = getElementAccessibleName(element, false);
  const receivesPointerEvents2 = receivesPointerEvents(element);
  const box = computeBox(element);
  if (role === "generic" && box.inline && element.childNodes.length === 1 && element.childNodes[0].nodeType === Node.TEXT_NODE)
    return null;
  const result = {
    role,
    name: normalizeWhiteSpace(name.text),
    children: [],
    props: {},
    box,
    receivesPointerEvents: receivesPointerEvents2,
    active
  };
  setAriaNodeElement(result, element);
  nameSourceElements.set(result, name.elements);
  computeAriaRef(result, options);
  if (kAriaCheckedRoles.includes(role))
    result.checked = getAriaChecked(element);
  if (kAriaDisabledRoles.includes(role))
    result.disabled = getAriaDisabled(element);
  if (kAriaExpandedRoles.includes(role))
    result.expanded = getAriaExpanded(element);
  if (kAriaInvalidRoles.includes(role)) {
    const invalid = getAriaInvalid(element);
    result.invalid = invalid === "false" ? false : invalid === "true" ? true : invalid;
  }
  if (kAriaLevelRoles.includes(role))
    result.level = getAriaLevel(element);
  if (kAriaPressedRoles.includes(role))
    result.pressed = getAriaPressed(element);
  if (kAriaSelectedRoles.includes(role))
    result.selected = getAriaSelected(element);
  if (element instanceof HTMLInputElement || element instanceof HTMLTextAreaElement) {
    if (element.type !== "checkbox" && element.type !== "radio" && element.type !== "file")
      result.children = [element.value];
  }
  return result;
}
function matchesStringOrRegex(text, template) {
  if (!template)
    return true;
  if (!text)
    return false;
  if (typeof template === "string")
    return text === template;
  return !!text.match(new RegExp(template.pattern));
}
function matchesTextValue(text, template) {
  if (!(template == null ? void 0 : template.normalized))
    return true;
  if (!text)
    return false;
  if (text === template.normalized)
    return true;
  if (text === template.raw)
    return true;
  const regex = cachedRegex(template);
  if (regex)
    return !!text.match(regex);
  return false;
}
var cachedRegexSymbol = /* @__PURE__ */ Symbol("cachedRegex");
function cachedRegex(template) {
  if (template[cachedRegexSymbol] !== void 0)
    return template[cachedRegexSymbol];
  const { raw } = template;
  const canBeRegex = raw.startsWith("/") && raw.endsWith("/") && raw.length > 1;
  let regex;
  try {
    regex = canBeRegex ? new RegExp(raw.slice(1, -1)) : null;
  } catch (e) {
    regex = null;
  }
  template[cachedRegexSymbol] = regex;
  return regex;
}
function matchesExpectAriaTemplate(rootElement, template) {
  const snapshot = generateAriaTree(rootElement, { mode: "default" });
  const matches = matchesNodeDeep(snapshot.root, template, false, false);
  const { json } = renderAriaTreeAsJSON(snapshot, { mode: "default" });
  return {
    matches,
    received: {
      raw: renderAriaSnapshotAsYaml(json),
      regex: renderAriaSnapshotAsYaml(json, { convertStringsToRegex: true })
    }
  };
}
function getAllElementsMatchingExpectAriaTemplate(rootElement, template) {
  const root = generateAriaTree(rootElement, { mode: "default" }).root;
  const matches = matchesNodeDeep(root, template, true, false);
  return matches.map((n) => ariaNodeElement(n));
}
function matchesNode(node, template, isDeepEqual) {
  var _a;
  if (typeof node === "string" && template.kind === "text")
    return matchesTextValue(node, template.text);
  if (node === null || typeof node !== "object" || template.kind !== "role")
    return false;
  if (template.role !== "fragment" && template.role !== node.role)
    return false;
  if (template.checked !== void 0 && template.checked !== node.checked)
    return false;
  if (template.disabled !== void 0 && template.disabled !== node.disabled)
    return false;
  if (template.expanded !== void 0 && template.expanded !== node.expanded)
    return false;
  if (template.invalid !== void 0 && template.invalid !== node.invalid)
    return false;
  if (template.level !== void 0 && template.level !== node.level)
    return false;
  if (template.pressed !== void 0 && template.pressed !== node.pressed)
    return false;
  if (template.selected !== void 0 && template.selected !== node.selected)
    return false;
  if (!matchesStringOrRegex(node.name, template.name))
    return false;
  if (!matchesTextValue(node.props.url, (_a = template.props) == null ? void 0 : _a.url))
    return false;
  if (template.containerMode === "contain")
    return containsList(node.children || [], template.children || []);
  if (template.containerMode === "equal")
    return listEqual(node.children || [], template.children || [], false);
  if (template.containerMode === "deep-equal" || isDeepEqual)
    return listEqual(node.children || [], template.children || [], true);
  return containsList(node.children || [], template.children || []);
}
function listEqual(children, template, isDeepEqual) {
  if (template.length !== children.length)
    return false;
  for (let i = 0; i < template.length; ++i) {
    if (!matchesNode(children[i], template[i], isDeepEqual))
      return false;
  }
  return true;
}
function containsList(children, template) {
  if (template.length > children.length)
    return false;
  const cc = children.slice();
  const tt = template.slice();
  for (const t of tt) {
    let c = cc.shift();
    while (c) {
      if (matchesNode(c, t, false))
        break;
      c = cc.shift();
    }
    if (!c)
      return false;
  }
  return true;
}
function matchesNodeDeep(root, template, collectAll, isDeepEqual) {
  const results = [];
  const visit3 = (node, parent) => {
    if (matchesNode(node, template, isDeepEqual)) {
      const result = typeof node === "string" ? parent : node;
      if (result)
        results.push(result);
      return !collectAll;
    }
    if (typeof node === "string")
      return false;
    for (const child of node.children || []) {
      if (visit3(child, node))
        return true;
    }
    return false;
  };
  visit3(root, null);
  return results;
}
function renderAriaTreeAsJSON(ariaSnapshot, publicOptions) {
  const options = toInternalOptions(publicOptions);
  const iframeDepths = {};
  const visit3 = (ariaNode, depth, renderCursorPointer) => {
    if (ariaNode.role === "iframe" && ariaNode.ref)
      iframeDepths[ariaNode.ref] = depth;
    const node = { role: ariaNode.role };
    if (ariaNode.name)
      node.name = ariaNode.name;
    if (ariaNode.checked === "mixed" || ariaNode.checked === true)
      node.checked = ariaNode.checked;
    if (ariaNode.disabled)
      node.disabled = true;
    if (ariaNode.expanded)
      node.expanded = true;
    if (ariaNode.active && options.renderActive)
      node.active = true;
    if (ariaNode.invalid)
      node.invalid = ariaNode.invalid;
    if (ariaNode.level)
      node.level = ariaNode.level;
    if (ariaNode.pressed === "mixed" || ariaNode.pressed === true)
      node.pressed = ariaNode.pressed;
    if (ariaNode.selected === true)
      node.selected = true;
    if (ariaNode.ref) {
      node.ref = ariaNode.ref;
      if (renderCursorPointer && hasPointerCursor(ariaNode))
        node.cursor = "pointer";
    }
    if (options.renderBoxes) {
      const element = ariaNodeElement(ariaNode);
      if (element) {
        const r = element.getBoundingClientRect();
        node.box = { x: Math.round(r.x), y: Math.round(r.y), width: Math.round(r.width), height: Math.round(r.height) };
      }
    }
    if (ariaNode.props.url !== void 0)
      node.url = ariaNode.props.url;
    if (ariaNode.props.placeholder !== void 0)
      node.placeholder = ariaNode.props.placeholder;
    if (ariaNode.props["aria-hidden"] !== void 0)
      node.ariaHidden = true;
    const singleTextChild = ariaNode.children.length === 1 && typeof ariaNode.children[0] === "string" ? ariaNode.children[0] : void 0;
    const isAtDepthLimit = !!publicOptions.depth && depth === publicOptions.depth;
    if (singleTextChild !== void 0) {
      node.text = singleTextChild;
    } else if (!isAtDepthLimit && ariaNode.children.length) {
      const inCursorPointer = !!ariaNode.ref && renderCursorPointer && hasPointerCursor(ariaNode);
      node.children = ariaNode.children.map((child) => {
        if (typeof child === "string")
          return child;
        return visit3(child, depth + 1, renderCursorPointer && !inCursorPointer);
      });
    }
    return node;
  };
  const json = [];
  const nodesToRender = ariaSnapshot.root.role === "fragment" ? ariaSnapshot.root.children : [ariaSnapshot.root];
  for (const nodeToRender of nodesToRender) {
    if (typeof nodeToRender === "string")
      json.push({ role: "text", text: nodeToRender });
    else
      json.push(visit3(nodeToRender, 0, !!options.renderCursorPointer));
  }
  return { json, iframeDepths };
}
var elementSymbol = /* @__PURE__ */ Symbol("element");
function ariaNodeElement(ariaNode) {
  return ariaNode[elementSymbol];
}
function setAriaNodeElement(ariaNode, element) {
  ariaNode[elementSymbol] = element;
}
function findNewElement(from, to) {
  const node = findNewNode(from, to);
  return node ? ariaNodeElement(node) : void 0;
}

// packages/injected/src/highlight.css?inline
var highlight_default = ":host{font-size:13px;font-family:system-ui,Ubuntu,Droid Sans,sans-serif;color:#333;color-scheme:light}svg{position:absolute;height:0}x-pw-tooltip{backdrop-filter:blur(5px);background-color:#fff;border-radius:6px;box-shadow:0 .5rem 1.2rem #0000004d;display:none;font-size:12.8px;font-weight:400;left:0;line-height:1.5;max-width:600px;position:absolute;top:0;padding:0;flex-direction:column;overflow:hidden}x-pw-tooltip-line{display:flex;max-width:600px;padding:6px;user-select:none;cursor:pointer}x-pw-tooltip-footer{display:flex;max-width:600px;padding:6px;user-select:none;color:#777}x-pw-dialog{background-color:#fff;pointer-events:auto;border-radius:6px;box-shadow:0 .5rem 1.2rem #0000004d;display:flex;flex-direction:column;position:absolute;z-index:10;font-size:13px}x-pw-dialog:not(.autosize){width:400px;height:150px}x-pw-dialog-body{display:flex;flex-direction:column;flex:auto}x-pw-dialog-body label{margin:5px 8px;display:flex;flex-direction:row;align-items:center}x-pw-highlight{position:absolute;top:0;left:0;width:0;height:0}x-pw-action-point{position:absolute;width:20px;height:20px;background:red;border-radius:10px;margin:-10px 0 0 -10px;z-index:2}x-pw-screencast-highlight,x-pw-screencast-point,x-pw-title{animation:pw-fade-out var(--pw-fade-duration) ease-out var(--pw-fade-delay, 0ms) forwards}x-pw-screencast-highlight,x-pw-screencast-point{position:absolute;pointer-events:none}x-pw-screencast-highlight{z-index:1}x-pw-screencast-point{width:0;height:0;overflow:visible;translate:-50% -50%;z-index:2}x-pw-action-cursor{position:absolute;width:14px;height:21px;pointer-events:none;z-index:4;filter:drop-shadow(0 1px 1.5px rgba(0,0,0,.35))}x-pw-action-cursor svg{width:100%;height:100%;position:static;overflow:visible}x-pw-title{position:absolute;backdrop-filter:blur(5px);background-color:#00000080;color:#fff;border-radius:6px;padding:6px;font-size:24px;line-height:1.4;white-space:nowrap;user-select:none;z-index:3}x-pw-user-overlays,x-pw-user-overlay{position:absolute;inset:0}@keyframes pw-fade-out{0%{opacity:1}to{opacity:0}}x-pw-separator{height:1px;margin:6px 9px;background:#949494e5}x-pw-tool-gripper{height:28px;width:24px;margin:2px 0;cursor:grab}x-pw-tool-gripper:active{cursor:grabbing}x-pw-tool-gripper>x-div{width:16px;height:16px;margin:6px 4px;clip-path:url(#icon-gripper);background-color:#555}x-pw-tools-list>label{display:flex;align-items:center;margin:0 10px;user-select:none}x-pw-tools-list{display:flex;width:100%;border-bottom:1px solid #dddddd}x-pw-tool-item{pointer-events:auto;height:28px;width:28px;border-radius:3px}x-pw-tool-item:not(.disabled){cursor:pointer}x-pw-tool-item:not(.disabled):hover{background-color:#dbdbdb}x-pw-tool-item.toggled{background-color:#8acae480}x-pw-tool-item.toggled:not(.disabled):hover{background-color:#8acae4c4}x-pw-tool-item>x-div{width:16px;height:16px;margin:6px;background-color:#3a3a3a}x-pw-tool-item.disabled>x-div{background-color:#61616180;cursor:default}x-pw-tool-item.record.toggled{background-color:transparent}x-pw-tool-item.record.toggled:not(.disabled):hover{background-color:#dbdbdb}x-pw-tool-item.record.toggled>x-div{background-color:#a1260d}x-pw-tool-item.record.disabled.toggled>x-div{opacity:.8}x-pw-tool-item.accept>x-div{background-color:#388a34}x-pw-tool-item.record>x-div{clip-path:url(#icon-circle-large-filled)}x-pw-tool-item.record.toggled>x-div{clip-path:url(#icon-stop-circle)}x-pw-tool-item.pick-locator>x-div{clip-path:url(#icon-inspect)}x-pw-tool-item.text>x-div{clip-path:url(#icon-whole-word)}x-pw-tool-item.visibility>x-div{clip-path:url(#icon-eye)}x-pw-tool-item.value>x-div{clip-path:url(#icon-symbol-constant)}x-pw-tool-item.snapshot>x-div{clip-path:url(#icon-gist)}x-pw-tool-item.accept>x-div{clip-path:url(#icon-check)}x-pw-tool-item.cancel>x-div{clip-path:url(#icon-close)}x-pw-tool-item.succeeded>x-div{clip-path:url(#icon-pass);background-color:#388a34!important}x-pw-overlay{position:absolute;top:0;max-width:min-content;z-index:2147483647;background:transparent;pointer-events:auto}x-pw-overlay x-pw-tools-list{background-color:#fffd;box-shadow:#0000001a 0 5px 5px;border-radius:3px;border-bottom:none}x-pw-overlay x-pw-tool-item{margin:2px}textarea.text-editor{font-family:system-ui,Ubuntu,Droid Sans,sans-serif;flex:auto;border:none;margin:6px 10px;color:#333;outline:1px solid transparent!important;resize:none;padding:0;font-size:13px}textarea.text-editor.does-not-match{outline:1px solid red!important}x-div{display:block}x-spacer{flex:auto}*{box-sizing:border-box}*[hidden]{display:none!important}x-locator-editor{flex:none;width:100%;height:60px;padding:4px;border-bottom:1px solid #dddddd;outline:1px solid transparent}x-locator-editor.does-not-match{outline:1px solid red}.CodeMirror{width:100%!important;height:100%!important}x-pw-action-list{flex:auto;display:flex;flex-direction:column;user-select:none}x-pw-action-item{padding:6px 10px;cursor:pointer;overflow:hidden}x-pw-action-item:hover{background-color:#f2f2f2}x-pw-action-item:last-child{border-bottom-left-radius:6px;border-bottom-right-radius:6px}\n";

// packages/injected/src/highlight.ts
var Highlight = class {
  constructor(injectedScript) {
    this._renderedEntries = [];
    this._screencastDecorations = [];
    this._userOverlays = /* @__PURE__ */ new Map();
    this._userOverlayHidden = false;
    this._language = "javascript";
    this._elementHighlights = [];
    this._injectedScript = injectedScript;
    const document = injectedScript.document;
    this._isUnderTest = injectedScript.isUnderTest;
    this._glassPaneElement = document.createElement("x-pw-glass");
    this._glassPaneElement.setAttribute("popover", "manual");
    this._glassPaneElement.style.inset = "0";
    this._glassPaneElement.style.width = "100%";
    this._glassPaneElement.style.height = "100%";
    this._glassPaneElement.style.maxWidth = "none";
    this._glassPaneElement.style.maxHeight = "none";
    this._glassPaneElement.style.padding = "0";
    this._glassPaneElement.style.margin = "0";
    this._glassPaneElement.style.border = "none";
    this._glassPaneElement.style.overflow = "visible";
    this._glassPaneElement.style.pointerEvents = "none";
    this._glassPaneElement.style.display = "flex";
    this._glassPaneElement.style.backgroundColor = "transparent";
    this._actionCursorElement = document.createElement("x-pw-action-cursor");
    this._actionCursorElement.style.visibility = "hidden";
    this._actionCursorElement.appendChild(this._createCursorSvg(document));
    this._titleElement = document.createElement("x-pw-title");
    this._titleElement.setAttribute("hidden", "true");
    this._userOverlayContainer = document.createElement("x-pw-user-overlays");
    this._userOverlayContainer.setAttribute("hidden", "true");
    this._glassPaneShadow = this._glassPaneElement.attachShadow({ mode: this._isUnderTest ? "open" : "closed" });
    if (typeof this._glassPaneShadow.adoptedStyleSheets.push === "function") {
      const sheet = new this._injectedScript.window.CSSStyleSheet();
      sheet.replaceSync(highlight_default);
      this._glassPaneShadow.adoptedStyleSheets.push(sheet);
    } else {
      const styleElement = this._injectedScript.document.createElement("style");
      styleElement.textContent = highlight_default;
      this._glassPaneShadow.appendChild(styleElement);
    }
    this._glassPaneShadow.appendChild(this._actionCursorElement);
    this._glassPaneShadow.appendChild(this._titleElement);
    this._glassPaneShadow.appendChild(this._userOverlayContainer);
  }
  install() {
    if (!this._injectedScript.document.documentElement)
      return;
    if (!this._injectedScript.document.documentElement.contains(this._glassPaneElement) || this._glassPaneElement.nextElementSibling)
      this._injectedScript.document.documentElement.appendChild(this._glassPaneElement);
    this._bringToFront();
  }
  _bringToFront() {
    this._glassPaneElement.hidePopover();
    this._glassPaneElement.showPopover();
  }
  setLanguage(language) {
    this._language = language;
  }
  setElementHighlights(highlights) {
    const hadHighlights = this._elementHighlights.length > 0;
    this._elementHighlights = highlights;
    if (this._elementHighlights.length) {
      this._ensureElementHighlightRaf();
    } else if (hadHighlights) {
      if (this._rafRequest) {
        this._injectedScript.utils.builtins.cancelAnimationFrame(this._rafRequest);
        this._rafRequest = void 0;
      }
      this.clearHighlight();
    }
  }
  _ensureElementHighlightRaf() {
    if (this._rafRequest)
      return;
    const tick = () => {
      const entries = [];
      const glassPanes = [...this._injectedScript.document.querySelectorAll("x-pw-glass")];
      for (const { selector, cssStyle } of this._elementHighlights) {
        let elements = [];
        try {
          elements = this._injectedScript.querySelectorAll(selector, this._injectedScript.document.documentElement);
        } catch {
        }
        elements = elements.filter((element) => !glassPanes.some((pane) => this._injectedScript.utils.isInsideScope(pane, element)));
        const locator = selector.parts.some((part) => part.name === "aria-template") ? void 0 : asLocator(this._language, stringifySelector(selector));
        const color = elements.length > 1 ? "#f6b26b7f" : "#6fa8dc7f";
        for (let i = 0; i < elements.length; ++i) {
          const suffix = elements.length > 1 ? ` [${i + 1} of ${elements.length}]` : "";
          entries.push({ element: elements[i], color, tooltipText: locator === void 0 ? void 0 : locator + suffix, cssStyle });
        }
      }
      this.updateHighlight(entries);
      this._rafRequest = this._injectedScript.utils.builtins.requestAnimationFrame(tick);
    };
    this._rafRequest = this._injectedScript.utils.builtins.requestAnimationFrame(tick);
  }
  uninstall() {
    if (this._rafRequest) {
      this._injectedScript.utils.builtins.cancelAnimationFrame(this._rafRequest);
      this._rafRequest = void 0;
    }
    this._elementHighlights = [];
    this._clearUserOverlayTimer();
    this._glassPaneElement.remove();
  }
  showActionPoint(x, y) {
    if (!this._actionPointElement) {
      this._actionPointElement = this._injectedScript.document.createElement("x-pw-action-point");
      this._glassPaneShadow.appendChild(this._actionPointElement);
    }
    this._actionPointElement.style.top = y + "px";
    this._actionPointElement.style.left = x + "px";
    this._actionPointElement.hidden = false;
  }
  hideActionPoint() {
    if (this._actionPointElement)
      this._actionPointElement.hidden = true;
  }
  showScreencastHighlight(box, style, fadeDuration) {
    const element = this._createScreencastDecoration("x-pw-screencast-highlight", style, fadeDuration);
    element.style.left = box.x + "px";
    element.style.top = box.y + "px";
    element.style.width = box.width + "px";
    element.style.height = box.height + "px";
  }
  showScreencastPoint(x, y, style, fadeDuration) {
    const element = this._createScreencastDecoration("x-pw-screencast-point", style, fadeDuration);
    element.style.left = x + "px";
    element.style.top = y + "px";
  }
  hideScreencastDecorations() {
    for (const element of this._screencastDecorations)
      element.remove();
    this._screencastDecorations = [];
  }
  _createScreencastDecoration(name, style, fadeDuration) {
    const element = this._injectedScript.document.createElement(name);
    element.style.cssText = style;
    if (fadeDuration)
      element.style.setProperty("--pw-fade-duration", fadeDuration + "ms");
    this._glassPaneShadow.appendChild(element);
    this._screencastDecorations.push(element);
    return element;
  }
  moveActionCursor(x, y, fadeDuration) {
    var _a;
    const element = this._actionCursorElement;
    const from = element.getBoundingClientRect();
    const wasVisible = element.style.visibility === "visible";
    (_a = this._actionCursorAnimation) == null ? void 0 : _a.cancel();
    this._actionCursorAnimation = void 0;
    element.style.left = x + "px";
    element.style.top = y + "px";
    element.style.visibility = "visible";
    const dx = from.left - x;
    const dy = from.top - y;
    if (!wasVisible || !fadeDuration || Math.hypot(dx, dy) < 1)
      return;
    const duration = Math.max(80, Math.min(fadeDuration * 0.6, 400));
    this._actionCursorAnimation = element.animate(cursorPathKeyframes(dx, dy, x + y + dx + dy), { duration, easing: "cubic-bezier(0.5, 0, 0.3, 1)" });
  }
  restoreActionCursor(x, y) {
    if (this._actionCursorElement.style.visibility !== "visible")
      this.moveActionCursor(x, y);
  }
  hideActionCursor() {
    this._actionCursorElement.style.visibility = "hidden";
  }
  _createCursorSvg(document) {
    const svgNs = "http://www.w3.org/2000/svg";
    const svg = document.createElementNS(svgNs, "svg");
    svg.setAttribute("viewBox", "0 0 12 18");
    const path = document.createElementNS(svgNs, "path");
    path.setAttribute("d", "M0 0 L0 15.8 L3.9 12.4 L6.6 18.6 L8.9 17.6 L6.2 11.4 L11.2 11.4 Z");
    path.setAttribute("fill", "black");
    path.setAttribute("stroke", "white");
    path.setAttribute("stroke-width", "1.6");
    path.setAttribute("stroke-linejoin", "round");
    path.setAttribute("paint-order", "stroke");
    svg.appendChild(path);
    return svg;
  }
  showActionTitle(text, fadeDuration, position, style) {
    this._titleElement.textContent = text;
    this._titleElement.hidden = false;
    this._titleElement.style.cssText = style != null ? style : "";
    if (fadeDuration) {
      const fadeTime = fadeDuration / 4;
      this._titleElement.style.setProperty("--pw-fade-duration", fadeTime + "ms");
      this._titleElement.style.setProperty("--pw-fade-delay", fadeDuration - fadeTime + "ms");
    }
    switch (position) {
      case "top-left":
        this._titleElement.style.top = "6px";
        this._titleElement.style.left = "6px";
        break;
      case "top":
        this._titleElement.style.top = "6px";
        this._titleElement.style.left = "50%";
        this._titleElement.style.transform = "translateX(-50%)";
        break;
      case "bottom-left":
        this._titleElement.style.bottom = "6px";
        this._titleElement.style.left = "6px";
        break;
      case "bottom":
        this._titleElement.style.bottom = "6px";
        this._titleElement.style.left = "50%";
        this._titleElement.style.transform = "translateX(-50%)";
        break;
      case "bottom-right":
        this._titleElement.style.bottom = "6px";
        this._titleElement.style.right = "6px";
        break;
      case "top-right":
      default:
        this._titleElement.style.top = "6px";
        this._titleElement.style.right = "6px";
        break;
    }
  }
  hideActionTitle() {
    this._titleElement.hidden = true;
  }
  addUserOverlay(id, html) {
    const element = this._injectedScript.document.createElement("div");
    element.className = "x-pw-user-overlay";
    element.innerHTML = html;
    for (const script of element.querySelectorAll("script"))
      script.remove();
    for (const el of element.querySelectorAll("*")) {
      for (const attr of [...el.attributes]) {
        if (attr.name.startsWith("on"))
          el.removeAttribute(attr.name);
      }
    }
    this._userOverlays.set(id, element);
    this._userOverlayContainer.appendChild(element);
    this._userOverlayContainer.hidden = this._userOverlayHidden;
    this._ensureUserOverlayTimer();
    return id;
  }
  getUserOverlay(id) {
    return this._userOverlays.get(id);
  }
  removeUserOverlay(id) {
    const element = this._userOverlays.get(id);
    if (element) {
      element.remove();
      this._userOverlays.delete(id);
    }
    if (this._userOverlays.size === 0) {
      this._userOverlayContainer.hidden = true;
      this._clearUserOverlayTimer();
    }
  }
  // Elements that enter the top layer later paint above the glass pane, so keep re-promoting it while overlays are showing.
  _ensureUserOverlayTimer() {
    if (this._userOverlayTimer !== void 0)
      return;
    const tick = () => {
      this.install();
      this._userOverlayTimer = this._injectedScript.utils.builtins.setTimeout(tick, 500);
    };
    this._userOverlayTimer = this._injectedScript.utils.builtins.setTimeout(tick, 500);
  }
  _clearUserOverlayTimer() {
    if (this._userOverlayTimer === void 0)
      return;
    this._injectedScript.utils.builtins.clearTimeout(this._userOverlayTimer);
    this._userOverlayTimer = void 0;
  }
  setUserOverlaysVisible(visible) {
    this._userOverlayHidden = !visible;
    this._userOverlayContainer.hidden = !visible || this._userOverlays.size === 0;
  }
  clearHighlight() {
    var _a, _b;
    for (const entry of this._renderedEntries) {
      (_a = entry.highlightElement) == null ? void 0 : _a.remove();
      (_b = entry.tooltipElement) == null ? void 0 : _b.remove();
    }
    this._renderedEntries = [];
  }
  addMaskedElements(elements, color) {
    const existingEntries = this._renderedEntries.map((e) => ({ element: e.targetElement, color: e.color }));
    const newEntries = elements.map((element) => ({ element, color }));
    this.updateHighlight([...existingEntries, ...newEntries]);
  }
  updateHighlight(entries) {
    if (this._highlightIsUpToDate(entries))
      return;
    this.clearHighlight();
    for (const entry of entries) {
      const highlightElement = this._createHighlightElement();
      this._glassPaneShadow.appendChild(highlightElement);
      let tooltipElement;
      if (entry.tooltipText) {
        tooltipElement = this._injectedScript.document.createElement("x-pw-tooltip");
        this._glassPaneShadow.appendChild(tooltipElement);
        tooltipElement.style.top = "0";
        tooltipElement.style.left = "0";
        tooltipElement.style.display = "flex";
        const lineElement = this._injectedScript.document.createElement("x-pw-tooltip-line");
        lineElement.textContent = entry.tooltipText;
        tooltipElement.appendChild(lineElement);
      }
      this._renderedEntries.push({ targetElement: entry.element, box: toDOMRect(entry.box), color: entry.color, cssStyle: entry.cssStyle, tooltipElement, highlightElement });
    }
    for (const entry of this._renderedEntries) {
      if (!entry.box && !entry.targetElement)
        continue;
      entry.box = entry.box || entry.targetElement.getBoundingClientRect();
      if (!entry.tooltipElement)
        continue;
      const { anchorLeft, anchorTop } = this.tooltipPosition(entry.box, entry.tooltipElement);
      entry.tooltipTop = anchorTop;
      entry.tooltipLeft = anchorLeft;
    }
    for (const entry of this._renderedEntries) {
      if (entry.tooltipElement) {
        entry.tooltipElement.style.top = entry.tooltipTop + "px";
        entry.tooltipElement.style.left = entry.tooltipLeft + "px";
      }
      const box = entry.box;
      entry.highlightElement.style.backgroundColor = entry.color;
      entry.highlightElement.style.left = box.x + "px";
      entry.highlightElement.style.top = box.y + "px";
      entry.highlightElement.style.width = box.width + "px";
      entry.highlightElement.style.height = box.height + "px";
      entry.highlightElement.style.display = "block";
      if (entry.cssStyle)
        entry.highlightElement.style.cssText += ";" + entry.cssStyle;
      if (this._isUnderTest)
        console.error("Highlight box for test: " + JSON.stringify({ x: box.x, y: box.y, width: box.width, height: box.height }));
    }
  }
  firstBox() {
    var _a;
    return (_a = this._renderedEntries[0]) == null ? void 0 : _a.box;
  }
  firstTooltipBox() {
    const entry = this._renderedEntries[0];
    if (!entry || !entry.tooltipElement || entry.tooltipLeft === void 0 || entry.tooltipTop === void 0)
      return;
    return {
      x: entry.tooltipLeft,
      y: entry.tooltipTop,
      left: entry.tooltipLeft,
      top: entry.tooltipTop,
      width: entry.tooltipElement.offsetWidth,
      height: entry.tooltipElement.offsetHeight,
      bottom: entry.tooltipTop + entry.tooltipElement.offsetHeight,
      right: entry.tooltipLeft + entry.tooltipElement.offsetWidth,
      toJSON: () => {
      }
    };
  }
  // Note: there is a copy of this method in dialog.tsx. Please fix bugs in both places.
  tooltipPosition(box, tooltipElement) {
    const tooltipWidth = tooltipElement.offsetWidth;
    const tooltipHeight = tooltipElement.offsetHeight;
    const totalWidth = this._glassPaneElement.offsetWidth;
    const totalHeight = this._glassPaneElement.offsetHeight;
    let anchorLeft = Math.max(5, box.left);
    if (anchorLeft + tooltipWidth > totalWidth - 5)
      anchorLeft = totalWidth - tooltipWidth - 5;
    let anchorTop = Math.max(0, box.bottom) + 5;
    if (anchorTop + tooltipHeight > totalHeight - 5) {
      if (Math.max(0, box.top) > tooltipHeight + 5) {
        anchorTop = Math.max(0, box.top) - tooltipHeight - 5;
      } else {
        anchorTop = totalHeight - 5 - tooltipHeight;
      }
    }
    return { anchorLeft, anchorTop };
  }
  _highlightIsUpToDate(entries) {
    if (entries.length !== this._renderedEntries.length)
      return false;
    for (let i = 0; i < this._renderedEntries.length; ++i) {
      if (entries[i].element !== this._renderedEntries[i].targetElement)
        return false;
      if (entries[i].color !== this._renderedEntries[i].color)
        return false;
      if (entries[i].cssStyle !== this._renderedEntries[i].cssStyle)
        return false;
      const oldBox = this._renderedEntries[i].box;
      if (!oldBox)
        return false;
      const box = entries[i].box ? toDOMRect(entries[i].box) : entries[i].element.getBoundingClientRect();
      if (box.top !== oldBox.top || box.right !== oldBox.right || box.bottom !== oldBox.bottom || box.left !== oldBox.left)
        return false;
    }
    return true;
  }
  _createHighlightElement() {
    return this._injectedScript.document.createElement("x-pw-highlight");
  }
  appendChild(element) {
    this._glassPaneShadow.appendChild(element);
  }
  onGlassPaneClick(handler) {
    this._glassPaneElement.style.pointerEvents = "auto";
    this._glassPaneElement.style.backgroundColor = "rgba(0, 0, 0, 0.3)";
    this._glassPaneElement.addEventListener("click", handler);
  }
  offGlassPaneClick(handler) {
    this._glassPaneElement.style.pointerEvents = "none";
    this._glassPaneElement.style.backgroundColor = "transparent";
    this._glassPaneElement.removeEventListener("click", handler);
  }
};
function cursorPathKeyframes(dx, dy, seed) {
  const random = (salt) => {
    const value = Math.sin(seed * 12.9898 + salt * 78.233) * 43758.5453;
    return value - Math.floor(value);
  };
  const distance = Math.hypot(dx, dy);
  const normalX = -dy / distance;
  const normalY = dx / distance;
  const side = random(1) < 0.5 ? -1 : 1;
  const startBow = side * distance * (0.03 + 0.03 * random(2));
  const endBow = side * distance * (0.01 + 0.02 * random(3));
  const c1x = dx * 2 / 3 + normalX * startBow;
  const c1y = dy * 2 / 3 + normalY * startBow;
  const c2x = dx / 3 + normalX * endBow;
  const c2y = dy / 3 + normalY * endBow;
  const keyframes = [];
  const steps = 30;
  for (let i = 0; i <= steps; i++) {
    const t = i / steps;
    const u = 1 - t;
    const px = u * u * u * dx + 3 * u * u * t * c1x + 3 * u * t * t * c2x;
    const py = u * u * u * dy + 3 * u * u * t * c1y + 3 * u * t * t * c2y;
    keyframes.push({ transform: `translate(${px}px, ${py}px)` });
  }
  return keyframes;
}
function toDOMRect(box) {
  if (!box)
    return void 0;
  return new DOMRect(box.x, box.y, box.width, box.height);
}

// packages/injected/src/layoutSelectorUtils.ts
function boxRightOf(box1, box2, maxDistance) {
  const distance = box1.left - box2.right;
  if (distance < 0 || maxDistance !== void 0 && distance > maxDistance)
    return;
  return distance + Math.max(box2.bottom - box1.bottom, 0) + Math.max(box1.top - box2.top, 0);
}
function boxLeftOf(box1, box2, maxDistance) {
  const distance = box2.left - box1.right;
  if (distance < 0 || maxDistance !== void 0 && distance > maxDistance)
    return;
  return distance + Math.max(box2.bottom - box1.bottom, 0) + Math.max(box1.top - box2.top, 0);
}
function boxAbove(box1, box2, maxDistance) {
  const distance = box2.top - box1.bottom;
  if (distance < 0 || maxDistance !== void 0 && distance > maxDistance)
    return;
  return distance + Math.max(box1.left - box2.left, 0) + Math.max(box2.right - box1.right, 0);
}
function boxBelow(box1, box2, maxDistance) {
  const distance = box1.top - box2.bottom;
  if (distance < 0 || maxDistance !== void 0 && distance > maxDistance)
    return;
  return distance + Math.max(box1.left - box2.left, 0) + Math.max(box2.right - box1.right, 0);
}
function boxNear(box1, box2, maxDistance) {
  const kThreshold = maxDistance === void 0 ? 50 : maxDistance;
  let score = 0;
  if (box1.left - box2.right >= 0)
    score += box1.left - box2.right;
  if (box2.left - box1.right >= 0)
    score += box2.left - box1.right;
  if (box2.top - box1.bottom >= 0)
    score += box2.top - box1.bottom;
  if (box1.top - box2.bottom >= 0)
    score += box1.top - box2.bottom;
  return score > kThreshold ? void 0 : score;
}
var kLayoutSelectorNames = ["left-of", "right-of", "above", "below", "near"];
function layoutSelectorScore(name, element, inner, maxDistance) {
  const box = element.getBoundingClientRect();
  const scorer = { "left-of": boxLeftOf, "right-of": boxRightOf, "above": boxAbove, "below": boxBelow, "near": boxNear }[name];
  let bestScore;
  for (const e of inner) {
    if (e === element)
      continue;
    const score = scorer(box, e.getBoundingClientRect(), maxDistance);
    if (score === void 0)
      continue;
    if (bestScore === void 0 || score < bestScore)
      bestScore = score;
  }
  return bestScore;
}

// packages/injected/src/selectorUtils.ts
function matchesAttributePart(value, attr) {
  const objValue = typeof value === "string" && !attr.caseSensitive ? value.toUpperCase() : value;
  const attrValue = typeof attr.value === "string" && !attr.caseSensitive ? attr.value.toUpperCase() : attr.value;
  if (attr.op === "<truthy>")
    return !!objValue;
  if (attr.op === "=") {
    if (attrValue instanceof RegExp) {
      attrValue.lastIndex = 0;
      return typeof objValue === "string" && !!objValue.match(attrValue);
    }
    return objValue === attrValue;
  }
  if (typeof objValue !== "string" || typeof attrValue !== "string")
    return false;
  if (attr.op === "*=")
    return objValue.includes(attrValue);
  if (attr.op === "^=")
    return objValue.startsWith(attrValue);
  if (attr.op === "$=")
    return objValue.endsWith(attrValue);
  if (attr.op === "|=")
    return objValue === attrValue || objValue.startsWith(attrValue + "-");
  if (attr.op === "~=")
    return objValue.split(" ").includes(attrValue);
  return false;
}
function shouldSkipForTextMatching(element) {
  const document = element.ownerDocument;
  return element.nodeName === "SCRIPT" || element.nodeName === "NOSCRIPT" || element.nodeName === "STYLE" || document.head && document.head.contains(element);
}
function elementText(cache, root) {
  let value = cache.get(root);
  if (value === void 0) {
    value = { full: "", normalized: "", immediate: [] };
    if (!shouldSkipForTextMatching(root)) {
      let currentImmediate = "";
      if (root instanceof HTMLInputElement && (root.type === "submit" || root.type === "button" || root.type === "reset")) {
        value = { full: root.value, normalized: normalizeWhiteSpace(root.value), immediate: [root.value] };
      } else {
        for (let child = root.firstChild; child; child = child.nextSibling) {
          if (child.nodeType === Node.TEXT_NODE) {
            value.full += child.nodeValue || "";
            currentImmediate += child.nodeValue || "";
          } else if (child.nodeType === Node.COMMENT_NODE) {
            continue;
          } else {
            if (currentImmediate)
              value.immediate.push(currentImmediate);
            currentImmediate = "";
            if (child.nodeType === Node.ELEMENT_NODE)
              value.full += elementText(cache, child).full;
          }
        }
        if (currentImmediate)
          value.immediate.push(currentImmediate);
        if (root.shadowRoot)
          value.full += elementText(cache, root.shadowRoot).full;
        if (value.full)
          value.normalized = normalizeWhiteSpace(value.full);
      }
    }
    cache.set(root, value);
  }
  return value;
}
function elementMatchesText(cache, element, matcher) {
  if (shouldSkipForTextMatching(element))
    return "none";
  if (!matcher(elementText(cache, element)))
    return "none";
  for (let child = element.firstChild; child; child = child.nextSibling) {
    if (child.nodeType === Node.ELEMENT_NODE && matcher(elementText(cache, child)))
      return "selfAndChildren";
  }
  if (element.shadowRoot && matcher(elementText(cache, element.shadowRoot)))
    return "selfAndChildren";
  return "self";
}
function getElementLabels(textCache, element, options) {
  let labels = getAriaLabelledByElements(element);
  if (labels) {
    if (options == null ? void 0 : options.skipRefsInsideElement)
      labels = labels.filter((label) => label !== element && !element.contains(label));
    return labels.map((label) => elementText(textCache, label));
  }
  const ariaLabel = element.getAttribute("aria-label");
  if (ariaLabel !== null && !!ariaLabel.trim())
    return [{ full: ariaLabel, normalized: normalizeWhiteSpace(ariaLabel), immediate: [ariaLabel] }];
  const isNonHiddenInput = element.nodeName === "INPUT" && element.type !== "hidden";
  if (["BUTTON", "METER", "OUTPUT", "PROGRESS", "SELECT", "TEXTAREA"].includes(element.nodeName) || isNonHiddenInput) {
    const labels2 = element.labels;
    if (labels2)
      return [...labels2].map((label) => elementText(textCache, label));
  }
  return [];
}

// packages/injected/src/roleSelectorEngine.ts
var kSupportedAttributes = ["selected", "checked", "pressed", "expanded", "level", "disabled", "name", "description", "include-hidden"];
kSupportedAttributes.sort();
function validateSupportedRole(attr, roles, role) {
  if (!roles.includes(role))
    throw new Error(`"${attr}" attribute is only supported for roles: ${roles.slice().sort().map((role2) => `"${role2}"`).join(", ")}`);
}
function validateSupportedValues(attr, values) {
  if (attr.op !== "<truthy>" && !values.includes(attr.value))
    throw new Error(`"${attr.name}" must be one of ${values.map((v) => JSON.stringify(v)).join(", ")}`);
}
function validateSupportedOp(attr, ops) {
  if (!ops.includes(attr.op))
    throw new Error(`"${attr.name}" does not support "${attr.op}" matcher`);
}
function validateAttributes(attrs, role) {
  const options = { role };
  for (const attr of attrs) {
    switch (attr.name) {
      case "checked": {
        validateSupportedRole(attr.name, kAriaCheckedRoles, role);
        validateSupportedValues(attr, [true, false, "mixed"]);
        validateSupportedOp(attr, ["<truthy>", "="]);
        options.checked = attr.op === "<truthy>" ? true : attr.value;
        break;
      }
      case "pressed": {
        validateSupportedRole(attr.name, kAriaPressedRoles, role);
        validateSupportedValues(attr, [true, false, "mixed"]);
        validateSupportedOp(attr, ["<truthy>", "="]);
        options.pressed = attr.op === "<truthy>" ? true : attr.value;
        break;
      }
      case "selected": {
        validateSupportedRole(attr.name, kAriaSelectedRoles, role);
        validateSupportedValues(attr, [true, false]);
        validateSupportedOp(attr, ["<truthy>", "="]);
        options.selected = attr.op === "<truthy>" ? true : attr.value;
        break;
      }
      case "expanded": {
        validateSupportedRole(attr.name, kAriaExpandedRoles, role);
        validateSupportedValues(attr, [true, false]);
        validateSupportedOp(attr, ["<truthy>", "="]);
        options.expanded = attr.op === "<truthy>" ? true : attr.value;
        break;
      }
      case "level": {
        validateSupportedRole(attr.name, kAriaLevelRoles, role);
        if (typeof attr.value === "string")
          attr.value = +attr.value;
        if (attr.op !== "=" || typeof attr.value !== "number" || Number.isNaN(attr.value))
          throw new Error(`"level" attribute must be compared to a number`);
        options.level = attr.value;
        break;
      }
      case "disabled": {
        validateSupportedValues(attr, [true, false]);
        validateSupportedOp(attr, ["<truthy>", "="]);
        options.disabled = attr.op === "<truthy>" ? true : attr.value;
        break;
      }
      case "name": {
        if (attr.op === "<truthy>")
          throw new Error(`"name" attribute must have a value`);
        if (typeof attr.value !== "string" && !(attr.value instanceof RegExp))
          throw new Error(`"name" attribute must be a string or a regular expression`);
        options.name = attr.value;
        options.nameOp = attr.op;
        options.nameExact = attr.caseSensitive;
        break;
      }
      case "description": {
        if (attr.op === "<truthy>")
          throw new Error(`"description" attribute must have a value`);
        if (typeof attr.value !== "string" && !(attr.value instanceof RegExp))
          throw new Error(`"description" attribute must be a string or a regular expression`);
        options.description = attr.value;
        options.descriptionOp = attr.op;
        options.descriptionExact = attr.caseSensitive;
        break;
      }
      case "include-hidden": {
        validateSupportedValues(attr, [true, false]);
        validateSupportedOp(attr, ["<truthy>", "="]);
        options.includeHidden = attr.op === "<truthy>" ? true : attr.value;
        break;
      }
      default: {
        throw new Error(`Unknown attribute "${attr.name}", must be one of ${kSupportedAttributes.map((a) => `"${a}"`).join(", ")}.`);
      }
    }
  }
  return options;
}
function queryRole(scope, options, internal) {
  const result = [];
  const match = (element) => {
    if (getAriaRole(element) !== options.role)
      return;
    if (options.selected !== void 0 && getAriaSelected(element) !== options.selected)
      return;
    if (options.checked !== void 0 && getAriaChecked(element) !== options.checked)
      return;
    if (options.pressed !== void 0 && getAriaPressed(element) !== options.pressed)
      return;
    if (options.expanded !== void 0 && getAriaExpanded(element) !== options.expanded)
      return;
    if (options.level !== void 0 && getAriaLevel(element) !== options.level)
      return;
    if (options.disabled !== void 0 && getAriaDisabled(element) !== options.disabled)
      return;
    if (!options.includeHidden) {
      const isHidden = isElementHiddenForAria(element);
      if (isHidden)
        return;
    }
    if (options.name !== void 0) {
      const accessibleName = normalizeWhiteSpace(getElementAccessibleNameText(element, !!options.includeHidden));
      if (typeof options.name === "string")
        options.name = normalizeWhiteSpace(options.name);
      if (internal && !options.nameExact && options.nameOp === "=")
        options.nameOp = "*=";
      if (!matchesAttributePart(accessibleName, { name: "", jsonPath: [], op: options.nameOp || "=", value: options.name, caseSensitive: !!options.nameExact }))
        return;
    }
    if (options.description !== void 0) {
      const accessibleDescription = normalizeWhiteSpace(getElementAccessibleDescription(element, !!options.includeHidden).text);
      if (typeof options.description === "string")
        options.description = normalizeWhiteSpace(options.description);
      if (internal && !options.descriptionExact && options.descriptionOp === "=")
        options.descriptionOp = "*=";
      if (!matchesAttributePart(accessibleDescription, { name: "", jsonPath: [], op: options.descriptionOp || "=", value: options.description, caseSensitive: !!options.descriptionExact }))
        return;
    }
    result.push(element);
  };
  const query = (root) => {
    const shadows = [];
    if (root.shadowRoot)
      shadows.push(root.shadowRoot);
    for (const element of root.querySelectorAll("*")) {
      match(element);
      if (element.shadowRoot)
        shadows.push(element.shadowRoot);
    }
    shadows.forEach(query);
  };
  query(scope);
  return result;
}
function createRoleEngine(internal) {
  return {
    queryAll: (scope, selector) => {
      const parsed = parseAttributeSelector(selector, true);
      const role = parsed.name.toLowerCase();
      if (!role)
        throw new Error(`Role must not be empty`);
      const options = validateAttributes(parsed.attributes, role);
      beginAriaCaches();
      try {
        return queryRole(scope, options, internal);
      } finally {
        endAriaCaches();
      }
    }
  };
}

// packages/injected/src/selectorEvaluator.ts
var SelectorEvaluatorImpl = class {
  constructor() {
    this._retainCacheCounter = 0;
    this._cacheText = /* @__PURE__ */ new Map();
    this._cacheQueryCSS = /* @__PURE__ */ new Map();
    this._cacheMatches = /* @__PURE__ */ new Map();
    this._cacheQuery = /* @__PURE__ */ new Map();
    this._cacheMatchesSimple = /* @__PURE__ */ new Map();
    this._cacheMatchesParents = /* @__PURE__ */ new Map();
    this._cacheCallMatches = /* @__PURE__ */ new Map();
    this._cacheCallQuery = /* @__PURE__ */ new Map();
    this._cacheQuerySimple = /* @__PURE__ */ new Map();
    this._engines = /* @__PURE__ */ new Map();
    this._engines.set("not", notEngine);
    this._engines.set("is", isEngine);
    this._engines.set("where", isEngine);
    this._engines.set("has", hasEngine);
    this._engines.set("scope", scopeEngine);
    this._engines.set("light", lightEngine);
    this._engines.set("visible", visibleEngine);
    this._engines.set("text", textEngine);
    this._engines.set("text-is", textIsEngine);
    this._engines.set("text-matches", textMatchesEngine);
    this._engines.set("has-text", hasTextEngine);
    this._engines.set("right-of", createLayoutEngine("right-of"));
    this._engines.set("left-of", createLayoutEngine("left-of"));
    this._engines.set("above", createLayoutEngine("above"));
    this._engines.set("below", createLayoutEngine("below"));
    this._engines.set("near", createLayoutEngine("near"));
    this._engines.set("nth-match", nthMatchEngine);
    const allNames = [...this._engines.keys()];
    allNames.sort();
    const parserNames = [...customCSSNames];
    parserNames.sort();
    if (allNames.join("|") !== parserNames.join("|"))
      throw new Error(`Please keep customCSSNames in sync with evaluator engines: ${allNames.join("|")} vs ${parserNames.join("|")}`);
  }
  begin() {
    ++this._retainCacheCounter;
  }
  end() {
    --this._retainCacheCounter;
    if (!this._retainCacheCounter) {
      this._cacheQueryCSS.clear();
      this._cacheMatches.clear();
      this._cacheQuery.clear();
      this._cacheMatchesSimple.clear();
      this._cacheMatchesParents.clear();
      this._cacheCallMatches.clear();
      this._cacheCallQuery.clear();
      this._cacheQuerySimple.clear();
      this._cacheText.clear();
    }
  }
  _cached(cache, main, rest, cb) {
    if (!cache.has(main))
      cache.set(main, []);
    const entries = cache.get(main);
    const entry = entries.find((e) => rest.every((value, index) => e.rest[index] === value));
    if (entry)
      return entry.result;
    const result = cb();
    entries.push({ rest, result });
    return result;
  }
  _checkSelector(s) {
    const wellFormed = typeof s === "object" && s && (Array.isArray(s) || "simples" in s && s.simples.length);
    if (!wellFormed)
      throw new Error(`Malformed selector "${s}"`);
    return s;
  }
  matches(element, s, context) {
    const selector = this._checkSelector(s);
    this.begin();
    try {
      return this._cached(this._cacheMatches, element, [selector, context.scope, context.pierceShadow, context.originalScope], () => {
        if (Array.isArray(selector))
          return this._matchesEngine(isEngine, element, selector, context);
        if (this._hasScopeClause(selector))
          context = this._expandContextForScopeMatching(context);
        if (!this._matchesSimple(element, selector.simples[selector.simples.length - 1].selector, context))
          return false;
        return this._matchesParents(element, selector, selector.simples.length - 2, context);
      });
    } finally {
      this.end();
    }
  }
  query(context, s) {
    const selector = this._checkSelector(s);
    this.begin();
    try {
      return this._cached(this._cacheQuery, selector, [context.scope, context.pierceShadow, context.originalScope], () => {
        if (Array.isArray(selector))
          return this._queryEngine(isEngine, context, selector);
        if (this._hasScopeClause(selector))
          context = this._expandContextForScopeMatching(context);
        const previousScoreMap = this._scoreMap;
        this._scoreMap = /* @__PURE__ */ new Map();
        let elements = this._querySimple(context, selector.simples[selector.simples.length - 1].selector);
        elements = elements.filter((element) => this._matchesParents(element, selector, selector.simples.length - 2, context));
        if (this._scoreMap.size) {
          elements.sort((a, b) => {
            const aScore = this._scoreMap.get(a);
            const bScore = this._scoreMap.get(b);
            if (aScore === bScore)
              return 0;
            if (aScore === void 0)
              return 1;
            if (bScore === void 0)
              return -1;
            return aScore - bScore;
          });
        }
        this._scoreMap = previousScoreMap;
        return elements;
      });
    } finally {
      this.end();
    }
  }
  _markScore(element, score) {
    if (this._scoreMap)
      this._scoreMap.set(element, score);
  }
  _hasScopeClause(selector) {
    return selector.simples.some((simple) => simple.selector.functions.some((f) => f.name === "scope"));
  }
  _expandContextForScopeMatching(context) {
    if (context.scope.nodeType !== 1)
      return context;
    const scope = parentElementOrShadowHost(context.scope);
    if (!scope)
      return context;
    return { ...context, scope, originalScope: context.originalScope || context.scope };
  }
  _matchesSimple(element, simple, context) {
    return this._cached(this._cacheMatchesSimple, element, [simple, context.scope, context.pierceShadow, context.originalScope], () => {
      if (element === context.scope)
        return false;
      if (simple.css && !this._matchesCSS(element, simple.css))
        return false;
      for (const func of simple.functions) {
        if (!this._matchesEngine(this._getEngine(func.name), element, func.args, context))
          return false;
      }
      return true;
    });
  }
  _querySimple(context, simple) {
    if (!simple.functions.length)
      return this._queryCSS(context, simple.css || "*");
    return this._cached(this._cacheQuerySimple, simple, [context.scope, context.pierceShadow, context.originalScope], () => {
      let css = simple.css;
      const funcs = simple.functions;
      if (css === "*" && funcs.length)
        css = void 0;
      let elements;
      let firstIndex = -1;
      if (css !== void 0) {
        elements = this._queryCSS(context, css);
      } else {
        firstIndex = funcs.findIndex((func) => this._getEngine(func.name).query !== void 0);
        if (firstIndex === -1)
          firstIndex = 0;
        elements = this._queryEngine(this._getEngine(funcs[firstIndex].name), context, funcs[firstIndex].args);
      }
      for (let i = 0; i < funcs.length; i++) {
        if (i === firstIndex)
          continue;
        const engine = this._getEngine(funcs[i].name);
        if (engine.matches !== void 0)
          elements = elements.filter((e) => this._matchesEngine(engine, e, funcs[i].args, context));
      }
      for (let i = 0; i < funcs.length; i++) {
        if (i === firstIndex)
          continue;
        const engine = this._getEngine(funcs[i].name);
        if (engine.matches === void 0)
          elements = elements.filter((e) => this._matchesEngine(engine, e, funcs[i].args, context));
      }
      return elements;
    });
  }
  _matchesParents(element, complex, index, context) {
    if (index < 0)
      return true;
    return this._cached(this._cacheMatchesParents, element, [complex, index, context.scope, context.pierceShadow, context.originalScope], () => {
      const { selector: simple, combinator } = complex.simples[index];
      if (combinator === ">") {
        const parent = parentElementOrShadowHostInContext(element, context);
        if (!parent || !this._matchesSimple(parent, simple, context))
          return false;
        return this._matchesParents(parent, complex, index - 1, context);
      }
      if (combinator === "+") {
        const previousSibling = previousSiblingInContext(element, context);
        if (!previousSibling || !this._matchesSimple(previousSibling, simple, context))
          return false;
        return this._matchesParents(previousSibling, complex, index - 1, context);
      }
      if (combinator === "") {
        let parent = parentElementOrShadowHostInContext(element, context);
        while (parent) {
          if (this._matchesSimple(parent, simple, context)) {
            if (this._matchesParents(parent, complex, index - 1, context))
              return true;
            if (complex.simples[index - 1].combinator === "")
              break;
          }
          parent = parentElementOrShadowHostInContext(parent, context);
        }
        return false;
      }
      if (combinator === "~") {
        let previousSibling = previousSiblingInContext(element, context);
        while (previousSibling) {
          if (this._matchesSimple(previousSibling, simple, context)) {
            if (this._matchesParents(previousSibling, complex, index - 1, context))
              return true;
            if (complex.simples[index - 1].combinator === "~")
              break;
          }
          previousSibling = previousSiblingInContext(previousSibling, context);
        }
        return false;
      }
      if (combinator === ">=") {
        let parent = element;
        while (parent) {
          if (this._matchesSimple(parent, simple, context)) {
            if (this._matchesParents(parent, complex, index - 1, context))
              return true;
            if (complex.simples[index - 1].combinator === "")
              break;
          }
          parent = parentElementOrShadowHostInContext(parent, context);
        }
        return false;
      }
      throw new Error(`Unsupported combinator "${combinator}"`);
    });
  }
  _matchesEngine(engine, element, args, context) {
    if (engine.matches)
      return this._callMatches(engine, element, args, context);
    if (engine.query)
      return this._callQuery(engine, args, context).includes(element);
    throw new Error(`Selector engine should implement "matches" or "query"`);
  }
  _queryEngine(engine, context, args) {
    if (engine.query)
      return this._callQuery(engine, args, context);
    if (engine.matches)
      return this._queryCSS(context, "*").filter((element) => this._callMatches(engine, element, args, context));
    throw new Error(`Selector engine should implement "matches" or "query"`);
  }
  _callMatches(engine, element, args, context) {
    return this._cached(this._cacheCallMatches, element, [engine, context.scope, context.pierceShadow, context.originalScope, ...args], () => {
      return engine.matches(element, args, context, this);
    });
  }
  _callQuery(engine, args, context) {
    return this._cached(this._cacheCallQuery, engine, [context.scope, context.pierceShadow, context.originalScope, ...args], () => {
      return engine.query(context, args, this);
    });
  }
  _matchesCSS(element, css) {
    return element.matches(css);
  }
  _queryCSS(context, css) {
    return this._cached(this._cacheQueryCSS, css, [context.scope, context.pierceShadow, context.originalScope], () => {
      let result = [];
      function query(root) {
        result = result.concat([...root.querySelectorAll(css)]);
        if (!context.pierceShadow)
          return;
        if (root.shadowRoot)
          query(root.shadowRoot);
        for (const element of root.querySelectorAll("*")) {
          if (element.shadowRoot)
            query(element.shadowRoot);
        }
      }
      query(context.scope);
      return result;
    });
  }
  _getEngine(name) {
    const engine = this._engines.get(name);
    if (!engine)
      throw new Error(`Unknown selector engine "${name}"`);
    return engine;
  }
};
var isEngine = {
  matches(element, args, context, evaluator) {
    if (args.length === 0)
      throw new Error(`"is" engine expects non-empty selector list`);
    return args.some((selector) => evaluator.matches(element, selector, context));
  },
  query(context, args, evaluator) {
    if (args.length === 0)
      throw new Error(`"is" engine expects non-empty selector list`);
    let elements = [];
    for (const arg of args)
      elements = elements.concat(evaluator.query(context, arg));
    return args.length === 1 ? elements : sortInDOMOrder(elements);
  }
};
var hasEngine = {
  matches(element, args, context, evaluator) {
    if (args.length === 0)
      throw new Error(`"has" engine expects non-empty selector list`);
    return evaluator.query({ ...context, scope: element }, args).length > 0;
  }
  // TODO: we can implement efficient "query" by matching "args" and returning
  // all parents/descendants, just have to be careful with the ":scope" matching.
};
var scopeEngine = {
  matches(element, args, context, evaluator) {
    if (args.length !== 0)
      throw new Error(`"scope" engine expects no arguments`);
    const actualScope = context.originalScope || context.scope;
    if (actualScope.nodeType === 9)
      return element === actualScope.documentElement;
    return element === actualScope;
  },
  query(context, args, evaluator) {
    if (args.length !== 0)
      throw new Error(`"scope" engine expects no arguments`);
    const actualScope = context.originalScope || context.scope;
    if (actualScope.nodeType === 9) {
      const root = actualScope.documentElement;
      return root ? [root] : [];
    }
    if (actualScope.nodeType === 1)
      return [actualScope];
    return [];
  }
};
var notEngine = {
  matches(element, args, context, evaluator) {
    if (args.length === 0)
      throw new Error(`"not" engine expects non-empty selector list`);
    return !evaluator.matches(element, args, context);
  }
};
var lightEngine = {
  query(context, args, evaluator) {
    return evaluator.query({ ...context, pierceShadow: false }, args);
  },
  matches(element, args, context, evaluator) {
    return evaluator.matches(element, args, { ...context, pierceShadow: false });
  }
};
var visibleEngine = {
  matches(element, args, context, evaluator) {
    if (args.length)
      throw new Error(`"visible" engine expects no arguments`);
    return isElementVisible(element);
  }
};
var textEngine = {
  matches(element, args, context, evaluator) {
    if (args.length !== 1 || typeof args[0] !== "string")
      throw new Error(`"text" engine expects a single string`);
    const text = normalizeWhiteSpace(args[0]).toLowerCase();
    const matcher = (elementText2) => elementText2.normalized.toLowerCase().includes(text);
    return elementMatchesText(evaluator._cacheText, element, matcher) === "self";
  }
};
var textIsEngine = {
  matches(element, args, context, evaluator) {
    if (args.length !== 1 || typeof args[0] !== "string")
      throw new Error(`"text-is" engine expects a single string`);
    const text = normalizeWhiteSpace(args[0]);
    const matcher = (elementText2) => {
      if (!text && !elementText2.immediate.length)
        return true;
      return elementText2.immediate.some((s) => normalizeWhiteSpace(s) === text);
    };
    return elementMatchesText(evaluator._cacheText, element, matcher) !== "none";
  }
};
var textMatchesEngine = {
  matches(element, args, context, evaluator) {
    if (args.length === 0 || typeof args[0] !== "string" || args.length > 2 || args.length === 2 && typeof args[1] !== "string")
      throw new Error(`"text-matches" engine expects a regexp body and optional regexp flags`);
    const re = new RegExp(args[0], args.length === 2 ? args[1] : void 0);
    const matcher = (elementText2) => {
      re.lastIndex = 0;
      return re.test(elementText2.full);
    };
    return elementMatchesText(evaluator._cacheText, element, matcher) === "self";
  }
};
var hasTextEngine = {
  matches(element, args, context, evaluator) {
    if (args.length !== 1 || typeof args[0] !== "string")
      throw new Error(`"has-text" engine expects a single string`);
    if (shouldSkipForTextMatching(element))
      return false;
    const text = normalizeWhiteSpace(args[0]).toLowerCase();
    const matcher = (elementText2) => elementText2.normalized.toLowerCase().includes(text);
    return matcher(elementText(evaluator._cacheText, element));
  }
};
function createLayoutEngine(name) {
  return {
    matches(element, args, context, evaluator) {
      const maxDistance = args.length && typeof args[args.length - 1] === "number" ? args[args.length - 1] : void 0;
      const queryArgs = maxDistance === void 0 ? args : args.slice(0, args.length - 1);
      if (args.length < 1 + (maxDistance === void 0 ? 0 : 1))
        throw new Error(`"${name}" engine expects a selector list and optional maximum distance in pixels`);
      const inner = evaluator.query(context, queryArgs);
      const score = layoutSelectorScore(name, element, inner, maxDistance);
      if (score === void 0)
        return false;
      evaluator._markScore(element, score);
      return true;
    }
  };
}
var nthMatchEngine = {
  query(context, args, evaluator) {
    let index = args[args.length - 1];
    if (args.length < 2)
      throw new Error(`"nth-match" engine expects non-empty selector list and an index argument`);
    if (typeof index !== "number" || index < 1)
      throw new Error(`"nth-match" engine expects a one-based index as the last argument`);
    const elements = isEngine.query(context, args.slice(0, args.length - 1), evaluator);
    index--;
    return index < elements.length ? [elements[index]] : [];
  }
};
function parentElementOrShadowHostInContext(element, context) {
  if (element === context.scope)
    return;
  if (!context.pierceShadow)
    return element.parentElement || void 0;
  return parentElementOrShadowHost(element);
}
function previousSiblingInContext(element, context) {
  if (element === context.scope)
    return;
  return element.previousElementSibling || void 0;
}
function sortInDOMOrder(elements) {
  const elementToEntry = /* @__PURE__ */ new Map();
  const roots = [];
  const result = [];
  function append(element) {
    let entry = elementToEntry.get(element);
    if (entry)
      return entry;
    const parent = parentElementOrShadowHost(element);
    if (parent) {
      const parentEntry = append(parent);
      parentEntry.children.push(element);
    } else {
      roots.push(element);
    }
    entry = { children: [], taken: false };
    elementToEntry.set(element, entry);
    return entry;
  }
  for (const e of elements)
    append(e).taken = true;
  function visit3(element) {
    const entry = elementToEntry.get(element);
    if (entry.taken)
      result.push(element);
    if (entry.children.length > 1) {
      const set2 = new Set(entry.children);
      entry.children = [];
      let child = element.firstElementChild;
      while (child && entry.children.length < set2.size) {
        if (set2.has(child))
          entry.children.push(child);
        child = child.nextElementSibling;
      }
      child = element.shadowRoot ? element.shadowRoot.firstElementChild : null;
      while (child && entry.children.length < set2.size) {
        if (set2.has(child))
          entry.children.push(child);
        child = child.nextElementSibling;
      }
    }
    entry.children.forEach(visit3);
  }
  roots.forEach(visit3);
  return result;
}

// packages/injected/src/selectorGenerator.ts
var kTextScoreRange = 10;
var kExactPenalty = kTextScoreRange / 2;
var kTestIdScore = 1;
var kOtherTestIdScore = 2;
var kIframeByAttributeScore = 10;
var kBeginPenalizedScore = 50;
var kRoleWithNameScore = 100;
var kPlaceholderScore = 120;
var kLabelScore = 140;
var kAltTextScore = 160;
var kTextScore = 180;
var kTitleScore = 200;
var kTextScoreRegex = 250;
var kPlaceholderScoreExact = kPlaceholderScore + kExactPenalty;
var kLabelScoreExact = kLabelScore + kExactPenalty;
var kRoleWithNameScoreExact = kRoleWithNameScore + kExactPenalty;
var kAltTextScoreExact = kAltTextScore + kExactPenalty;
var kTextScoreExact = kTextScore + kExactPenalty;
var kTitleScoreExact = kTitleScore + kExactPenalty;
var kEndPenalizedScore = 300;
var kCSSIdScore = 500;
var kRoleWithoutNameScore = 510;
var kCSSInputTypeNameScore = 520;
var kCSSTagNameScore = 530;
var kNthScore = 1e4;
var kCSSFallbackScore = 1e7;
var kScoreThresholdForTextExpect = 1e3;
function generateSelector(injectedScript, targetElement, options) {
  var _a;
  injectedScript._evaluator.begin();
  const cache = { allowText: /* @__PURE__ */ new Map(), disallowText: /* @__PURE__ */ new Map() };
  beginAriaCaches();
  beginDOMCaches();
  try {
    let targetTokens;
    if (options.forTextExpect) {
      targetTokens = cssFallback(injectedScript, targetElement.ownerDocument.documentElement, options);
      for (let element = targetElement; element; element = parentElementOrShadowHost(element)) {
        const tokens = generateSelectorFor(cache, injectedScript, element, { ...options, noText: true });
        if (!tokens)
          continue;
        const score = combineScores(tokens);
        if (score <= kScoreThresholdForTextExpect) {
          targetTokens = tokens;
          break;
        }
      }
    } else {
      if (!targetElement.matches("input,textarea,select") && !targetElement.isContentEditable) {
        const interactiveParent = closestCrossShadow(targetElement, "button,select,input,[role=button],[role=checkbox],[role=radio],a,[role=link]", options.root);
        if (interactiveParent && isElementVisible(interactiveParent))
          targetElement = interactiveParent;
      }
      targetTokens = generateSelectorFor(cache, injectedScript, targetElement, options) || cssFallback(injectedScript, targetElement, options);
    }
    const selector = joinTokens(targetTokens);
    const parsedSelector = injectedScript.parseSelector(selector);
    return {
      selector,
      elements: injectedScript.querySelectorAll(parsedSelector, (_a = options.root) != null ? _a : targetElement.ownerDocument)
    };
  } finally {
    endDOMCaches();
    endAriaCaches();
    injectedScript._evaluator.end();
  }
}
function generateSelectorFor(cache, injectedScript, targetElement, options) {
  var _a;
  if (options.root && !isInsideScope(options.root, targetElement))
    throw new Error(`Target element must belong to the root's subtree`);
  if (targetElement === options.root)
    return [{ engine: "css", selector: ":scope", score: 1 }];
  if (targetElement.ownerDocument.documentElement === targetElement)
    return [{ engine: "css", selector: "html", score: 1 }];
  let result = null;
  const updateResult = (candidate) => {
    if (!result || combineScores(candidate) < combineScores(result))
      result = candidate;
  };
  const candidates = [];
  for (const candidate of buildTextCandidates(injectedScript, targetElement, !options.isRecursive, options))
    candidates.push({ candidate, isTextCandidate: true });
  for (const token of buildNoTextCandidates(injectedScript, targetElement, options)) {
    if (options.omitInternalEngines && token.engine.startsWith("internal:"))
      continue;
    candidates.push({ candidate: [token], isTextCandidate: false });
  }
  candidates.sort((a, b) => combineScores(a.candidate) - combineScores(b.candidate));
  for (const { candidate, isTextCandidate } of candidates) {
    const elements = injectedScript.querySelectorAll(injectedScript.parseSelector(joinTokens(candidate)), (_a = options.root) != null ? _a : targetElement.ownerDocument);
    if (!elements.includes(targetElement)) {
      continue;
    }
    if (elements.length === 1) {
      updateResult(candidate);
      break;
    }
    const index = elements.indexOf(targetElement);
    if (index > 5) {
      continue;
    }
    updateResult([...candidate, { engine: "nth", selector: String(index), score: kNthScore }]);
    if (options.isRecursive) {
      continue;
    }
    for (let parent = parentElementOrShadowHost(targetElement); parent && parent !== options.root; parent = parentElementOrShadowHost(parent)) {
      const filtered = elements.filter((e) => isInsideScope(parent, e) && e !== parent);
      const newIndex = filtered.indexOf(targetElement);
      if (filtered.length > 5 || newIndex === -1 || newIndex === index && filtered.length > 1) {
        continue;
      }
      const inParent = filtered.length === 1 ? candidate : [...candidate, { engine: "nth", selector: String(newIndex), score: kNthScore }];
      const idealSelectorForParent = { engine: "", selector: "", score: 1 };
      if (result && combineScores([idealSelectorForParent, ...inParent]) >= combineScores(result)) {
        continue;
      }
      const noText = !!options.noText || isTextCandidate;
      const cacheMap = noText ? cache.disallowText : cache.allowText;
      let parentTokens = cacheMap.get(parent);
      if (parentTokens === void 0) {
        parentTokens = generateSelectorFor(cache, injectedScript, parent, { ...options, isRecursive: true, noText }) || cssFallback(injectedScript, parent, options);
        cacheMap.set(parent, parentTokens);
      }
      if (!parentTokens)
        continue;
      updateResult([...parentTokens, ...inParent]);
    }
  }
  return result;
}
function buildNoTextCandidates(injectedScript, element, options) {
  const candidates = [];
  const testIdAttributeNames = splitTestIdAttributeNames(options.testIdAttributeName);
  {
    for (const attr of ["data-testid", "data-test-id", "data-test"]) {
      if (!testIdAttributeNames.includes(attr) && element.getAttribute(attr))
        candidates.push({ engine: "css", selector: `[${attr}=${quoteCSSAttributeValue(element.getAttribute(attr))}]`, score: kOtherTestIdScore });
    }
    const idAttr = element.getAttribute("id");
    if (idAttr && !isGuidLike(idAttr))
      candidates.push({ engine: "css", selector: makeSelectorForId(idAttr), score: kCSSIdScore });
    candidates.push({ engine: "css", selector: escapeNodeName(element), score: kCSSTagNameScore });
  }
  if (element.nodeName === "IFRAME" || element.nodeName === "FRAME") {
    for (const attribute of ["name", "title"]) {
      if (element.getAttribute(attribute))
        candidates.push({ engine: "css", selector: `${escapeNodeName(element)}[${attribute}=${quoteCSSAttributeValue(element.getAttribute(attribute))}]`, score: kIframeByAttributeScore });
    }
    for (const testIdAttr of testIdAttributeNames) {
      if (element.getAttribute(testIdAttr))
        candidates.push({ engine: "css", selector: `[${testIdAttr}=${quoteCSSAttributeValue(element.getAttribute(testIdAttr))}]`, score: kTestIdScore });
    }
    penalizeScoreForLength([candidates]);
    return candidates;
  }
  for (const testIdAttr of testIdAttributeNames) {
    if (element.getAttribute(testIdAttr))
      candidates.push({ engine: "internal:testid", selector: `[${testIdAttr}=${escapeForAttributeSelector(element.getAttribute(testIdAttr), true)}]`, score: kTestIdScore });
  }
  if (element.nodeName === "INPUT" || element.nodeName === "TEXTAREA") {
    const input = element;
    if (input.placeholder) {
      candidates.push({ engine: "internal:attr", selector: `[placeholder=${escapeForAttributeSelector(input.placeholder, true)}]`, score: kPlaceholderScoreExact });
      for (const alternative of suitableTextAlternatives(input.placeholder))
        candidates.push({ engine: "internal:attr", selector: `[placeholder=${escapeForAttributeSelector(alternative.text, false)}]`, score: kPlaceholderScore - alternative.scoreBonus });
    }
  }
  const labels = getElementLabels(injectedScript._evaluator._cacheText, element, { skipRefsInsideElement: options.noText });
  for (const label of labels) {
    const labelText = label.normalized;
    candidates.push({ engine: "internal:label", selector: escapeForTextSelector(labelText, true), score: kLabelScoreExact });
    for (const alternative of suitableTextAlternatives(labelText))
      candidates.push({ engine: "internal:label", selector: escapeForTextSelector(alternative.text, false), score: kLabelScore - alternative.scoreBonus });
  }
  const ariaRole = getAriaRole(element);
  if (ariaRole && !["none", "presentation"].includes(ariaRole))
    candidates.push({ engine: "internal:role", selector: ariaRole, score: kRoleWithoutNameScore });
  if (element.getAttribute("name") && ["BUTTON", "FORM", "FIELDSET", "FRAME", "IFRAME", "INPUT", "KEYGEN", "OBJECT", "OUTPUT", "SELECT", "TEXTAREA", "MAP", "META", "PARAM"].includes(element.nodeName))
    candidates.push({ engine: "css", selector: `${escapeNodeName(element)}[name=${quoteCSSAttributeValue(element.getAttribute("name"))}]`, score: kCSSInputTypeNameScore });
  if (["INPUT", "TEXTAREA"].includes(element.nodeName) && element.getAttribute("type") !== "hidden") {
    if (element.getAttribute("type"))
      candidates.push({ engine: "css", selector: `${escapeNodeName(element)}[type=${quoteCSSAttributeValue(element.getAttribute("type"))}]`, score: kCSSInputTypeNameScore });
  }
  if (["INPUT", "TEXTAREA", "SELECT"].includes(element.nodeName) && element.getAttribute("type") !== "hidden")
    candidates.push({ engine: "css", selector: escapeNodeName(element), score: kCSSInputTypeNameScore + 1 });
  penalizeScoreForLength([candidates]);
  return candidates;
}
function buildTextCandidates(injectedScript, element, isTargetNode, options) {
  if (element.nodeName === "SELECT")
    return [];
  const candidates = [];
  if (!options.noText) {
    const title = element.getAttribute("title");
    if (title) {
      candidates.push([{ engine: "internal:attr", selector: `[title=${escapeForAttributeSelector(title, true)}]`, score: kTitleScoreExact }]);
      for (const alternative of suitableTextAlternatives(title))
        candidates.push([{ engine: "internal:attr", selector: `[title=${escapeForAttributeSelector(alternative.text, false)}]`, score: kTitleScore - alternative.scoreBonus }]);
    }
    const alt = element.getAttribute("alt");
    if (alt && ["APPLET", "AREA", "IMG", "INPUT"].includes(element.nodeName)) {
      candidates.push([{ engine: "internal:attr", selector: `[alt=${escapeForAttributeSelector(alt, true)}]`, score: kAltTextScoreExact }]);
      for (const alternative of suitableTextAlternatives(alt))
        candidates.push([{ engine: "internal:attr", selector: `[alt=${escapeForAttributeSelector(alternative.text, false)}]`, score: kAltTextScore - alternative.scoreBonus }]);
    }
  }
  const text = options.noText ? "" : elementText(injectedScript._evaluator._cacheText, element).normalized;
  const textAlternatives = text ? suitableTextAlternatives(text) : [];
  if (text) {
    if (isTargetNode) {
      if (text.length <= 80)
        candidates.push([{ engine: "internal:text", selector: escapeForTextSelector(text, true), score: kTextScoreExact }]);
      for (const alternative of textAlternatives)
        candidates.push([{ engine: "internal:text", selector: escapeForTextSelector(alternative.text, false), score: kTextScore - alternative.scoreBonus }]);
    }
    const cssToken = { engine: "css", selector: escapeNodeName(element), score: kCSSTagNameScore };
    for (const alternative of textAlternatives)
      candidates.push([cssToken, { engine: "internal:has-text", selector: escapeForTextSelector(alternative.text, false), score: kTextScore - alternative.scoreBonus }]);
    if (isTargetNode && text.length <= 80) {
      const re = new RegExp("^" + escapeRegExp(text) + "$");
      candidates.push([cssToken, { engine: "internal:has-text", selector: escapeForTextSelector(re, false), score: kTextScoreRegex }]);
    }
  }
  const ariaRole = getAriaRole(element);
  if (ariaRole && !["none", "presentation"].includes(ariaRole)) {
    const accessibleName = getElementAccessibleName(element, false);
    const ariaName = options.noText && accessibleName.derivedFromContent ? "" : accessibleName.text;
    const accessibleDescription = getElementAccessibleDescription(element, false);
    const ariaDescription = options.noText && accessibleDescription.derivedFromContent ? "" : accessibleDescription.text;
    if (ariaName && !ariaName.match(/^\p{Co}+$/u)) {
      const roleToken = { engine: "internal:role", selector: `${ariaRole}[name=${escapeForAttributeSelector(ariaName, true)}]`, score: kRoleWithNameScoreExact };
      candidates.push([roleToken]);
      for (const alternative of suitableTextAlternatives(ariaName))
        candidates.push([{ engine: "internal:role", selector: `${ariaRole}[name=${escapeForAttributeSelector(alternative.text, false)}]`, score: kRoleWithNameScore - alternative.scoreBonus }]);
      if (ariaDescription) {
        candidates.push([{ engine: "internal:role", selector: `${ariaRole}[name=${escapeForAttributeSelector(ariaName, true)}][description=${escapeForAttributeSelector(ariaDescription, true)}]`, score: kRoleWithNameScoreExact + 1 }]);
        for (const alternative of suitableTextAlternatives(ariaName))
          candidates.push([{ engine: "internal:role", selector: `${ariaRole}[name=${escapeForAttributeSelector(alternative.text, false)}][description=${escapeForAttributeSelector(ariaDescription, false)}]`, score: kRoleWithNameScore - alternative.scoreBonus + 1 }]);
      }
    } else {
      const roleToken = { engine: "internal:role", selector: `${ariaRole}`, score: kRoleWithoutNameScore };
      if (ariaDescription)
        candidates.push([{ engine: "internal:role", selector: `${ariaRole}[description=${escapeForAttributeSelector(ariaDescription, true)}]`, score: kRoleWithoutNameScore + 1 }]);
      for (const alternative of textAlternatives)
        candidates.push([roleToken, { engine: "internal:has-text", selector: escapeForTextSelector(alternative.text, false), score: kTextScore - alternative.scoreBonus }]);
      if (!options.noText && isTargetNode && text.length <= 80) {
        const re = new RegExp("^" + escapeRegExp(text) + "$");
        candidates.push([roleToken, { engine: "internal:has-text", selector: escapeForTextSelector(re, false), score: kTextScoreRegex }]);
      }
    }
  }
  penalizeScoreForLength(candidates);
  return candidates;
}
function makeSelectorForId(id) {
  return /^[a-zA-Z][a-zA-Z0-9\-\_]+$/.test(id) ? "#" + id : `[id=${quoteCSSAttributeValue(id)}]`;
}
function cssFallback(injectedScript, targetElement, options) {
  var _a;
  const root = (_a = options.root) != null ? _a : targetElement.ownerDocument;
  const tokens = [];
  function uniqueCSSSelector(prefix) {
    const path = tokens.slice();
    if (prefix)
      path.unshift(prefix);
    const selector = path.join(" > ");
    const parsedSelector = injectedScript.parseSelector(selector);
    const node = injectedScript.querySelector(parsedSelector, root, false);
    return node === targetElement ? selector : void 0;
  }
  function makeStrict(selector) {
    const token = { engine: "css", selector, score: kCSSFallbackScore };
    const parsedSelector = injectedScript.parseSelector(selector);
    const elements = injectedScript.querySelectorAll(parsedSelector, root);
    if (elements.length === 1)
      return [token];
    const nth = { engine: "nth", selector: String(elements.indexOf(targetElement)), score: kNthScore };
    return [token, nth];
  }
  for (let element = targetElement; element && element !== root; element = parentElementOrShadowHost(element)) {
    let bestTokenForLevel = "";
    if (element.id) {
      const token = makeSelectorForId(element.id);
      const selector = uniqueCSSSelector(token);
      if (selector)
        return makeStrict(selector);
      bestTokenForLevel = token;
    }
    const parent = element.parentNode;
    const classes = [...element.classList].map(escapeClassName);
    for (let i = 0; i < classes.length; ++i) {
      const token = "." + classes.slice(0, i + 1).join(".");
      const selector = uniqueCSSSelector(token);
      if (selector)
        return makeStrict(selector);
      if (!bestTokenForLevel && parent) {
        const sameClassSiblings = parent.querySelectorAll(token);
        if (sameClassSiblings.length === 1)
          bestTokenForLevel = token;
      }
    }
    if (parent) {
      const siblings = [...parent.children];
      const nodeName = element.nodeName;
      const sameTagSiblings = siblings.filter((sibling) => sibling.nodeName === nodeName);
      const token = sameTagSiblings.indexOf(element) === 0 ? escapeNodeName(element) : `${escapeNodeName(element)}:nth-child(${1 + siblings.indexOf(element)})`;
      const selector = uniqueCSSSelector(token);
      if (selector)
        return makeStrict(selector);
      if (!bestTokenForLevel)
        bestTokenForLevel = token;
    } else if (!bestTokenForLevel) {
      bestTokenForLevel = escapeNodeName(element);
    }
    tokens.unshift(bestTokenForLevel);
  }
  return makeStrict(uniqueCSSSelector());
}
function penalizeScoreForLength(groups) {
  for (const group of groups) {
    for (const token of group) {
      if (token.score > kBeginPenalizedScore && token.score < kEndPenalizedScore)
        token.score += Math.min(kTextScoreRange, token.selector.length / 10 | 0);
    }
  }
}
function joinTokens(tokens) {
  const parts = [];
  let lastEngine = "";
  for (const { engine, selector } of tokens) {
    if (parts.length && (lastEngine !== "css" || engine !== "css" || selector.startsWith(":nth-match(")))
      parts.push(">>");
    lastEngine = engine;
    if (engine === "css")
      parts.push(selector);
    else
      parts.push(`${engine}=${selector}`);
  }
  return parts.join(" ");
}
function combineScores(tokens) {
  let score = 0;
  for (let i = 0; i < tokens.length; i++)
    score += tokens[i].score * (tokens.length - i);
  return score;
}
function isGuidLike(id) {
  let lastCharacterType;
  let transitionCount = 0;
  for (let i = 0; i < id.length; ++i) {
    const c = id[i];
    let characterType;
    if (c === "-" || c === "_")
      continue;
    if (c >= "a" && c <= "z")
      characterType = "lower";
    else if (c >= "A" && c <= "Z")
      characterType = "upper";
    else if (c >= "0" && c <= "9")
      characterType = "digit";
    else
      characterType = "other";
    if (characterType === "lower" && lastCharacterType === "upper") {
      lastCharacterType = characterType;
      continue;
    }
    if (lastCharacterType && lastCharacterType !== characterType)
      ++transitionCount;
    lastCharacterType = characterType;
  }
  return transitionCount >= id.length / 4;
}
function trimWordBoundary(text, maxLength) {
  if (text.length <= maxLength)
    return text;
  text = text.substring(0, maxLength);
  const match = text.match(/^(.*)\b(.+?)$/);
  if (!match)
    return "";
  return match[1].trimEnd();
}
function suitableTextAlternatives(text) {
  let result = [];
  {
    const match = text.match(/^([\d.,]+)[^.,\w]/);
    const leadingNumberLength = match ? match[1].length : 0;
    if (leadingNumberLength) {
      const alt = trimWordBoundary(text.substring(leadingNumberLength).trimStart(), 80);
      result.push({ text: alt, scoreBonus: alt.length <= 30 ? 2 : 1 });
    }
  }
  {
    const match = text.match(/[^.,\w]([\d.,]+)$/);
    const trailingNumberLength = match ? match[1].length : 0;
    if (trailingNumberLength) {
      const alt = trimWordBoundary(text.substring(0, text.length - trailingNumberLength).trimEnd(), 80);
      result.push({ text: alt, scoreBonus: alt.length <= 30 ? 2 : 1 });
    }
  }
  if (text.length <= 30) {
    result.push({ text, scoreBonus: 0 });
  } else {
    result.push({ text: trimWordBoundary(text, 80), scoreBonus: 0 });
    result.push({ text: trimWordBoundary(text, 30), scoreBonus: 1 });
  }
  result = result.filter((r) => r.text);
  if (!result.length)
    result.push({ text: text.substring(0, 80), scoreBonus: 0 });
  return result;
}
function escapeNodeName(node) {
  return node.nodeName.toLocaleLowerCase().replace(/[:\.]/g, (char) => "\\" + char);
}
function escapeClassName(className) {
  let result = "";
  for (let i = 0; i < className.length; i++)
    result += cssEscapeCharacter(className, i);
  return result;
}
function cssEscapeCharacter(s, i) {
  const c = s.charCodeAt(i);
  if (c === 0)
    return "\uFFFD";
  if (c >= 1 && c <= 31 || c >= 48 && c <= 57 && (i === 0 || i === 1 && s.charCodeAt(0) === 45))
    return "\\" + c.toString(16) + " ";
  if (i === 0 && c === 45 && s.length === 1)
    return "\\" + s.charAt(i);
  if (c >= 128 || c === 45 || c === 95 || c >= 48 && c <= 57 || c >= 65 && c <= 90 || c >= 97 && c <= 122)
    return s.charAt(i);
  return "\\" + s.charAt(i);
}

// packages/injected/src/xpathSelectorEngine.ts
var XPathEngine = {
  queryAll(root, selector) {
    if (selector.startsWith("/") && root.nodeType !== Node.DOCUMENT_NODE)
      selector = "." + selector;
    const result = [];
    const document = root.ownerDocument || root;
    if (!document)
      return result;
    const it = document.evaluate(selector, root, null, XPathResult.ORDERED_NODE_ITERATOR_TYPE);
    for (let node = it.iterateNext(); node; node = it.iterateNext()) {
      if (node.nodeType === Node.ELEMENT_NODE)
        result.push(node);
    }
    return result;
  }
};

// packages/injected/src/consoleApi.ts
var selectorSymbol = /* @__PURE__ */ Symbol("selector");
selectorSymbol;
var _Locator = class _Locator {
  constructor(injectedScript, selector, options) {
    if (options == null ? void 0 : options.hasText)
      selector += ` >> internal:has-text=${escapeForTextSelector(options.hasText, false)}`;
    if (options == null ? void 0 : options.hasNotText)
      selector += ` >> internal:has-not-text=${escapeForTextSelector(options.hasNotText, false)}`;
    if (options == null ? void 0 : options.has)
      selector += ` >> internal:has=` + JSON.stringify(options.has[selectorSymbol]);
    if (options == null ? void 0 : options.hasNot)
      selector += ` >> internal:has-not=` + JSON.stringify(options.hasNot[selectorSymbol]);
    if ((options == null ? void 0 : options.visible) !== void 0)
      selector += ` >> visible=${options.visible ? "true" : "false"}`;
    this[selectorSymbol] = selector;
    if (selector) {
      const parsed = injectedScript.parseSelector(selector);
      this.element = injectedScript.querySelector(parsed, injectedScript.document, false);
      this.elements = injectedScript.querySelectorAll(parsed, injectedScript.document);
    }
    const selectorBase = selector;
    const self = this;
    self.locator = (selector2, options2) => {
      return new _Locator(injectedScript, selectorBase ? selectorBase + " >> " + selector2 : selector2, options2);
    };
    self.getByTestId = (testId) => self.locator(getByTestIdSelector(injectedScript.testIdAttributeNameForStrictErrorAndConsoleCodegen(), testId));
    self.getByAltText = (text, options2) => self.locator(getByAltTextSelector(text, options2));
    self.getByLabel = (text, options2) => self.locator(getByLabelSelector(text, options2));
    self.getByPlaceholder = (text, options2) => self.locator(getByPlaceholderSelector(text, options2));
    self.getByText = (text, options2) => self.locator(getByTextSelector(text, options2));
    self.getByTitle = (text, options2) => self.locator(getByTitleSelector(text, options2));
    self.getByRole = (role, options2 = {}) => self.locator(getByRoleSelector(role, options2));
    self.filter = (options2) => new _Locator(injectedScript, selector, options2);
    self.visible = () => new _Locator(injectedScript, selector, { visible: true });
    self.first = () => self.locator("nth=0");
    self.last = () => self.locator("nth=-1");
    self.nth = (index) => self.locator(`nth=${index}`);
    self.and = (locator) => new _Locator(injectedScript, selectorBase + ` >> internal:and=` + JSON.stringify(locator[selectorSymbol]));
    self.or = (locator) => new _Locator(injectedScript, selectorBase + ` >> internal:or=` + JSON.stringify(locator[selectorSymbol]));
  }
};
var Locator = _Locator;
var ConsoleAPI = class {
  constructor(injectedScript) {
    this._injectedScript = injectedScript;
  }
  install() {
    if (this._injectedScript.window.playwright)
      return;
    this._injectedScript.window.playwright = {
      $: (selector, strict) => this._querySelector(selector, !!strict),
      $$: (selector) => this._querySelectorAll(selector),
      inspect: (selector) => this._inspect(selector),
      selector: (element) => this._selector(element),
      generateLocator: (element, language) => this._generateLocator(element, language),
      ariaSnapshot: (element, options) => {
        return this._injectedScript.ariaSnapshot(element || this._injectedScript.document.body, options || { mode: "default" });
      },
      resume: () => this._resume(),
      ...new Locator(this._injectedScript, "")
    };
    delete this._injectedScript.window.playwright.filter;
    delete this._injectedScript.window.playwright.visible;
    delete this._injectedScript.window.playwright.first;
    delete this._injectedScript.window.playwright.last;
    delete this._injectedScript.window.playwright.nth;
    delete this._injectedScript.window.playwright.and;
    delete this._injectedScript.window.playwright.or;
  }
  _querySelector(selector, strict) {
    if (typeof selector !== "string")
      throw new Error(`Usage: playwright.query('Playwright >> selector').`);
    const parsed = this._injectedScript.parseSelector(selector);
    return this._injectedScript.querySelector(parsed, this._injectedScript.document, strict);
  }
  _querySelectorAll(selector) {
    if (typeof selector !== "string")
      throw new Error(`Usage: playwright.$$('Playwright >> selector').`);
    const parsed = this._injectedScript.parseSelector(selector);
    return this._injectedScript.querySelectorAll(parsed, this._injectedScript.document);
  }
  _inspect(selector) {
    if (typeof selector !== "string")
      throw new Error(`Usage: playwright.inspect('Playwright >> selector').`);
    this._injectedScript.window.inspect(this._querySelector(selector, false));
  }
  _selector(element) {
    if (!(element instanceof Element))
      throw new Error(`Usage: playwright.selector(element).`);
    return this._injectedScript.generateSelectorSimple(element);
  }
  _generateLocator(element, language) {
    if (!(element instanceof Element))
      throw new Error(`Usage: playwright.locator(element).`);
    const selector = this._injectedScript.generateSelectorSimple(element);
    return asLocator(language || "javascript", selector);
  }
  _resume() {
    if (!this._injectedScript.window.__pw_resume)
      return false;
    this._injectedScript.window.__pw_resume().catch(() => {
    });
  }
};

// packages/isomorphic/utilityScriptSerializers.ts
var kFunctionBindingPrefix = "__pw_fn_";
var kBindingsControllerProperty = "__playwright__binding__controller__";
function isRegExp2(obj) {
  try {
    return obj instanceof RegExp || Object.prototype.toString.call(obj) === "[object RegExp]";
  } catch (error) {
    return false;
  }
}
function isDate(obj) {
  try {
    return obj instanceof Date || Object.prototype.toString.call(obj) === "[object Date]";
  } catch (error) {
    return false;
  }
}
function isURL(obj) {
  try {
    return obj instanceof URL || Object.prototype.toString.call(obj) === "[object URL]";
  } catch (error) {
    return false;
  }
}
function isError(obj) {
  var _a;
  try {
    return obj instanceof Error || obj && ((_a = Object.getPrototypeOf(obj)) == null ? void 0 : _a.name) === "Error";
  } catch (error) {
    return false;
  }
}
function isTypedArray(obj, constructor) {
  try {
    return obj instanceof constructor || Object.prototype.toString.call(obj) === `[object ${constructor.name}]`;
  } catch (error) {
    return false;
  }
}
function isArrayBuffer(obj) {
  try {
    return obj instanceof ArrayBuffer || Object.prototype.toString.call(obj) === "[object ArrayBuffer]";
  } catch (error) {
    return false;
  }
}
function isMap(obj) {
  try {
    return obj instanceof Map || Object.prototype.toString.call(obj) === "[object Map]";
  } catch (error) {
    return false;
  }
}
function isSet(obj) {
  try {
    return obj instanceof Set || Object.prototype.toString.call(obj) === "[object Set]";
  } catch (error) {
    return false;
  }
}
var typedArrayConstructors = {
  i8: Int8Array,
  ui8: Uint8Array,
  ui8c: Uint8ClampedArray,
  i16: Int16Array,
  ui16: Uint16Array,
  i32: Int32Array,
  ui32: Uint32Array,
  // TODO: add Float16Array once it's in baseline
  f32: Float32Array,
  f64: Float64Array,
  bi64: BigInt64Array,
  bui64: BigUint64Array
};
function typedArrayToBase64(array) {
  if ("toBase64" in array)
    return array.toBase64();
  const binary2 = Array.from(new Uint8Array(array.buffer, array.byteOffset, array.byteLength)).map((b) => String.fromCharCode(b)).join("");
  return btoa(binary2);
}
function base64ToTypedArray(base64, TypedArrayConstructor) {
  const binary2 = atob(base64);
  const bytes = new Uint8Array(binary2.length);
  for (let i = 0; i < binary2.length; i++)
    bytes[i] = binary2.charCodeAt(i);
  return new TypedArrayConstructor(bytes.buffer);
}
function parseEvaluationResultValue(value, handles = [], refs = /* @__PURE__ */ new Map()) {
  if (Object.is(value, void 0))
    return void 0;
  if (typeof value === "object" && value) {
    if ("ref" in value)
      return refs.get(value.ref);
    if ("v" in value) {
      if (value.v === "undefined")
        return void 0;
      if (value.v === "null")
        return null;
      if (value.v === "NaN")
        return NaN;
      if (value.v === "Infinity")
        return Infinity;
      if (value.v === "-Infinity")
        return -Infinity;
      if (value.v === "-0")
        return -0;
      return void 0;
    }
    if ("d" in value) {
      return new Date(value.d);
    }
    if ("u" in value)
      return new URL(value.u);
    if ("bi" in value)
      return BigInt(value.bi);
    if ("e" in value) {
      const error = new Error(value.e.m);
      error.name = value.e.n;
      error.stack = value.e.s;
      return error;
    }
    if ("r" in value)
      return new RegExp(value.r.p, value.r.f);
    if ("a" in value) {
      const result = [];
      refs.set(value.id, result);
      for (const a of value.a)
        result.push(parseEvaluationResultValue(a, handles, refs));
      return result;
    }
    if ("o" in value) {
      const result = {};
      refs.set(value.id, result);
      for (const { k, v } of value.o) {
        if (k === "__proto__")
          continue;
        result[k] = parseEvaluationResultValue(v, handles, refs);
      }
      return result;
    }
    if ("m" in value) {
      const result = /* @__PURE__ */ new Map();
      refs.set(value.id, result);
      for (const { k, v } of value.m)
        result.set(parseEvaluationResultValue(k, handles, refs), parseEvaluationResultValue(v, handles, refs));
      return result;
    }
    if ("s" in value) {
      const result = /* @__PURE__ */ new Set();
      refs.set(value.id, result);
      for (const item of value.s)
        result.add(parseEvaluationResultValue(item, handles, refs));
      return result;
    }
    if ("h" in value)
      return handles[value.h];
    if ("fn" in value) {
      const name = value.fn;
      return (...args) => globalThis[kBindingsControllerProperty].callBinding(name, ...args);
    }
    if ("ta" in value)
      return base64ToTypedArray(value.ta.b, typedArrayConstructors[value.ta.k]);
    if ("ab" in value)
      return base64ToTypedArray(value.ab.b, Uint8Array).buffer;
  }
  return value;
}
function serializeAsCallArgument(value, handleSerializer, options = {}) {
  return serialize(value, handleSerializer, { visited: /* @__PURE__ */ new Map(), lastId: 0, extendedSerialization: options.extendedSerialization });
}
function serialize(value, handleSerializer, visitorInfo) {
  if (value && typeof value === "object") {
    if (typeof globalThis.Window === "function" && value instanceof globalThis.Window)
      return "ref: <Window>";
    if (typeof globalThis.Document === "function" && value instanceof globalThis.Document)
      return "ref: <Document>";
    if (typeof globalThis.Node === "function" && value instanceof globalThis.Node)
      return "ref: <Node>";
  }
  return innerSerialize(value, handleSerializer, visitorInfo);
}
function innerSerialize(value, handleSerializer, visitorInfo) {
  var _a;
  const result = handleSerializer(value);
  if ("fallThrough" in result)
    value = result.fallThrough;
  else
    return result;
  if (typeof value === "symbol")
    return { v: "undefined" };
  if (Object.is(value, void 0))
    return { v: "undefined" };
  if (Object.is(value, null))
    return { v: "null" };
  if (Object.is(value, NaN))
    return { v: "NaN" };
  if (Object.is(value, Infinity))
    return { v: "Infinity" };
  if (Object.is(value, -Infinity))
    return { v: "-Infinity" };
  if (Object.is(value, -0))
    return { v: "-0" };
  if (typeof value === "boolean")
    return value;
  if (typeof value === "number")
    return value;
  if (typeof value === "string")
    return value;
  if (typeof value === "bigint")
    return { bi: value.toString() };
  if (isError(value)) {
    let stack;
    if ((_a = value.stack) == null ? void 0 : _a.startsWith(value.name + ": " + value.message)) {
      stack = value.stack;
    } else {
      stack = `${value.name}: ${value.message}
${value.stack}`;
    }
    return { e: { n: value.name, m: value.message, s: stack } };
  }
  if (isDate(value))
    return { d: value.toJSON() };
  if (isURL(value))
    return { u: value.toJSON() };
  if (isRegExp2(value))
    return { r: { p: value.source, f: value.flags } };
  for (const [k, ctor] of Object.entries(typedArrayConstructors)) {
    if (isTypedArray(value, ctor))
      return { ta: { b: typedArrayToBase64(value), k } };
  }
  if (isArrayBuffer(value))
    return { ab: { b: typedArrayToBase64(new Uint8Array(value)) } };
  const id = visitorInfo.visited.get(value);
  if (id)
    return { ref: id };
  if (visitorInfo.extendedSerialization) {
    if (isMap(value)) {
      const m = [];
      const id2 = ++visitorInfo.lastId;
      visitorInfo.visited.set(value, id2);
      const iterator = value.entries();
      const next = (/* @__PURE__ */ new Map()).entries().next;
      while (true) {
        const entry = next.call(iterator);
        if (entry.done)
          break;
        m.push({ k: serialize(entry.value[0], handleSerializer, visitorInfo), v: serialize(entry.value[1], handleSerializer, visitorInfo) });
      }
      return { m, id: id2 };
    }
    if (isSet(value)) {
      const s = [];
      const id2 = ++visitorInfo.lastId;
      visitorInfo.visited.set(value, id2);
      const iterator = value.values();
      const next = (/* @__PURE__ */ new Set()).values().next;
      while (true) {
        const entry = next.call(iterator);
        if (entry.done)
          break;
        s.push(serialize(entry.value, handleSerializer, visitorInfo));
      }
      return { s, id: id2 };
    }
  }
  if (Array.isArray(value)) {
    const a = [];
    const id2 = ++visitorInfo.lastId;
    visitorInfo.visited.set(value, id2);
    for (let i = 0; i < value.length; ++i)
      a.push(serialize(value[i], handleSerializer, visitorInfo));
    return { a, id: id2 };
  }
  if (typeof value === "object") {
    const o = [];
    const id2 = ++visitorInfo.lastId;
    visitorInfo.visited.set(value, id2);
    for (const name of Object.keys(value)) {
      let item;
      try {
        item = value[name];
      } catch (e) {
        continue;
      }
      if (name === "toJSON" && typeof item === "function")
        o.push({ k: name, v: { o: [], id: 0 } });
      else
        o.push({ k: name, v: serialize(item, handleSerializer, visitorInfo) });
    }
    let jsonWrapper;
    try {
      if (o.length === 0 && value.toJSON && typeof value.toJSON === "function")
        jsonWrapper = { value: value.toJSON() };
    } catch (e) {
    }
    if (jsonWrapper)
      return innerSerialize(jsonWrapper.value, handleSerializer, visitorInfo);
    return { o, id: id2 };
  }
  if (typeof value === "function" && value.name.startsWith(kFunctionBindingPrefix))
    return { fn: value.name };
}

// packages/injected/src/utilityScript.ts
var UtilityScript = class {
  constructor(global, isUnderTest) {
    var _a, _b, _c, _d, _e, _f, _g, _h;
    this.global = global;
    this.isUnderTest = isUnderTest;
    if (global.__pwClock) {
      this.builtins = global.__pwClock.builtins;
    } else {
      this.builtins = {
        setTimeout: (_a = global.setTimeout) == null ? void 0 : _a.bind(global),
        clearTimeout: (_b = global.clearTimeout) == null ? void 0 : _b.bind(global),
        setInterval: (_c = global.setInterval) == null ? void 0 : _c.bind(global),
        clearInterval: (_d = global.clearInterval) == null ? void 0 : _d.bind(global),
        requestAnimationFrame: (_e = global.requestAnimationFrame) == null ? void 0 : _e.bind(global),
        cancelAnimationFrame: (_f = global.cancelAnimationFrame) == null ? void 0 : _f.bind(global),
        requestIdleCallback: (_g = global.requestIdleCallback) == null ? void 0 : _g.bind(global),
        cancelIdleCallback: (_h = global.cancelIdleCallback) == null ? void 0 : _h.bind(global),
        performance: global.performance,
        Intl: global.Intl,
        Date: global.Date,
        AbortSignal: global.AbortSignal,
        Temporal: global.Temporal
      };
    }
    if (this.isUnderTest)
      global.builtins = this.builtins;
  }
  evaluate(isFunction, returnByValue, expression, argCount, ...argsAndHandles) {
    const args = argsAndHandles.slice(0, argCount);
    const handles = argsAndHandles.slice(argCount);
    const parameters = [];
    for (let i = 0; i < args.length; i++)
      parameters[i] = parseEvaluationResultValue(args[i], handles);
    let result = this.global.eval(expression);
    if (isFunction === true) {
      result = result(...parameters);
    } else if (isFunction === false) {
      result = result;
    } else {
      if (typeof result === "function")
        result = result(...parameters);
    }
    return returnByValue ? this._promiseAwareJsonValueNoThrow(result) : result;
  }
  jsonValue(returnByValue, value) {
    if (value === void 0)
      return void 0;
    return serializeAsCallArgument(value, (value2) => ({ fallThrough: value2 }));
  }
  _promiseAwareJsonValueNoThrow(value) {
    const safeJson = (value2) => {
      try {
        return this.jsonValue(true, value2);
      } catch (e) {
        return void 0;
      }
    };
    if (value && typeof value === "object" && typeof value.then === "function") {
      return (async () => {
        const promiseValue = await value;
        return safeJson(promiseValue);
      })();
    }
    return safeJson(value);
  }
};

// packages/injected/src/injectedScript.ts
var InjectedScript = class {
  constructor(window, options) {
    this._testIdAttributeNameForStrictErrorAndConsoleCodegen = "data-testid";
    // Recorder must use any external dependencies through InjectedScript.
    // Otherwise it will end up with a copy of all modules it uses, and any
    // module-level globals will be duplicated, which leads to subtle bugs.
    this.utils = {
      asLocator,
      cacheNormalizedWhitespaces,
      elementText,
      getAriaRole,
      getElementAccessibleNameText,
      getElementAccessibleDescription,
      isElementVisible,
      isInsideScope,
      normalizeWhiteSpace,
      parseAriaSnapshot,
      generateAriaTree,
      findNewElement,
      // Builtins protect injected code from clock emulation.
      builtins: null
    };
    this.window = window;
    this.document = window.document;
    this.isUnderTest = options.isUnderTest;
    this.utils.builtins = new UtilityScript(window, options.isUnderTest).builtins;
    this._sdkLanguage = options.sdkLanguage;
    this._frameSeq = options.frameSeq;
    this._testIdAttributeNameForStrictErrorAndConsoleCodegen = options.testIdAttributeName;
    this._evaluator = new SelectorEvaluatorImpl();
    this.consoleApi = new ConsoleAPI(this);
    this.onGlobalListenersRemoved = /* @__PURE__ */ new Set();
    this._autoClosingTags = /* @__PURE__ */ new Set(["AREA", "BASE", "BR", "COL", "COMMAND", "EMBED", "HR", "IMG", "INPUT", "KEYGEN", "LINK", "MENUITEM", "META", "PARAM", "SOURCE", "TRACK", "WBR"]);
    this._booleanAttributes = /* @__PURE__ */ new Set(["checked", "selected", "disabled", "readonly", "multiple"]);
    this._eventTypes = /* @__PURE__ */ new Map([
      ["auxclick", "mouse"],
      ["click", "mouse"],
      ["dblclick", "mouse"],
      ["mousedown", "mouse"],
      ["mouseeenter", "mouse"],
      ["mouseleave", "mouse"],
      ["mousemove", "mouse"],
      ["mouseout", "mouse"],
      ["mouseover", "mouse"],
      ["mouseup", "mouse"],
      ["mouseleave", "mouse"],
      ["mousewheel", "mouse"],
      ["keydown", "keyboard"],
      ["keyup", "keyboard"],
      ["keypress", "keyboard"],
      ["textInput", "keyboard"],
      ["touchstart", "touch"],
      ["touchmove", "touch"],
      ["touchend", "touch"],
      ["touchcancel", "touch"],
      ["pointerover", "pointer"],
      ["pointerout", "pointer"],
      ["pointerenter", "pointer"],
      ["pointerleave", "pointer"],
      ["pointerdown", "pointer"],
      ["pointerup", "pointer"],
      ["pointermove", "pointer"],
      ["pointercancel", "pointer"],
      ["gotpointercapture", "pointer"],
      ["lostpointercapture", "pointer"],
      ["focus", "focus"],
      ["blur", "focus"],
      ["drag", "drag"],
      ["dragstart", "drag"],
      ["dragend", "drag"],
      ["dragover", "drag"],
      ["dragenter", "drag"],
      ["dragleave", "drag"],
      ["dragexit", "drag"],
      ["drop", "drag"],
      ["wheel", "wheel"],
      ["deviceorientation", "deviceorientation"],
      ["deviceorientationabsolute", "deviceorientation"],
      ["devicemotion", "devicemotion"]
    ]);
    this._hoverHitTargetInterceptorEvents = /* @__PURE__ */ new Set(["mousemove"]);
    this._tapHitTargetInterceptorEvents = /* @__PURE__ */ new Set(["pointerdown", "pointerup", "touchstart", "touchend", "touchcancel"]);
    this._mouseHitTargetInterceptorEvents = /* @__PURE__ */ new Set(["mousedown", "mouseup", "pointerdown", "pointerup", "click", "auxclick", "dblclick", "contextmenu"]);
    this._allHitTargetInterceptorEvents = /* @__PURE__ */ new Set([...this._hoverHitTargetInterceptorEvents, ...this._tapHitTargetInterceptorEvents, ...this._mouseHitTargetInterceptorEvents]);
    this._engines = /* @__PURE__ */ new Map();
    this._engines.set("xpath", XPathEngine);
    this._engines.set("xpath:light", XPathEngine);
    this._engines.set("role", createRoleEngine(false));
    this._engines.set("text", this._createTextEngine(true, false));
    this._engines.set("text:light", this._createTextEngine(false, false));
    this._engines.set("id", this._createAttributeEngine("id", true));
    this._engines.set("id:light", this._createAttributeEngine("id", false));
    this._engines.set("data-testid", this._createAttributeEngine("data-testid", true));
    this._engines.set("data-testid:light", this._createAttributeEngine("data-testid", false));
    this._engines.set("data-test-id", this._createAttributeEngine("data-test-id", true));
    this._engines.set("data-test-id:light", this._createAttributeEngine("data-test-id", false));
    this._engines.set("data-test", this._createAttributeEngine("data-test", true));
    this._engines.set("data-test:light", this._createAttributeEngine("data-test", false));
    this._engines.set("css", this._createCSSEngine());
    this._engines.set("nth", { queryAll: () => [] });
    this._engines.set("visible", this._createVisibleEngine());
    this._engines.set("internal:control", this._createControlEngine());
    this._engines.set("internal:has", this._createHasEngine());
    this._engines.set("internal:has-not", this._createHasNotEngine());
    this._engines.set("internal:and", { queryAll: () => [] });
    this._engines.set("internal:or", { queryAll: () => [] });
    this._engines.set("internal:chain", this._createInternalChainEngine());
    this._engines.set("internal:label", this._createInternalLabelEngine());
    this._engines.set("internal:text", this._createTextEngine(true, true));
    this._engines.set("internal:has-text", this._createInternalHasTextEngine());
    this._engines.set("internal:has-not-text", this._createInternalHasNotTextEngine());
    this._engines.set("internal:attr", this._createNamedAttributeEngine());
    this._engines.set("internal:testid", this._createTestIdEngine());
    this._engines.set("internal:role", createRoleEngine(true));
    this._engines.set("internal:describe", this._createDescribeEngine());
    this._engines.set("aria-ref", this._createAriaRefEngine());
    this._engines.set("aria-template", this._createAriaTemplateEngine());
    for (const { name, source } of options.customEngines)
      this._engines.set(name, this.eval(source));
    this._stableRafCount = options.stableRafCount;
    this._browserName = options.browserName;
    this._shouldPrependErrorPrefix = !!options.shouldPrependErrorPrefix;
    this._isUtilityWorld = !!options.isUtilityWorld;
    setGlobalOptions({ browserNameForWorkarounds: options.browserName });
    this._setupGlobalListenersRemovalDetection();
    this._setupHitTargetInterceptors();
    if (this.isUnderTest)
      this.window.__injectedScript = this;
  }
  eval(expression) {
    return this.window.eval(expression);
  }
  testIdAttributeNameForStrictErrorAndConsoleCodegen() {
    return this._testIdAttributeNameForStrictErrorAndConsoleCodegen;
  }
  parseSelector(selector) {
    const result = parseSelector(selector);
    visitAllSelectorParts(result, (part) => {
      if (!this._engines.has(part.name))
        throw this.createStacklessError(`Unknown engine "${part.name}" while parsing selector ${selector}`);
    });
    return result;
  }
  generateSelector(targetElement, options) {
    return generateSelector(this, targetElement, options);
  }
  generateSelectorSimple(targetElement, options) {
    return generateSelector(this, targetElement, { ...options, testIdAttributeName: this._testIdAttributeNameForStrictErrorAndConsoleCodegen }).selector;
  }
  querySelector(selector, root, strict) {
    const result = this.querySelectorAll(selector, root);
    if (strict && result.length > 1)
      throw this.strictModeViolationError(selector, result);
    this.checkDeprecatedSelectorUsage(selector, result);
    return result[0];
  }
  _queryNth(elements, part) {
    const list = [...elements];
    let nth = +part.body;
    if (nth === -1)
      nth = list.length - 1;
    return new Set(list.slice(nth, nth + 1));
  }
  _queryLayoutSelector(elements, part, originalRoot) {
    const name = part.name;
    const body = part.body;
    const result = [];
    const inner = this.querySelectorAll(body.parsed, originalRoot);
    for (const element of elements) {
      const score = layoutSelectorScore(name, element, inner, body.distance);
      if (score !== void 0)
        result.push({ element, score });
    }
    result.sort((a, b) => a.score - b.score);
    return new Set(result.map((r) => r.element));
  }
  ariaSnapshot(node, options) {
    const { json } = this.ariaSnapshotJSON(node, options);
    return renderAriaSnapshotAsYaml(json, { convertStringsToRegex: options.mode === "codegen" });
  }
  ariaSnapshotJSON(node, options) {
    if (node.nodeType !== Node.ELEMENT_NODE)
      throw this.createStacklessError("Can only capture aria snapshot of Element nodes.");
    options = { ...options, refPrefix: this._frameSeq && options.mode === "ai" ? "f" + this._frameSeq : "" };
    const ariaSnapshot = generateAriaTree(node, options);
    const rendered = renderAriaTreeAsJSON(ariaSnapshot, options);
    this._lastAriaSnapshotForQuery = ariaSnapshot;
    return { json: rendered.json, iframeRefs: ariaSnapshot.iframeRefs, iframeDepths: rendered.iframeDepths };
  }
  ariaSnapshotForRecorder() {
    const tree = generateAriaTree(this.document.body, { mode: "ai" });
    const { json } = renderAriaTreeAsJSON(tree, { mode: "ai" });
    return { ariaSnapshot: renderAriaSnapshotAsYaml(json), refs: tree.refs };
  }
  ariaSnapshotForExpectFailure(element, options) {
    const { json } = renderAriaTreeAsJSON(generateAriaTree(element, options), options);
    return renderAriaSnapshotAsYaml(json);
  }
  documentContent(includeShadow) {
    let content = "";
    if (this.document.doctype)
      content = new XMLSerializer().serializeToString(this.document.doctype);
    const root = this.document.documentElement;
    if (!root)
      return content;
    if (!includeShadow)
      return content + root.outerHTML;
    const shadowRoots = [];
    const collectShadowRoots = (node) => {
      for (const element of node.querySelectorAll("*")) {
        if (element.shadowRoot) {
          shadowRoots.push(element.shadowRoot);
          collectShadowRoots(element.shadowRoot);
        }
      }
    };
    collectShadowRoots(this.document);
    const emptyRoot = root.cloneNode(false).outerHTML;
    const endTagIndex = emptyRoot.lastIndexOf("</");
    return content + emptyRoot.slice(0, endTagIndex) + root.getHTML({ shadowRoots }) + emptyRoot.slice(endTagIndex);
  }
  getAllElementsMatchingExpectAriaTemplate(document, template) {
    return getAllElementsMatchingExpectAriaTemplate(document.documentElement, template);
  }
  querySelectorAll(selector, root) {
    if (selector.capture !== void 0) {
      if (selector.parts.some((part) => part.name === "nth"))
        throw this.createStacklessError(`Can't query n-th element in a request with the capture.`);
      const withHas = { parts: selector.parts.slice(0, selector.capture + 1) };
      if (selector.capture < selector.parts.length - 1) {
        const parsed = { parts: selector.parts.slice(selector.capture + 1) };
        const has = { name: "internal:has", body: { parsed }, source: stringifySelector(parsed) };
        withHas.parts.push(has);
      }
      return this.querySelectorAll(withHas, root);
    }
    if (!root["querySelectorAll"])
      throw this.createStacklessError("Node is not queryable.");
    if (selector.capture !== void 0) {
      throw this.createStacklessError("Internal error: there should not be a capture in the selector.");
    }
    if (root.nodeType === 11 && selector.parts.length === 1 && selector.parts[0].name === "css" && selector.parts[0].source === ":scope")
      return [root];
    this._evaluator.begin();
    try {
      let roots = /* @__PURE__ */ new Set([root]);
      for (const part of selector.parts) {
        if (part.name === "nth") {
          roots = this._queryNth(roots, part);
        } else if (part.name === "internal:and") {
          const andElements = this.querySelectorAll(part.body.parsed, root);
          roots = new Set(andElements.filter((e) => roots.has(e)));
        } else if (part.name === "internal:or") {
          const orElements = this.querySelectorAll(part.body.parsed, root);
          roots = new Set(sortInDOMOrder(/* @__PURE__ */ new Set([...roots, ...orElements])));
        } else if (kLayoutSelectorNames.includes(part.name)) {
          roots = this._queryLayoutSelector(roots, part, root);
        } else {
          const next = /* @__PURE__ */ new Set();
          for (const root2 of roots) {
            const all = this._queryEngineAll(part, root2);
            for (const one of all)
              next.add(one);
          }
          roots = next;
        }
      }
      return [...roots];
    } finally {
      this._evaluator.end();
    }
  }
  _queryEngineAll(part, root) {
    const result = this._engines.get(part.name).queryAll(root, part.body);
    for (const element of result) {
      if (!("nodeName" in element))
        throw this.createStacklessError(`Expected a Node but got ${Object.prototype.toString.call(element)}`);
    }
    return result;
  }
  _createAttributeEngine(attribute, shadow) {
    const toCSS = (selector) => {
      const css = `[${attribute}=${JSON.stringify(selector)}]`;
      return [{ simples: [{ selector: { css, functions: [] }, combinator: "" }] }];
    };
    return {
      queryAll: (root, selector) => {
        return this._evaluator.query({ scope: root, pierceShadow: shadow }, toCSS(selector));
      }
    };
  }
  _createCSSEngine() {
    return {
      queryAll: (root, body) => {
        return this._evaluator.query({ scope: root, pierceShadow: true }, body);
      }
    };
  }
  _createTextEngine(shadow, internal) {
    const queryAll = (root, selector) => {
      const { matcher, kind } = createTextMatcher(selector, internal);
      const result = [];
      let lastDidNotMatchSelf = null;
      const appendElement = (element) => {
        if (kind === "lax" && lastDidNotMatchSelf && lastDidNotMatchSelf.contains(element))
          return false;
        const matches = elementMatchesText(this._evaluator._cacheText, element, matcher);
        if (matches === "none")
          lastDidNotMatchSelf = element;
        if (matches === "self" || matches === "selfAndChildren" && kind === "strict" && !internal)
          result.push(element);
      };
      if (root.nodeType === Node.ELEMENT_NODE)
        appendElement(root);
      const elements = this._evaluator._queryCSS({ scope: root, pierceShadow: shadow }, "*");
      for (const element of elements)
        appendElement(element);
      return result;
    };
    return { queryAll };
  }
  _createInternalHasTextEngine() {
    return {
      queryAll: (root, selector) => {
        if (root.nodeType !== 1)
          return [];
        const element = root;
        const text = elementText(this._evaluator._cacheText, element);
        const { matcher } = createTextMatcher(selector, true);
        return matcher(text) ? [element] : [];
      }
    };
  }
  _createInternalHasNotTextEngine() {
    return {
      queryAll: (root, selector) => {
        if (root.nodeType !== 1)
          return [];
        const element = root;
        const text = elementText(this._evaluator._cacheText, element);
        const { matcher } = createTextMatcher(selector, true);
        return matcher(text) ? [] : [element];
      }
    };
  }
  _createInternalLabelEngine() {
    return {
      queryAll: (root, selector) => {
        const { matcher } = createTextMatcher(selector, true);
        const allElements = this._evaluator._queryCSS({ scope: root, pierceShadow: true }, "*");
        return allElements.filter((element) => {
          return getElementLabels(this._evaluator._cacheText, element).some((label) => matcher(label));
        });
      }
    };
  }
  _createNamedAttributeEngine() {
    const queryAll = (root, selector) => {
      const parsed = parseAttributeSelector(selector, true);
      if (parsed.name || parsed.attributes.length !== 1)
        throw new Error("Malformed attribute selector: " + selector);
      const { name } = parsed.attributes[0];
      const matcher = createAttributeMatcher(parsed.attributes[0]);
      const elements = this._evaluator._queryCSS({ scope: root, pierceShadow: true }, `[${name}]`);
      return elements.filter((e) => matcher(e.getAttribute(name)));
    };
    return { queryAll };
  }
  _createTestIdEngine() {
    const queryAll = (root, selector) => {
      const parsed = parseAttributeSelector(selector, true);
      if (parsed.name || parsed.attributes.length !== 1)
        throw new Error("Malformed test id selector: " + selector);
      const names = splitTestIdAttributeNames(parsed.attributes[0].name);
      const matcher = createAttributeMatcher(parsed.attributes[0]);
      const cssQuery = names.map((n) => `[${n}]`).join(",");
      const elements = this._evaluator._queryCSS({ scope: root, pierceShadow: true }, cssQuery);
      return elements.filter((e) => names.some((n) => {
        const actual = e.getAttribute(n);
        return actual !== null && matcher(actual);
      }));
    };
    return { queryAll };
  }
  _createDescribeEngine() {
    const queryAll = (root) => {
      if (root.nodeType !== 1)
        return [];
      return [root];
    };
    return { queryAll };
  }
  _createControlEngine() {
    return {
      queryAll(root, body) {
        if (body === "enter-frame")
          return [];
        if (body === "any-frame")
          return [];
        if (body === "return-empty")
          return [];
        if (body === "component") {
          if (root.nodeType !== 1)
            return [];
          return [root.childElementCount === 1 ? root.firstElementChild : root];
        }
        throw new Error(`Internal error, unknown internal:control selector ${body}`);
      }
    };
  }
  _createHasEngine() {
    const queryAll = (root, body) => {
      if (root.nodeType !== 1)
        return [];
      const has = !!this.querySelector(body.parsed, root, false);
      return has ? [root] : [];
    };
    return { queryAll };
  }
  _createHasNotEngine() {
    const queryAll = (root, body) => {
      if (root.nodeType !== 1)
        return [];
      const has = !!this.querySelector(body.parsed, root, false);
      return has ? [] : [root];
    };
    return { queryAll };
  }
  _createVisibleEngine() {
    const queryAll = (root, body) => {
      if (root.nodeType !== 1)
        return [];
      const visible = body === "true";
      return isElementVisible(root) === visible ? [root] : [];
    };
    return { queryAll };
  }
  _createInternalChainEngine() {
    const queryAll = (root, body) => {
      return this.querySelectorAll(body.parsed, root);
    };
    return { queryAll };
  }
  extend(source, params) {
    const constrFunction = this.window.eval(`
    (() => {
      const module = {};
      ${source}
      return module.exports.default();
    })()`);
    return new constrFunction(this, params);
  }
  async viewportRatio(element) {
    return await new Promise((resolve) => {
      const observer = new IntersectionObserver((entries) => {
        resolve(entries[0].intersectionRatio);
        observer.disconnect();
      });
      observer.observe(element);
      this.utils.builtins.requestAnimationFrame(() => {
      });
    });
  }
  getElementBorderWidth(node) {
    if (node.nodeType !== Node.ELEMENT_NODE || !node.ownerDocument || !node.ownerDocument.defaultView)
      return { left: 0, top: 0 };
    const style = node.ownerDocument.defaultView.getComputedStyle(node);
    return { left: parseInt(style.borderLeftWidth || "", 10), top: parseInt(style.borderTopWidth || "", 10) };
  }
  describeIFrameStyle(iframe) {
    if (!iframe.ownerDocument || !iframe.ownerDocument.defaultView)
      return "error:notconnected";
    const defaultView = iframe.ownerDocument.defaultView;
    for (let e = iframe; e; e = parentElementOrShadowHost(e)) {
      if (defaultView.getComputedStyle(e).transform !== "none")
        return "transformed";
    }
    const iframeStyle = defaultView.getComputedStyle(iframe);
    return {
      left: parseInt(iframeStyle.borderLeftWidth || "", 10) + parseInt(iframeStyle.paddingLeft || "", 10),
      top: parseInt(iframeStyle.borderTopWidth || "", 10) + parseInt(iframeStyle.paddingTop || "", 10)
    };
  }
  retarget(node, behavior) {
    let element = node.nodeType === Node.ELEMENT_NODE ? node : node.parentElement;
    if (!element)
      return null;
    if (behavior === "none")
      return element;
    if (!element.matches("input, textarea, select") && !element.isContentEditable) {
      if (behavior === "button-link")
        element = element.closest("button, [role=button], a, [role=link]") || element;
      else
        element = element.closest("button, [role=button], [role=checkbox], [role=radio]") || element;
    }
    if (behavior === "follow-label") {
      if (!element.matches("a, input, textarea, button, select, [role=link], [role=button], [role=checkbox], [role=radio]") && !element.isContentEditable) {
        const enclosingLabel = element.closest("label");
        if (enclosingLabel && enclosingLabel.control)
          element = enclosingLabel.control;
      }
    }
    return element;
  }
  async checkElementStates(node, states, frameVisible) {
    if (states.includes("stable")) {
      const stableResult = await this._checkElementIsStable(node);
      if (stableResult === false)
        return { missingState: "stable" };
      if (stableResult === "error:notconnected")
        return "error:notconnected";
    }
    for (const state of states) {
      if (state !== "stable") {
        const result = this.elementState(node, state, frameVisible);
        if (result.received === "error:notconnected")
          return "error:notconnected";
        if (!result.matches)
          return { missingState: state };
      }
    }
  }
  async _checkElementIsStable(node) {
    const continuePolling = /* @__PURE__ */ Symbol("continuePolling");
    let lastRect;
    let stableRafCounter = 0;
    let lastTime = 0;
    const check = () => {
      const element = this.retarget(node, "no-follow-label");
      if (!element)
        return "error:notconnected";
      const time = this.utils.builtins.performance.now();
      if (this._stableRafCount > 1 && time - lastTime < 15)
        return continuePolling;
      lastTime = time;
      const clientRect = element.getBoundingClientRect();
      const rect = { x: clientRect.top, y: clientRect.left, width: clientRect.width, height: clientRect.height };
      if (lastRect) {
        const samePosition = rect.x === lastRect.x && rect.y === lastRect.y && rect.width === lastRect.width && rect.height === lastRect.height;
        if (!samePosition)
          return false;
        if (++stableRafCounter >= this._stableRafCount)
          return true;
      }
      lastRect = rect;
      return continuePolling;
    };
    let fulfill;
    let reject;
    const result = new Promise((f, r) => {
      fulfill = f;
      reject = r;
    });
    const raf = () => {
      try {
        const success = check();
        if (success !== continuePolling)
          fulfill(success);
        else
          this.utils.builtins.requestAnimationFrame(raf);
      } catch (e) {
        reject(e);
      }
    };
    this.utils.builtins.requestAnimationFrame(raf);
    return result;
  }
  _createAriaRefEngine() {
    const queryAll = (root, selector) => {
      var _a, _b;
      const result = (_b = (_a = this._lastAriaSnapshotForQuery) == null ? void 0 : _a.info) == null ? void 0 : _b.get(selector);
      return result && result.element.isConnected ? [result.element] : [];
    };
    return { queryAll };
  }
  _createAriaTemplateEngine() {
    const queryAll = (root, body) => {
      const template = JSON.parse(body);
      const rootElement = root.nodeType === 9 ? root.documentElement : root;
      if (!rootElement)
        return [];
      return getAllElementsMatchingExpectAriaTemplate(rootElement, template);
    };
    return { queryAll };
  }
  elementState(node, state, frameVisible) {
    const element = this.retarget(node, ["visible", "hidden"].includes(state) ? "none" : "follow-label");
    if (!element || !element.isConnected) {
      if (state === "hidden")
        return { matches: true, received: "hidden" };
      return { matches: false, received: "error:notconnected" };
    }
    if (state === "visible" || state === "hidden") {
      const visible = !!frameVisible && isElementVisible(element);
      return {
        matches: state === "visible" ? visible : !visible,
        received: visible ? "visible" : "hidden"
      };
    }
    if (state === "disabled" || state === "enabled") {
      const disabled = getAriaDisabled(element);
      return {
        matches: state === "disabled" ? disabled : !disabled,
        received: disabled ? "disabled" : "enabled"
      };
    }
    if (state === "editable") {
      const disabled = getAriaDisabled(element);
      const readonly = getReadonly(element);
      if (readonly === "error")
        throw this.createStacklessError("Element is not an <input>, <textarea>, <select> or [contenteditable] and does not have a role allowing [aria-readonly]");
      return {
        matches: !disabled && !readonly,
        received: disabled ? "disabled" : readonly ? "readOnly" : "editable"
      };
    }
    if (state === "checked" || state === "unchecked") {
      const need = state === "checked";
      const checked = getCheckedWithoutMixed(element);
      if (checked === "error")
        throw this.createStacklessError("Not a checkbox or radio button");
      const isRadio = element.nodeName === "INPUT" && element.type === "radio";
      return {
        matches: need === checked,
        received: checked ? "checked" : "unchecked",
        isRadio
      };
    }
    if (state === "indeterminate") {
      const checked = getCheckedAllowMixed(element);
      if (checked === "error")
        throw this.createStacklessError("Not a checkbox or radio button");
      return {
        matches: checked === "mixed",
        received: checked === true ? "checked" : checked === false ? "unchecked" : "mixed"
      };
    }
    throw this.createStacklessError(`Unexpected element state "${state}"`);
  }
  selectOptions(node, optionsToSelect) {
    const element = this.retarget(node, "follow-label");
    if (!element)
      return "error:notconnected";
    if (element.nodeName.toLowerCase() !== "select")
      throw this.createStacklessError("Element is not a <select> element");
    const select = element;
    const options = [...select.options];
    const selectedOptions = [];
    let remainingOptionsToSelect = optionsToSelect.slice();
    for (let index = 0; index < options.length; index++) {
      const option = options[index];
      const normalizedOptionLabel = normalizeWhiteSpace(option.label);
      const filter = (optionToSelect) => {
        if (optionToSelect instanceof Node)
          return option === optionToSelect;
        const matchesLabel = (label) => label === option.label || normalizeWhiteSpace(label) === normalizedOptionLabel;
        let matches = true;
        if (optionToSelect.valueOrLabel !== void 0)
          matches = matches && (optionToSelect.valueOrLabel === option.value || matchesLabel(optionToSelect.valueOrLabel));
        if (optionToSelect.value !== void 0)
          matches = matches && optionToSelect.value === option.value;
        if (optionToSelect.label !== void 0)
          matches = matches && matchesLabel(optionToSelect.label);
        if (optionToSelect.index !== void 0)
          matches = matches && optionToSelect.index === index;
        return matches;
      };
      if (!remainingOptionsToSelect.some(filter))
        continue;
      if (!this.elementState(option, "enabled").matches)
        return "error:optionnotenabled";
      selectedOptions.push(option);
      if (select.multiple) {
        remainingOptionsToSelect = remainingOptionsToSelect.filter((o) => !filter(o));
      } else {
        remainingOptionsToSelect = [];
        break;
      }
    }
    if (remainingOptionsToSelect.length)
      return "error:optionsnotfound";
    select.value = void 0;
    selectedOptions.forEach((option) => option.selected = true);
    select.dispatchEvent(new Event("input", { bubbles: true, composed: true }));
    select.dispatchEvent(new Event("change", { bubbles: true }));
    return selectedOptions.map((option) => option.value);
  }
  fill(node, value) {
    const element = this.retarget(node, "follow-label");
    if (!element)
      return "error:notconnected";
    if (element.nodeName.toLowerCase() === "input") {
      const input = element;
      const type = input.type.toLowerCase();
      const kInputTypesToSetValue = /* @__PURE__ */ new Set(["color", "date", "time", "datetime-local", "month", "range", "week"]);
      const kInputTypesToTypeInto = /* @__PURE__ */ new Set(["", "email", "number", "password", "search", "tel", "text", "url"]);
      if (!kInputTypesToTypeInto.has(type) && !kInputTypesToSetValue.has(type))
        throw this.createStacklessError(`Input of type "${type}" cannot be filled`);
      if (type === "number") {
        value = value.trim();
        if (isNaN(Number(value)))
          throw this.createStacklessError("Cannot type text into input[type=number]");
      }
      if (type === "color")
        value = value.toLowerCase();
      if (kInputTypesToSetValue.has(type)) {
        value = value.trim();
        input.focus();
        input.value = value;
        if (input.value !== value)
          throw this.createStacklessError("Malformed value");
        element.dispatchEvent(new Event("input", { bubbles: true, composed: true }));
        element.dispatchEvent(new Event("change", { bubbles: true }));
        return "done";
      }
    } else if (element.nodeName.toLowerCase() === "textarea") {
    } else if (!element.isContentEditable) {
      throw this.createStacklessError("Element is not an <input>, <textarea> or [contenteditable] element");
    }
    this.selectText(element);
    return "needsinput";
  }
  selectText(node) {
    const element = this.retarget(node, "follow-label");
    if (!element)
      return "error:notconnected";
    if (element.nodeName.toLowerCase() === "input") {
      const input = element;
      input.select();
      input.focus();
      return "done";
    }
    if (element.nodeName.toLowerCase() === "textarea") {
      const textarea = element;
      textarea.selectionStart = 0;
      textarea.selectionEnd = textarea.value.length;
      textarea.focus();
      return "done";
    }
    element.focus();
    const range = element.ownerDocument.createRange();
    range.selectNodeContents(element);
    const selection = element.ownerDocument.defaultView.getSelection();
    if (selection) {
      selection.removeAllRanges();
      selection.addRange(range);
    }
    return "done";
  }
  _activelyFocused(node) {
    const activeElement = node.getRootNode().activeElement;
    const isFocused = activeElement === node && !!node.ownerDocument && node.ownerDocument.hasFocus();
    return { activeElement, isFocused };
  }
  focusNode(node, resetSelectionIfNotFocused) {
    if (!node.isConnected)
      return "error:notconnected";
    if (node.nodeType !== Node.ELEMENT_NODE)
      throw this.createStacklessError("Node is not an element");
    const { activeElement, isFocused: wasFocused } = this._activelyFocused(node);
    if (node.isContentEditable && !wasFocused && activeElement && activeElement.blur) {
      activeElement.blur();
    }
    node.focus();
    node.focus();
    if (resetSelectionIfNotFocused && !wasFocused && node.nodeName.toLowerCase() === "input") {
      try {
        const input = node;
        input.setSelectionRange(0, 0);
      } catch (e) {
      }
    }
    return "done";
  }
  blurNode(node) {
    if (!node.isConnected)
      return "error:notconnected";
    if (node.nodeType !== Node.ELEMENT_NODE)
      throw this.createStacklessError("Node is not an element");
    node.blur();
    return "done";
  }
  setInputFiles(node, payloads) {
    if (node.nodeType !== Node.ELEMENT_NODE)
      return "Node is not of type HTMLElement";
    const element = node;
    if (element.nodeName !== "INPUT")
      return "Not an <input> element";
    const input = element;
    const type = (input.getAttribute("type") || "").toLowerCase();
    if (type !== "file")
      return "Not an input[type=file] element";
    const files = payloads.map((file) => {
      const bytes = Uint8Array.from(atob(file.buffer), (c) => c.charCodeAt(0));
      return new File([bytes], file.name, { type: file.mimeType, lastModified: file.lastModifiedMs });
    });
    const dt = new DataTransfer();
    for (const file of files)
      dt.items.add(file);
    input.files = dt.files;
    input.dispatchEvent(new Event("input", { bubbles: true, composed: true }));
    input.dispatchEvent(new Event("change", { bubbles: true }));
  }
  expectHitTarget(hitPoint, targetElement) {
    var _a;
    const roots = [];
    let parentElement = targetElement;
    while (parentElement) {
      const root = enclosingShadowRootOrDocument(parentElement);
      if (!root)
        break;
      roots.push(root);
      if (root.nodeType === 9)
        break;
      parentElement = root.host;
    }
    let hitElement;
    for (let index = roots.length - 1; index >= 0; index--) {
      const root = roots[index];
      const elements = root.elementsFromPoint(hitPoint.x, hitPoint.y);
      const singleElement = root.elementFromPoint(hitPoint.x, hitPoint.y);
      if (singleElement && elements[0] && parentElementOrShadowHost(singleElement) === elements[0]) {
        const style = this.window.getComputedStyle(singleElement);
        if ((style == null ? void 0 : style.display) === "contents") {
          elements.unshift(singleElement);
        }
      }
      if (elements[0] && elements[1] === singleElement && roots.slice(index).some((r) => r.host === elements[0]))
        elements.shift();
      const innerElement = elements[0];
      if (!innerElement)
        break;
      hitElement = innerElement;
      if (index && innerElement !== roots[index - 1].host)
        break;
    }
    const hitParents = [];
    while (hitElement && hitElement !== targetElement) {
      hitParents.push(hitElement);
      hitElement = (_a = hitElement.assignedSlot) != null ? _a : parentElementOrShadowHost(hitElement);
    }
    if (hitElement === targetElement)
      return "done";
    const hitTargetDescription = this.previewNode(hitParents[0] || this.document.documentElement);
    let rootHitTargetDescription;
    let element = targetElement;
    while (element) {
      const index = hitParents.indexOf(element);
      if (index !== -1) {
        if (index > 1)
          rootHitTargetDescription = this.previewNode(hitParents[index - 1]);
        break;
      }
      element = parentElementOrShadowHost(element);
    }
    if (rootHitTargetDescription)
      return { hitTargetDescription: `${hitTargetDescription} from ${rootHitTargetDescription} subtree` };
    return { hitTargetDescription };
  }
  // Life of a pointer action, for example click.
  //
  // 0. Retry items 1 and 2 while action fails due to navigation or element being detached.
  //   1. Resolve selector to an element.
  //   2. Retry the following steps until the element is detached or frame navigates away.
  //     2a. Wait for the element to be stable (not moving), visible and enabled.
  //     2b. Scroll element into view. Scrolling alternates between:
  //         - Built-in protocol scrolling.
  //         - Anchoring to the top/left, bottom/right and center/center.
  //         This is to scroll elements from under sticky headers/footers.
  //     2c. Click point is calculated, either based on explicitly specified position,
  //         or some visible point of the element based on protocol content quads.
  //     2d. Click point relative to page viewport is converted relative to the target iframe
  //         for the next hit-point check.
  //     2e. (injected) Hit target at the click point must be a descendant of the target element.
  //         This prevents mis-clicking in edge cases like <iframe> overlaying the target.
  //     2f. (injected) Events specific for click (or some other action type) are intercepted on
  //         the Window with capture:true. See 2i for details.
  //         Note: this step is skipped for drag&drop (see inline comments for the reason).
  //     2g. Necessary keyboard modifiers are pressed.
  //     2h. Click event is issued (mousemove + mousedown + mouseup).
  //     2i. (injected) For each event, we check that hit target at the event point
  //         is a descendant of the target element.
  //         This guarantees no race between issuing the event and handling it in the page,
  //         for example due to layout shift.
  //         When hit target check fails, we block all future events in the page.
  //     2j. Keyboard modifiers are restored.
  //     2k. (injected) Event interceptor is removed.
  //     2l. All navigations triggered between 2g-2k are awaited to be either committed or canceled.
  //     2m. If failed, wait for increasing amount of time before the next retry.
  setupHitTargetInterceptor(node, action, hitPoint, blockAllEvents) {
    const element = this.retarget(node, "button-link");
    if (!element || !element.isConnected)
      return "error:notconnected";
    if (hitPoint) {
      const preliminaryResult = this.expectHitTarget(hitPoint, element);
      if (preliminaryResult !== "done")
        return preliminaryResult.hitTargetDescription;
    }
    if (action === "drag")
      return { stop: () => "done" };
    const events = {
      "hover": this._hoverHitTargetInterceptorEvents,
      "tap": this._tapHitTargetInterceptorEvents,
      "mouse": this._mouseHitTargetInterceptorEvents
    }[action];
    let result;
    let listener = (event) => {
      if (!events.has(event.type))
        return;
      if (!event.isTrusted && !event.__pwTrustedSynthetic)
        return;
      const point = !!this.window.TouchEvent && event instanceof this.window.TouchEvent ? event.touches[0] : event;
      if (result === void 0 && point)
        result = this.expectHitTarget({ x: point.clientX, y: point.clientY }, element);
      if (blockAllEvents || result !== "done" && result !== void 0) {
        event.preventDefault();
        event.stopPropagation();
        event.stopImmediatePropagation();
      }
    };
    const stop = () => {
      if (this._hitTargetInterceptor === listener)
        this._hitTargetInterceptor = void 0;
      listener = void 0;
      return result || "done";
    };
    this._hitTargetInterceptor = listener;
    return { stop };
  }
  dispatchEvent(node, type, eventInitObj) {
    var _a, _b, _c, _d, _e;
    let event;
    const eventInit = { bubbles: true, cancelable: true, composed: true, ...eventInitObj };
    switch (this._eventTypes.get(type)) {
      case "mouse":
        event = new MouseEvent(type, eventInit);
        break;
      case "keyboard":
        event = new KeyboardEvent(type, eventInit);
        break;
      case "touch": {
        if (this._browserName === "webkit") {
          const createTouch = (t) => {
            var _a2, _b2, _c2;
            if (t instanceof Touch)
              return t;
            let pageX = t.pageX;
            if (pageX === void 0 && t.clientX !== void 0)
              pageX = t.clientX + (((_a2 = this.document.scrollingElement) == null ? void 0 : _a2.scrollLeft) || 0);
            let pageY = t.pageY;
            if (pageY === void 0 && t.clientY !== void 0)
              pageY = t.clientY + (((_b2 = this.document.scrollingElement) == null ? void 0 : _b2.scrollTop) || 0);
            return this.document.createTouch(this.window, (_c2 = t.target) != null ? _c2 : node, t.identifier, pageX, pageY, t.screenX, t.screenY, t.radiusX, t.radiusY, t.rotationAngle, t.force);
          };
          const createTouchList = (touches) => {
            if (touches instanceof TouchList || !touches)
              return touches;
            return this.document.createTouchList(...touches.map(createTouch));
          };
          (_a = eventInit.target) != null ? _a : eventInit.target = node;
          eventInit.touches = createTouchList(eventInit.touches);
          eventInit.targetTouches = createTouchList(eventInit.targetTouches);
          eventInit.changedTouches = createTouchList(eventInit.changedTouches);
          event = new TouchEvent(type, eventInit);
        } else {
          (_b = eventInit.target) != null ? _b : eventInit.target = node;
          eventInit.touches = (_c = eventInit.touches) == null ? void 0 : _c.map((t) => {
            var _a2;
            return t instanceof Touch ? t : new Touch({ ...t, target: (_a2 = t.target) != null ? _a2 : node });
          });
          eventInit.targetTouches = (_d = eventInit.targetTouches) == null ? void 0 : _d.map((t) => {
            var _a2;
            return t instanceof Touch ? t : new Touch({ ...t, target: (_a2 = t.target) != null ? _a2 : node });
          });
          eventInit.changedTouches = (_e = eventInit.changedTouches) == null ? void 0 : _e.map((t) => {
            var _a2;
            return t instanceof Touch ? t : new Touch({ ...t, target: (_a2 = t.target) != null ? _a2 : node });
          });
          event = new TouchEvent(type, eventInit);
        }
        break;
      }
      case "pointer":
        event = new PointerEvent(type, eventInit);
        break;
      case "focus":
        event = new FocusEvent(type, eventInit);
        break;
      case "drag":
        event = new DragEvent(type, eventInit);
        break;
      case "wheel":
        event = new WheelEvent(type, eventInit);
        break;
      case "deviceorientation":
        try {
          event = new DeviceOrientationEvent(type, eventInit);
        } catch {
          const { bubbles, cancelable, alpha, beta, gamma, absolute } = eventInit;
          event = this.document.createEvent("DeviceOrientationEvent");
          event.initDeviceOrientationEvent(type, bubbles, cancelable, alpha, beta, gamma, absolute);
        }
        break;
      case "devicemotion":
        try {
          event = new DeviceMotionEvent(type, eventInit);
        } catch {
          const { bubbles, cancelable, acceleration, accelerationIncludingGravity, rotationRate, interval } = eventInit;
          event = this.document.createEvent("DeviceMotionEvent");
          event.initDeviceMotionEvent(type, bubbles, cancelable, acceleration, accelerationIncludingGravity, rotationRate, interval);
        }
        break;
      default:
        event = new Event(type, eventInit);
        break;
    }
    node.dispatchEvent(event);
  }
  previewNode(node) {
    if (node.nodeType === Node.TEXT_NODE)
      return oneLine(`#text=${node.nodeValue || ""}`);
    if (node.nodeType !== Node.ELEMENT_NODE)
      return oneLine(`<${node.nodeName.toLowerCase()} />`);
    const element = node;
    const attrs = [];
    for (let i = 0; i < element.attributes.length; i++) {
      const { name, value } = element.attributes[i];
      if (name === "style")
        continue;
      if (!value && this._booleanAttributes.has(name))
        attrs.push(` ${name}`);
      else
        attrs.push(` ${name}="${value}"`);
    }
    attrs.sort((a, b) => a.length - b.length);
    const attrText = trimStringWithEllipsis(attrs.join(""), 500);
    if (this._autoClosingTags.has(element.nodeName))
      return oneLine(`<${element.nodeName.toLowerCase()}${attrText}/>`);
    const children = element.childNodes;
    let onlyText = false;
    if (children.length <= 5) {
      onlyText = true;
      for (let i = 0; i < children.length; i++)
        onlyText = onlyText && children[i].nodeType === Node.TEXT_NODE;
    }
    const text = onlyText ? element.textContent || "" : children.length ? "\u2026" : "";
    return oneLine(`<${element.nodeName.toLowerCase()}${attrText}>${trimStringWithEllipsis(text, 50)}</${element.nodeName.toLowerCase()}>`);
  }
  _generateSelectors(elements) {
    this._evaluator.begin();
    beginAriaCaches();
    beginDOMCaches();
    try {
      const maxElements = this._isUtilityWorld && this._browserName === "firefox" ? 2 : 10;
      const infos = elements.slice(0, maxElements).map((m) => ({
        preview: this.previewNode(m),
        selector: this.generateSelectorSimple(m)
      }));
      return infos.map((info, i) => `${i + 1}) ${info.preview} aka ${asLocator(this._sdkLanguage, info.selector)}`);
    } finally {
      endDOMCaches();
      endAriaCaches();
      this._evaluator.end();
    }
  }
  strictModeViolationError(selector, matches) {
    const lines = this._generateSelectors(matches).map((line) => `
    ` + line);
    if (lines.length < matches.length)
      lines.push("\n    ...");
    return this.createStacklessError(`strict mode violation: ${asLocator(this._sdkLanguage, stringifySelector(selector))} resolved to ${matches.length} elements:${lines.join("")}
`);
  }
  checkDeprecatedSelectorUsage(selector, matches) {
    const kDeprecatedSelectors = /* @__PURE__ */ new Set([
      "_react",
      "_vue",
      "xpath:light",
      "text:light",
      "id:light",
      "data-testid:light",
      "data-test-id:light",
      "data-test:light"
    ]);
    if (!matches.length)
      return;
    const deperecated = selector.parts.find((part) => kDeprecatedSelectors.has(part.name));
    if (!deperecated)
      return;
    const lines = this._generateSelectors(matches).map((line) => `
    ` + line);
    if (lines.length < matches.length)
      lines.push("\n    ...");
    throw this.createStacklessError(`"${deperecated.name}" selector is not supported: ${asLocator(this._sdkLanguage, stringifySelector(selector))} resolved to ${matches.length} element${matches.length === 1 ? "" : "s"}:${lines.join("")}
`);
  }
  createStacklessError(message) {
    const error = this._shouldPrependErrorPrefix ? new Error("Error: " + message) : new Error(message);
    if (this._browserName === "firefox") {
      error.stack = "";
      return error;
    }
    delete error.stack;
    return error;
  }
  createHighlight() {
    return new Highlight(this);
  }
  addMaskedElements(elements, color) {
    const highlight = this._ensureHighlight();
    highlight.addMaskedElements(elements, color);
  }
  _ensureHighlight() {
    if (!this._highlight)
      this._highlight = new Highlight(this);
    this._highlight.install();
    return this._highlight;
  }
  setHighlights(highlights) {
    if (!highlights.length && !this._highlight)
      return;
    const highlight = this._ensureHighlight();
    highlight.setElementHighlights(highlights);
  }
  setScreencastAnnotation(annotation) {
    var _a, _b, _c, _d;
    const highlight = this._ensureHighlight();
    highlight.hideScreencastDecorations();
    if (!annotation) {
      highlight.hideActionTitle();
      return;
    }
    const fadeDuration = (_a = annotation.duration) != null ? _a : 500;
    if (annotation.box && ((_b = annotation.style) == null ? void 0 : _b.highlight))
      highlight.showScreencastHighlight(annotation.box, annotation.style.highlight, fadeDuration);
    if (annotation.point) {
      if (annotation.cursor !== "none") {
        if (annotation.lastCursorPoint)
          highlight.restoreActionCursor(annotation.lastCursorPoint.x, annotation.lastCursorPoint.y);
        highlight.moveActionCursor(annotation.point.x, annotation.point.y, fadeDuration);
      }
      if ((_c = annotation.style) == null ? void 0 : _c.point)
        highlight.showScreencastPoint(annotation.point.x, annotation.point.y, annotation.style.point, fadeDuration);
    }
    if (annotation.actionTitle)
      highlight.showActionTitle(annotation.actionTitle, fadeDuration, annotation.position, (_d = annotation.style) == null ? void 0 : _d.title);
  }
  restoreScreencastCursor(point) {
    this._ensureHighlight().restoreActionCursor(point.x, point.y);
  }
  hideScreencastCursor() {
    var _a;
    (_a = this._highlight) == null ? void 0 : _a.hideActionCursor();
  }
  addUserOverlay(id, html) {
    const highlight = this._ensureHighlight();
    highlight.addUserOverlay(id, html);
  }
  getUserOverlay(id) {
    const highlight = this._ensureHighlight();
    return highlight.getUserOverlay(id);
  }
  removeUserOverlay(id) {
    const highlight = this._ensureHighlight();
    highlight.removeUserOverlay(id);
  }
  setUserOverlaysVisible(visible) {
    const highlight = this._ensureHighlight();
    highlight.setUserOverlaysVisible(visible);
  }
  hideHighlight() {
    if (this._highlight) {
      this._highlight.uninstall();
      delete this._highlight;
    }
  }
  markTargetElements(markedElements) {
    const resetEvent = new CustomEvent("__playwright_reset_targets__", {
      bubbles: true,
      cancelable: true,
      composed: true
    });
    this.document.dispatchEvent(resetEvent);
    const markEvent = new CustomEvent("__playwright_mark_target__", {
      bubbles: true,
      cancelable: true,
      composed: true
    });
    for (const element of markedElements)
      element.dispatchEvent(markEvent);
  }
  _setupGlobalListenersRemovalDetection() {
    const customEventName = "__playwright_global_listeners_check__";
    let seenEvent = false;
    const handleCustomEvent = () => seenEvent = true;
    this.window.addEventListener(customEventName, handleCustomEvent);
    new MutationObserver((entries) => {
      const newDocumentElement = entries.some((entry) => Array.from(entry.addedNodes).includes(this.document.documentElement));
      if (!newDocumentElement)
        return;
      seenEvent = false;
      this.window.dispatchEvent(new CustomEvent(customEventName));
      if (seenEvent)
        return;
      this.window.addEventListener(customEventName, handleCustomEvent);
      for (const callback of this.onGlobalListenersRemoved)
        callback();
    }).observe(this.document, { childList: true });
  }
  _setupHitTargetInterceptors() {
    const listener = (event) => {
      var _a;
      return (_a = this._hitTargetInterceptor) == null ? void 0 : _a.call(this, event);
    };
    const addHitTargetInterceptorListeners = () => {
      for (const event of this._allHitTargetInterceptorEvents)
        this.window.addEventListener(event, listener, { capture: true, passive: false });
    };
    addHitTargetInterceptorListeners();
    this.onGlobalListenersRemoved.add(addHitTargetInterceptorListeners);
  }
  async expect(element, options, elements, frameVisible) {
    const isArray = options.expression === "to.have.count" || options.expression.endsWith(".array");
    const core = isArray ? this.expectArray(elements, options) : await this.expectSingleElement(element, options, frameVisible);
    const ariaSnapshot = core.matches !== options.isNot ? void 0 : this._ariaSnapshotForExpect(element, options, frameVisible);
    if (core.received === void 0 && ariaSnapshot === void 0)
      return { matches: core.matches };
    return { matches: core.matches, received: { value: core.received, ariaSnapshot } };
  }
  _ariaSnapshotForExpect(element, options, frameVisible) {
    const expression = options.expression;
    if (expression === "to.have.count" || expression.endsWith(".array") || expression === "to.match.aria")
      return void 0;
    if (frameVisible && isElementVisible(element) && expression !== "to.have.title" && expression !== "to.have.url") {
      const isContainment = expression === "to.have.text";
      return this.ariaSnapshotForExpectFailure(element, { mode: "default", depth: isContainment ? void 0 : 1 });
    }
    if (!this.document.body)
      return void 0;
    return this.ariaSnapshotForExpectFailure(this.document.body, { mode: "default" });
  }
  async expectSingleElement(element, options, frameVisible) {
    var _a, _b;
    const expression = options.expression;
    {
      if (expression === "to.have.title") {
        const received = this.document.title;
        return { received, matches: new ExpectedTextMatcher(options.expectedText[0]).matches(received) };
      }
      if (expression === "to.have.url") {
        const received = this.document.location.href;
        return { received, matches: new ExpectedTextMatcher(options.expectedText[0]).matches(received) };
      }
    }
    {
      let result;
      if (expression === "to.have.attribute") {
        const hasAttribute = element.hasAttribute(options.expressionArg);
        result = {
          matches: hasAttribute,
          received: hasAttribute ? "attribute present" : "attribute not present"
        };
      } else if (expression === "to.be.checked") {
        const { checked, indeterminate } = options.expectedValue;
        if (indeterminate) {
          if (checked !== void 0)
            throw this.createStacklessError("Can't assert indeterminate and checked at the same time");
          result = this.elementState(element, "indeterminate");
        } else {
          result = this.elementState(element, checked === false ? "unchecked" : "checked");
        }
      } else if (expression === "to.be.disabled") {
        result = this.elementState(element, "disabled");
      } else if (expression === "to.be.editable") {
        result = this.elementState(element, "editable");
      } else if (expression === "to.be.readonly") {
        result = this.elementState(element, "editable");
        result.matches = !result.matches;
      } else if (expression === "to.be.empty") {
        if (element.nodeName === "INPUT" || element.nodeName === "TEXTAREA") {
          const value = element.value;
          result = { matches: !value, received: value ? "notEmpty" : "empty" };
        } else {
          const text = (_a = element.textContent) == null ? void 0 : _a.trim();
          result = { matches: !text, received: text ? "notEmpty" : "empty" };
        }
      } else if (expression === "to.be.enabled") {
        result = this.elementState(element, "enabled");
      } else if (expression === "to.be.focused") {
        const focused = this._activelyFocused(element).isFocused;
        result = {
          matches: focused,
          received: focused ? "focused" : "inactive"
        };
      } else if (expression === "to.be.hidden") {
        result = this.elementState(element, "hidden", frameVisible);
      } else if (expression === "to.be.visible") {
        result = this.elementState(element, "visible", frameVisible);
      } else if (expression === "to.be.attached") {
        result = {
          matches: true,
          received: "attached"
        };
      } else if (expression === "to.be.detached") {
        result = {
          matches: false,
          received: "attached"
        };
      }
      if (result) {
        if (result.received === "error:notconnected")
          throw this.createStacklessError("Element is not connected");
        return result;
      }
    }
    {
      if (expression === "to.have.property") {
        let target = element;
        const properties = options.expressionArg.split(".");
        for (let i = 0; i < properties.length - 1; i++) {
          if (typeof target !== "object" || !(properties[i] in target))
            return { received: void 0, matches: false };
          target = target[properties[i]];
        }
        const received = target[properties[properties.length - 1]];
        const matches = deepEquals(received, options.expectedValue);
        return { received, matches };
      }
    }
    {
      if (expression === "to.be.in.viewport") {
        const ratio = await this.viewportRatio(element);
        return { received: `viewport ratio ${ratio}`, matches: ratio > 0 && ratio > ((_b = options.expectedNumber) != null ? _b : 0) - 1e-9 };
      }
    }
    {
      if (expression === "to.have.values") {
        element = this.retarget(element, "follow-label");
        if (element.nodeName !== "SELECT" || !element.multiple)
          throw this.createStacklessError("Not a select element with a multiple attribute");
        const received = [...element.selectedOptions].map((o) => o.value);
        if (received.length !== options.expectedText.length)
          return { received, matches: false };
        return { received, matches: received.map((r, i) => new ExpectedTextMatcher(options.expectedText[i]).matches(r)).every(Boolean) };
      }
    }
    {
      if (expression === "to.match.aria") {
        const result = matchesExpectAriaTemplate(element, options.expectedValue);
        return {
          received: result.received,
          matches: !!result.matches.length
        };
      }
    }
    {
      let received;
      if (expression === "to.have.attribute.value") {
        const value = element.getAttribute(options.expressionArg);
        if (value === null)
          return { received: null, matches: false };
        received = value;
      } else if (["to.have.class", "to.contain.class"].includes(expression)) {
        if (!options.expectedText)
          throw this.createStacklessError("Expected text is not provided for " + expression);
        return {
          received: element.classList.toString(),
          matches: new ExpectedTextMatcher(options.expectedText[0]).matchesClassList(
            this,
            element.classList,
            /* partial */
            expression === "to.contain.class"
          )
        };
      } else if (expression === "to.have.css") {
        received = this.window.getComputedStyle(element, options.pseudo ? `::${options.pseudo}` : void 0).getPropertyValue(options.expressionArg);
      } else if (expression === "to.have.id") {
        received = element.id;
      } else if (expression === "to.have.text") {
        received = options.useInnerText ? element.innerText : elementText(/* @__PURE__ */ new Map(), element).full;
      } else if (expression === "to.have.accessible.name") {
        received = getElementAccessibleNameText(
          element,
          false
          /* includeHidden */
        );
      } else if (expression === "to.have.accessible.description") {
        received = getElementAccessibleDescription(
          element,
          false
          /* includeHidden */
        ).text;
      } else if (expression === "to.have.accessible.error.message") {
        received = getElementAccessibleErrorMessage(element);
      } else if (expression === "to.have.role") {
        received = getAriaRole(element) || "";
      } else if (expression === "to.have.value") {
        element = this.retarget(element, "follow-label");
        if (element.nodeName !== "INPUT" && element.nodeName !== "TEXTAREA" && element.nodeName !== "SELECT")
          throw this.createStacklessError("Not an input element");
        received = element.value;
      }
      if (received !== void 0 && options.expectedText) {
        const matcher = new ExpectedTextMatcher(options.expectedText[0]);
        return { received, matches: matcher.matches(received) };
      }
    }
    throw this.createStacklessError("Unknown expect matcher: " + expression);
  }
  expectArray(elements, options) {
    const expression = options.expression;
    if (expression === "to.have.count") {
      const received2 = elements.length;
      const matches2 = received2 === options.expectedNumber;
      return { received: received2, matches: matches2 };
    }
    if (!options.expectedText)
      throw this.createStacklessError("Expected text is not provided for " + expression);
    if (["to.have.class.array", "to.contain.class.array"].includes(expression)) {
      const receivedClassLists = elements.map((e) => e.classList);
      const received2 = receivedClassLists.map(String);
      if (receivedClassLists.length !== options.expectedText.length)
        return { received: received2, matches: false };
      const matches2 = this._matchSequentially(
        options.expectedText,
        receivedClassLists,
        (matcher, r) => matcher.matchesClassList(
          this,
          r,
          /* partial */
          expression === "to.contain.class.array"
        )
      );
      return {
        received: received2,
        matches: matches2
      };
    }
    if (!["to.contain.text.array", "to.have.text.array"].includes(expression))
      throw this.createStacklessError("Unknown expect matcher: " + expression);
    const received = elements.map((e) => options.useInnerText ? e.innerText : elementText(/* @__PURE__ */ new Map(), e).full);
    const lengthShouldMatch = expression !== "to.contain.text.array";
    const matchesLength = received.length === options.expectedText.length || !lengthShouldMatch;
    if (!matchesLength)
      return { received, matches: false };
    const matches = this._matchSequentially(options.expectedText, received, (matcher, r) => matcher.matches(r));
    return { received, matches };
  }
  _matchSequentially(expectedText, received, matchFn) {
    const matchers = expectedText.map((e) => new ExpectedTextMatcher(e));
    let mIndex = 0;
    let rIndex = 0;
    while (mIndex < matchers.length && rIndex < received.length) {
      if (matchFn(matchers[mIndex], received[rIndex]))
        ++mIndex;
      ++rIndex;
    }
    return mIndex === matchers.length;
  }
};
function oneLine(s) {
  return s.replace(/\n/g, "\u21B5").replace(/\t/g, "\u21C6");
}
function createAttributeMatcher(part) {
  const { value, caseSensitive } = part;
  if (value instanceof RegExp) {
    return (s) => {
      value.lastIndex = 0;
      return !!s.match(value);
    };
  }
  if (caseSensitive)
    return (s) => s === value;
  const lowerCaseValue = value.toLowerCase();
  return (s) => s.toLowerCase().includes(lowerCaseValue);
}
function cssUnquote(s) {
  s = s.substring(1, s.length - 1);
  if (!s.includes("\\"))
    return s;
  const r = [];
  let i = 0;
  while (i < s.length) {
    if (s[i] === "\\" && i + 1 < s.length)
      i++;
    r.push(s[i++]);
  }
  return r.join("");
}
function createTextMatcher(selector, internal) {
  if (selector[0] === "/" && selector.lastIndexOf("/") > 0) {
    const lastSlash = selector.lastIndexOf("/");
    const re = new RegExp(selector.substring(1, lastSlash), selector.substring(lastSlash + 1));
    const matcher = (elementText2) => {
      re.lastIndex = 0;
      return re.test(elementText2.full);
    };
    return { matcher, kind: "regex" };
  }
  const unquote = internal ? JSON.parse.bind(JSON) : cssUnquote;
  let strict = false;
  if (selector.length > 1 && selector[0] === '"' && selector[selector.length - 1] === '"') {
    selector = unquote(selector);
    strict = true;
  } else if (internal && selector.length > 1 && selector[0] === '"' && selector[selector.length - 2] === '"' && selector[selector.length - 1] === "i") {
    selector = unquote(selector.substring(0, selector.length - 1));
    strict = false;
  } else if (internal && selector.length > 1 && selector[0] === '"' && selector[selector.length - 2] === '"' && selector[selector.length - 1] === "s") {
    selector = unquote(selector.substring(0, selector.length - 1));
    strict = true;
  } else if (selector.length > 1 && selector[0] === "'" && selector[selector.length - 1] === "'") {
    selector = unquote(selector);
    strict = true;
  }
  selector = normalizeWhiteSpace(selector);
  if (strict) {
    if (internal)
      return { kind: "strict", matcher: (elementText2) => elementText2.normalized === selector };
    const strictTextNodeMatcher = (elementText2) => {
      if (!selector && !elementText2.immediate.length)
        return true;
      return elementText2.immediate.some((s) => normalizeWhiteSpace(s) === selector);
    };
    return { matcher: strictTextNodeMatcher, kind: "strict" };
  }
  selector = selector.toLowerCase();
  return { kind: "lax", matcher: (elementText2) => elementText2.normalized.toLowerCase().includes(selector) };
}
var ExpectedTextMatcher = class {
  constructor(expected) {
    this._normalizeWhiteSpace = expected.normalizeWhiteSpace;
    this._ignoreCase = expected.ignoreCase;
    this._string = expected.matchSubstring ? void 0 : this.normalize(expected.string);
    this._substring = expected.matchSubstring ? this.normalize(expected.string) : void 0;
    if (expected.regexSource) {
      const flags = new Set((expected.regexFlags || "").split(""));
      if (expected.ignoreCase === false)
        flags.delete("i");
      if (expected.ignoreCase === true)
        flags.add("i");
      this._regex = new RegExp(expected.regexSource, [...flags].join(""));
    }
  }
  matches(text) {
    if (!this._regex)
      text = this.normalize(text);
    if (this._string !== void 0)
      return text === this._string;
    if (this._substring !== void 0)
      return text.includes(this._substring);
    if (this._regex) {
      this._regex.lastIndex = 0;
      return !!this._regex.test(text);
    }
    return false;
  }
  matchesClassList(injectedScript, classList, partial) {
    if (partial) {
      if (this._regex)
        throw injectedScript.createStacklessError("Partial matching does not support regular expressions. Please provide a string value.");
      return this._string.split(/\s+/g).filter(Boolean).every((className) => classList.contains(className));
    }
    return this.matches(classList.toString());
  }
  normalize(s) {
    if (!s)
      return s;
    if (this._normalizeWhiteSpace)
      s = normalizeWhiteSpace(s);
    if (this._ignoreCase)
      s = s.toLocaleLowerCase();
    return s;
  }
};
function deepEquals(a, b) {
  if (a === b)
    return true;
  if (a && b && typeof a === "object" && typeof b === "object") {
    if (a.constructor !== b.constructor)
      return false;
    if (Array.isArray(a)) {
      if (a.length !== b.length)
        return false;
      for (let i = 0; i < a.length; ++i) {
        if (!deepEquals(a[i], b[i]))
          return false;
      }
      return true;
    }
    if (a instanceof RegExp)
      return a.source === b.source && a.flags === b.flags;
    if (a.valueOf !== Object.prototype.valueOf)
      return a.valueOf() === b.valueOf();
    if (a.toString !== Object.prototype.toString)
      return a.toString() === b.toString();
    const keys = Object.keys(a);
    if (keys.length !== Object.keys(b).length)
      return false;
    for (let i = 0; i < keys.length; ++i) {
      if (!b.hasOwnProperty(keys[i]))
        return false;
    }
    for (const key of keys) {
      if (!deepEquals(a[key], b[key]))
        return false;
    }
    return true;
  }
  if (typeof a === "number" && typeof b === "number")
    return isNaN(a) && isNaN(b);
  return false;
}

// node_modules/yaml/browser/index.js
var browser_exports = {};
__export(browser_exports, {
  Alias: () => Alias,
  CST: () => cst_exports,
  Composer: () => Composer,
  Document: () => Document,
  Lexer: () => Lexer,
  LineCounter: () => LineCounter,
  Pair: () => Pair,
  Parser: () => Parser,
  Scalar: () => Scalar,
  Schema: () => Schema,
  YAMLError: () => YAMLError,
  YAMLMap: () => YAMLMap,
  YAMLParseError: () => YAMLParseError,
  YAMLSeq: () => YAMLSeq,
  YAMLWarning: () => YAMLWarning,
  default: () => browser_default,
  isAlias: () => isAlias,
  isCollection: () => isCollection,
  isDocument: () => isDocument,
  isMap: () => isMap2,
  isNode: () => isNode,
  isPair: () => isPair,
  isScalar: () => isScalar,
  isSeq: () => isSeq,
  parse: () => parse,
  parseAllDocuments: () => parseAllDocuments,
  parseDocument: () => parseDocument,
  stringify: () => stringify3,
  visit: () => visit,
  visitAsync: () => visitAsync
});

// node_modules/yaml/browser/dist/index.js
var dist_exports = {};
__export(dist_exports, {
  Alias: () => Alias,
  CST: () => cst_exports,
  Composer: () => Composer,
  Document: () => Document,
  Lexer: () => Lexer,
  LineCounter: () => LineCounter,
  Pair: () => Pair,
  Parser: () => Parser,
  Scalar: () => Scalar,
  Schema: () => Schema,
  YAMLError: () => YAMLError,
  YAMLMap: () => YAMLMap,
  YAMLParseError: () => YAMLParseError,
  YAMLSeq: () => YAMLSeq,
  YAMLWarning: () => YAMLWarning,
  isAlias: () => isAlias,
  isCollection: () => isCollection,
  isDocument: () => isDocument,
  isMap: () => isMap2,
  isNode: () => isNode,
  isPair: () => isPair,
  isScalar: () => isScalar,
  isSeq: () => isSeq,
  parse: () => parse,
  parseAllDocuments: () => parseAllDocuments,
  parseDocument: () => parseDocument,
  stringify: () => stringify3,
  visit: () => visit,
  visitAsync: () => visitAsync
});

// node_modules/yaml/browser/dist/nodes/identity.js
var ALIAS = /* @__PURE__ */ Symbol.for("yaml.alias");
var DOC = /* @__PURE__ */ Symbol.for("yaml.document");
var MAP = /* @__PURE__ */ Symbol.for("yaml.map");
var PAIR = /* @__PURE__ */ Symbol.for("yaml.pair");
var SCALAR = /* @__PURE__ */ Symbol.for("yaml.scalar");
var SEQ = /* @__PURE__ */ Symbol.for("yaml.seq");
var NODE_TYPE = /* @__PURE__ */ Symbol.for("yaml.node.type");
var isAlias = (node) => !!node && typeof node === "object" && node[NODE_TYPE] === ALIAS;
var isDocument = (node) => !!node && typeof node === "object" && node[NODE_TYPE] === DOC;
var isMap2 = (node) => !!node && typeof node === "object" && node[NODE_TYPE] === MAP;
var isPair = (node) => !!node && typeof node === "object" && node[NODE_TYPE] === PAIR;
var isScalar = (node) => !!node && typeof node === "object" && node[NODE_TYPE] === SCALAR;
var isSeq = (node) => !!node && typeof node === "object" && node[NODE_TYPE] === SEQ;
function isCollection(node) {
  if (node && typeof node === "object")
    switch (node[NODE_TYPE]) {
      case MAP:
      case SEQ:
        return true;
    }
  return false;
}
function isNode(node) {
  if (node && typeof node === "object")
    switch (node[NODE_TYPE]) {
      case ALIAS:
      case MAP:
      case SCALAR:
      case SEQ:
        return true;
    }
  return false;
}
var hasAnchor = (node) => (isScalar(node) || isCollection(node)) && !!node.anchor;

// node_modules/yaml/browser/dist/visit.js
var BREAK = /* @__PURE__ */ Symbol("break visit");
var SKIP = /* @__PURE__ */ Symbol("skip children");
var REMOVE = /* @__PURE__ */ Symbol("remove node");
function visit(node, visitor) {
  const visitor_ = initVisitor(visitor);
  if (isDocument(node)) {
    const cd = visit_(null, node.contents, visitor_, Object.freeze([node]));
    if (cd === REMOVE)
      node.contents = null;
  } else
    visit_(null, node, visitor_, Object.freeze([]));
}
visit.BREAK = BREAK;
visit.SKIP = SKIP;
visit.REMOVE = REMOVE;
function visit_(key, node, visitor, path) {
  const ctrl = callVisitor(key, node, visitor, path);
  if (isNode(ctrl) || isPair(ctrl)) {
    replaceNode(key, path, ctrl);
    return visit_(key, ctrl, visitor, path);
  }
  if (typeof ctrl !== "symbol") {
    if (isCollection(node)) {
      path = Object.freeze(path.concat(node));
      for (let i = 0; i < node.items.length; ++i) {
        const ci = visit_(i, node.items[i], visitor, path);
        if (typeof ci === "number")
          i = ci - 1;
        else if (ci === BREAK)
          return BREAK;
        else if (ci === REMOVE) {
          node.items.splice(i, 1);
          i -= 1;
        }
      }
    } else if (isPair(node)) {
      path = Object.freeze(path.concat(node));
      const ck = visit_("key", node.key, visitor, path);
      if (ck === BREAK)
        return BREAK;
      else if (ck === REMOVE)
        node.key = null;
      const cv = visit_("value", node.value, visitor, path);
      if (cv === BREAK)
        return BREAK;
      else if (cv === REMOVE)
        node.value = null;
    }
  }
  return ctrl;
}
async function visitAsync(node, visitor) {
  const visitor_ = initVisitor(visitor);
  if (isDocument(node)) {
    const cd = await visitAsync_(null, node.contents, visitor_, Object.freeze([node]));
    if (cd === REMOVE)
      node.contents = null;
  } else
    await visitAsync_(null, node, visitor_, Object.freeze([]));
}
visitAsync.BREAK = BREAK;
visitAsync.SKIP = SKIP;
visitAsync.REMOVE = REMOVE;
async function visitAsync_(key, node, visitor, path) {
  const ctrl = await callVisitor(key, node, visitor, path);
  if (isNode(ctrl) || isPair(ctrl)) {
    replaceNode(key, path, ctrl);
    return visitAsync_(key, ctrl, visitor, path);
  }
  if (typeof ctrl !== "symbol") {
    if (isCollection(node)) {
      path = Object.freeze(path.concat(node));
      for (let i = 0; i < node.items.length; ++i) {
        const ci = await visitAsync_(i, node.items[i], visitor, path);
        if (typeof ci === "number")
          i = ci - 1;
        else if (ci === BREAK)
          return BREAK;
        else if (ci === REMOVE) {
          node.items.splice(i, 1);
          i -= 1;
        }
      }
    } else if (isPair(node)) {
      path = Object.freeze(path.concat(node));
      const ck = await visitAsync_("key", node.key, visitor, path);
      if (ck === BREAK)
        return BREAK;
      else if (ck === REMOVE)
        node.key = null;
      const cv = await visitAsync_("value", node.value, visitor, path);
      if (cv === BREAK)
        return BREAK;
      else if (cv === REMOVE)
        node.value = null;
    }
  }
  return ctrl;
}
function initVisitor(visitor) {
  if (typeof visitor === "object" && (visitor.Collection || visitor.Node || visitor.Value)) {
    return Object.assign({
      Alias: visitor.Node,
      Map: visitor.Node,
      Scalar: visitor.Node,
      Seq: visitor.Node
    }, visitor.Value && {
      Map: visitor.Value,
      Scalar: visitor.Value,
      Seq: visitor.Value
    }, visitor.Collection && {
      Map: visitor.Collection,
      Seq: visitor.Collection
    }, visitor);
  }
  return visitor;
}
function callVisitor(key, node, visitor, path) {
  var _a, _b, _c, _d, _e;
  if (typeof visitor === "function")
    return visitor(key, node, path);
  if (isMap2(node))
    return (_a = visitor.Map) == null ? void 0 : _a.call(visitor, key, node, path);
  if (isSeq(node))
    return (_b = visitor.Seq) == null ? void 0 : _b.call(visitor, key, node, path);
  if (isPair(node))
    return (_c = visitor.Pair) == null ? void 0 : _c.call(visitor, key, node, path);
  if (isScalar(node))
    return (_d = visitor.Scalar) == null ? void 0 : _d.call(visitor, key, node, path);
  if (isAlias(node))
    return (_e = visitor.Alias) == null ? void 0 : _e.call(visitor, key, node, path);
  return void 0;
}
function replaceNode(key, path, node) {
  const parent = path[path.length - 1];
  if (isCollection(parent)) {
    parent.items[key] = node;
  } else if (isPair(parent)) {
    if (key === "key")
      parent.key = node;
    else
      parent.value = node;
  } else if (isDocument(parent)) {
    parent.contents = node;
  } else {
    const pt = isAlias(parent) ? "alias" : "scalar";
    throw new Error(`Cannot replace node with ${pt} parent`);
  }
}

// node_modules/yaml/browser/dist/doc/directives.js
var escapeChars = {
  "!": "%21",
  ",": "%2C",
  "[": "%5B",
  "]": "%5D",
  "{": "%7B",
  "}": "%7D"
};
var escapeTagName = (tn) => tn.replace(/[!,[\]{}]/g, (ch) => escapeChars[ch]);
var Directives = class _Directives {
  constructor(yaml, tags) {
    this.docStart = null;
    this.docEnd = false;
    this.yaml = Object.assign({}, _Directives.defaultYaml, yaml);
    this.tags = Object.assign({}, _Directives.defaultTags, tags);
  }
  clone() {
    const copy = new _Directives(this.yaml, this.tags);
    copy.docStart = this.docStart;
    return copy;
  }
  /**
   * During parsing, get a Directives instance for the current document and
   * update the stream state according to the current version's spec.
   */
  atDocument() {
    const res = new _Directives(this.yaml, this.tags);
    switch (this.yaml.version) {
      case "1.1":
        this.atNextDocument = true;
        break;
      case "1.2":
        this.atNextDocument = false;
        this.yaml = {
          explicit: _Directives.defaultYaml.explicit,
          version: "1.2"
        };
        this.tags = Object.assign({}, _Directives.defaultTags);
        break;
    }
    return res;
  }
  /**
   * @param onError - May be called even if the action was successful
   * @returns `true` on success
   */
  add(line, onError) {
    if (this.atNextDocument) {
      this.yaml = { explicit: _Directives.defaultYaml.explicit, version: "1.1" };
      this.tags = Object.assign({}, _Directives.defaultTags);
      this.atNextDocument = false;
    }
    const parts = line.trim().split(/[ \t]+/);
    const name = parts.shift();
    switch (name) {
      case "%TAG": {
        if (parts.length !== 2) {
          onError(0, "%TAG directive should contain exactly two parts");
          if (parts.length < 2)
            return false;
        }
        const [handle, prefix] = parts;
        this.tags[handle] = prefix;
        return true;
      }
      case "%YAML": {
        this.yaml.explicit = true;
        if (parts.length !== 1) {
          onError(0, "%YAML directive should contain exactly one part");
          return false;
        }
        const [version] = parts;
        if (version === "1.1" || version === "1.2") {
          this.yaml.version = version;
          return true;
        } else {
          const isValid = /^\d+\.\d+$/.test(version);
          onError(6, `Unsupported YAML version ${version}`, isValid);
          return false;
        }
      }
      default:
        onError(0, `Unknown directive ${name}`, true);
        return false;
    }
  }
  /**
   * Resolves a tag, matching handles to those defined in %TAG directives.
   *
   * @returns Resolved tag, which may also be the non-specific tag `'!'` or a
   *   `'!local'` tag, or `null` if unresolvable.
   */
  tagName(source, onError) {
    if (source === "!")
      return "!";
    if (source[0] !== "!") {
      onError(`Not a valid tag: ${source}`);
      return null;
    }
    if (source[1] === "<") {
      const verbatim = source.slice(2, -1);
      if (verbatim === "!" || verbatim === "!!") {
        onError(`Verbatim tags aren't resolved, so ${source} is invalid.`);
        return null;
      }
      if (source[source.length - 1] !== ">")
        onError("Verbatim tags must end with a >");
      return verbatim;
    }
    const [, handle, suffix] = source.match(/^(.*!)([^!]*)$/s);
    if (!suffix)
      onError(`The ${source} tag has no suffix`);
    const prefix = this.tags[handle];
    if (prefix) {
      try {
        return prefix + decodeURIComponent(suffix);
      } catch (error) {
        onError(String(error));
        return null;
      }
    }
    if (handle === "!")
      return source;
    onError(`Could not resolve tag: ${source}`);
    return null;
  }
  /**
   * Given a fully resolved tag, returns its printable string form,
   * taking into account current tag prefixes and defaults.
   */
  tagString(tag) {
    for (const [handle, prefix] of Object.entries(this.tags)) {
      if (tag.startsWith(prefix))
        return handle + escapeTagName(tag.substring(prefix.length));
    }
    return tag[0] === "!" ? tag : `!<${tag}>`;
  }
  toString(doc) {
    const lines = this.yaml.explicit ? [`%YAML ${this.yaml.version || "1.2"}`] : [];
    const tagEntries = Object.entries(this.tags);
    let tagNames;
    if (doc && tagEntries.length > 0 && isNode(doc.contents)) {
      const tags = {};
      visit(doc.contents, (_key, node) => {
        if (isNode(node) && node.tag)
          tags[node.tag] = true;
      });
      tagNames = Object.keys(tags);
    } else
      tagNames = [];
    for (const [handle, prefix] of tagEntries) {
      if (handle === "!!" && prefix === "tag:yaml.org,2002:")
        continue;
      if (!doc || tagNames.some((tn) => tn.startsWith(prefix)))
        lines.push(`%TAG ${handle} ${prefix}`);
    }
    return lines.join("\n");
  }
};
Directives.defaultYaml = { explicit: false, version: "1.2" };
Directives.defaultTags = { "!!": "tag:yaml.org,2002:" };

// node_modules/yaml/browser/dist/doc/anchors.js
function anchorIsValid(anchor) {
  if (/[\x00-\x19\s,[\]{}]/.test(anchor)) {
    const sa = JSON.stringify(anchor);
    const msg = `Anchor must not contain whitespace or control characters: ${sa}`;
    throw new Error(msg);
  }
  return true;
}
function anchorNames(root) {
  const anchors = /* @__PURE__ */ new Set();
  visit(root, {
    Value(_key, node) {
      if (node.anchor)
        anchors.add(node.anchor);
    }
  });
  return anchors;
}
function findNewAnchor(prefix, exclude) {
  for (let i = 1; true; ++i) {
    const name = `${prefix}${i}`;
    if (!exclude.has(name))
      return name;
  }
}
function createNodeAnchors(doc, prefix) {
  const aliasObjects = [];
  const sourceObjects = /* @__PURE__ */ new Map();
  let prevAnchors = null;
  return {
    onAnchor: (source) => {
      aliasObjects.push(source);
      prevAnchors != null ? prevAnchors : prevAnchors = anchorNames(doc);
      const anchor = findNewAnchor(prefix, prevAnchors);
      prevAnchors.add(anchor);
      return anchor;
    },
    /**
     * With circular references, the source node is only resolved after all
     * of its child nodes are. This is why anchors are set only after all of
     * the nodes have been created.
     */
    setAnchors: () => {
      for (const source of aliasObjects) {
        const ref = sourceObjects.get(source);
        if (typeof ref === "object" && ref.anchor && (isScalar(ref.node) || isCollection(ref.node))) {
          ref.node.anchor = ref.anchor;
        } else {
          const error = new Error("Failed to resolve repeated object (this should not happen)");
          error.source = source;
          throw error;
        }
      }
    },
    sourceObjects
  };
}

// node_modules/yaml/browser/dist/doc/applyReviver.js
function applyReviver(reviver, obj, key, val) {
  if (val && typeof val === "object") {
    if (Array.isArray(val)) {
      for (let i = 0, len = val.length; i < len; ++i) {
        const v0 = val[i];
        const v1 = applyReviver(reviver, val, String(i), v0);
        if (v1 === void 0)
          delete val[i];
        else if (v1 !== v0)
          val[i] = v1;
      }
    } else if (val instanceof Map) {
      for (const k of Array.from(val.keys())) {
        const v0 = val.get(k);
        const v1 = applyReviver(reviver, val, k, v0);
        if (v1 === void 0)
          val.delete(k);
        else if (v1 !== v0)
          val.set(k, v1);
      }
    } else if (val instanceof Set) {
      for (const v0 of Array.from(val)) {
        const v1 = applyReviver(reviver, val, v0, v0);
        if (v1 === void 0)
          val.delete(v0);
        else if (v1 !== v0) {
          val.delete(v0);
          val.add(v1);
        }
      }
    } else {
      for (const [k, v0] of Object.entries(val)) {
        const v1 = applyReviver(reviver, val, k, v0);
        if (v1 === void 0)
          delete val[k];
        else if (v1 !== v0)
          val[k] = v1;
      }
    }
  }
  return reviver.call(obj, key, val);
}

// node_modules/yaml/browser/dist/nodes/toJS.js
function toJS(value, arg, ctx) {
  if (Array.isArray(value))
    return value.map((v, i) => toJS(v, String(i), ctx));
  if (value && typeof value.toJSON === "function") {
    if (!ctx || !hasAnchor(value))
      return value.toJSON(arg, ctx);
    const data = { aliasCount: 0, count: 1, res: void 0 };
    ctx.anchors.set(value, data);
    ctx.onCreate = (res2) => {
      data.res = res2;
      delete ctx.onCreate;
    };
    const res = value.toJSON(arg, ctx);
    if (ctx.onCreate)
      ctx.onCreate(res);
    return res;
  }
  if (typeof value === "bigint" && !(ctx == null ? void 0 : ctx.keep))
    return Number(value);
  return value;
}

// node_modules/yaml/browser/dist/nodes/Node.js
var NodeBase = class {
  constructor(type) {
    Object.defineProperty(this, NODE_TYPE, { value: type });
  }
  /** Create a copy of this node.  */
  clone() {
    const copy = Object.create(Object.getPrototypeOf(this), Object.getOwnPropertyDescriptors(this));
    if (this.range)
      copy.range = this.range.slice();
    return copy;
  }
  /** A plain JavaScript representation of this node. */
  toJS(doc, { mapAsMap, maxAliasCount, onAnchor, reviver } = {}) {
    if (!isDocument(doc))
      throw new TypeError("A document argument is required");
    const ctx = {
      anchors: /* @__PURE__ */ new Map(),
      doc,
      keep: true,
      mapAsMap: mapAsMap === true,
      mapKeyWarned: false,
      maxAliasCount: typeof maxAliasCount === "number" ? maxAliasCount : 100
    };
    const res = toJS(this, "", ctx);
    if (typeof onAnchor === "function")
      for (const { count, res: res2 } of ctx.anchors.values())
        onAnchor(res2, count);
    return typeof reviver === "function" ? applyReviver(reviver, { "": res }, "", res) : res;
  }
};

// node_modules/yaml/browser/dist/nodes/Alias.js
var Alias = class extends NodeBase {
  constructor(source) {
    super(ALIAS);
    this.source = source;
    Object.defineProperty(this, "tag", {
      set() {
        throw new Error("Alias nodes cannot have tags");
      }
    });
  }
  /**
   * Resolve the value of this alias within `doc`, finding the last
   * instance of the `source` anchor before this node.
   */
  resolve(doc, ctx) {
    if ((ctx == null ? void 0 : ctx.maxAliasCount) === 0)
      throw new ReferenceError("Alias resolution is disabled");
    let nodes;
    if (ctx == null ? void 0 : ctx.aliasResolveCache) {
      nodes = ctx.aliasResolveCache;
    } else {
      nodes = [];
      visit(doc, {
        Node: (_key, node) => {
          if (isAlias(node) || hasAnchor(node))
            nodes.push(node);
        }
      });
      if (ctx)
        ctx.aliasResolveCache = nodes;
    }
    let found = void 0;
    for (const node of nodes) {
      if (node === this)
        break;
      if (node.anchor === this.source)
        found = node;
    }
    return found;
  }
  toJSON(_arg, ctx) {
    if (!ctx)
      return { source: this.source };
    const { anchors, doc, maxAliasCount } = ctx;
    const source = this.resolve(doc, ctx);
    if (!source) {
      const msg = `Unresolved alias (the anchor must be set before the alias): ${this.source}`;
      throw new ReferenceError(msg);
    }
    let data = anchors.get(source);
    if (!data) {
      toJS(source, null, ctx);
      data = anchors.get(source);
    }
    if ((data == null ? void 0 : data.res) === void 0) {
      const msg = "This should not happen: Alias anchor was not resolved?";
      throw new ReferenceError(msg);
    }
    if (maxAliasCount >= 0) {
      data.count += 1;
      if (data.aliasCount === 0)
        data.aliasCount = getAliasCount(doc, source, anchors);
      if (data.count * data.aliasCount > maxAliasCount) {
        const msg = "Excessive alias count indicates a resource exhaustion attack";
        throw new ReferenceError(msg);
      }
    }
    return data.res;
  }
  toString(ctx, _onComment, _onChompKeep) {
    const src = `*${this.source}`;
    if (ctx) {
      anchorIsValid(this.source);
      if (ctx.options.verifyAliasOrder && !ctx.anchors.has(this.source)) {
        const msg = `Unresolved alias (the anchor must be set before the alias): ${this.source}`;
        throw new Error(msg);
      }
      if (ctx.implicitKey)
        return `${src} `;
    }
    return src;
  }
};
function getAliasCount(doc, node, anchors) {
  if (isAlias(node)) {
    const source = node.resolve(doc);
    const anchor = anchors && source && anchors.get(source);
    return anchor ? anchor.count * anchor.aliasCount : 0;
  } else if (isCollection(node)) {
    let count = 0;
    for (const item of node.items) {
      const c = getAliasCount(doc, item, anchors);
      if (c > count)
        count = c;
    }
    return count;
  } else if (isPair(node)) {
    const kc = getAliasCount(doc, node.key, anchors);
    const vc = getAliasCount(doc, node.value, anchors);
    return Math.max(kc, vc);
  }
  return 1;
}

// node_modules/yaml/browser/dist/nodes/Scalar.js
var isScalarValue = (value) => !value || typeof value !== "function" && typeof value !== "object";
var Scalar = class extends NodeBase {
  constructor(value) {
    super(SCALAR);
    this.value = value;
  }
  toJSON(arg, ctx) {
    return (ctx == null ? void 0 : ctx.keep) ? this.value : toJS(this.value, arg, ctx);
  }
  toString() {
    return String(this.value);
  }
};
Scalar.BLOCK_FOLDED = "BLOCK_FOLDED";
Scalar.BLOCK_LITERAL = "BLOCK_LITERAL";
Scalar.PLAIN = "PLAIN";
Scalar.QUOTE_DOUBLE = "QUOTE_DOUBLE";
Scalar.QUOTE_SINGLE = "QUOTE_SINGLE";

// node_modules/yaml/browser/dist/doc/createNode.js
var defaultTagPrefix = "tag:yaml.org,2002:";
function findTagObject(value, tagName, tags) {
  var _a;
  if (tagName) {
    const match = tags.filter((t) => t.tag === tagName);
    const tagObj = (_a = match.find((t) => !t.format)) != null ? _a : match[0];
    if (!tagObj)
      throw new Error(`Tag ${tagName} not found`);
    return tagObj;
  }
  return tags.find((t) => {
    var _a2;
    return ((_a2 = t.identify) == null ? void 0 : _a2.call(t, value)) && !t.format;
  });
}
function createNode(value, tagName, ctx) {
  var _a, _b, _c, _d;
  if (isDocument(value))
    value = value.contents;
  if (isNode(value))
    return value;
  if (isPair(value)) {
    const map2 = (_b = (_a = ctx.schema[MAP]).createNode) == null ? void 0 : _b.call(_a, ctx.schema, null, ctx);
    map2.items.push(value);
    return map2;
  }
  if (value instanceof String || value instanceof Number || value instanceof Boolean || typeof BigInt !== "undefined" && value instanceof BigInt) {
    value = value.valueOf();
  }
  const { aliasDuplicateObjects, onAnchor, onTagObj, schema: schema4, sourceObjects } = ctx;
  let ref = void 0;
  if (aliasDuplicateObjects && value && typeof value === "object") {
    ref = sourceObjects.get(value);
    if (ref) {
      (_c = ref.anchor) != null ? _c : ref.anchor = onAnchor(value);
      return new Alias(ref.anchor);
    } else {
      ref = { anchor: null, node: null };
      sourceObjects.set(value, ref);
    }
  }
  if (tagName == null ? void 0 : tagName.startsWith("!!"))
    tagName = defaultTagPrefix + tagName.slice(2);
  let tagObj = findTagObject(value, tagName, schema4.tags);
  if (!tagObj) {
    if (value && typeof value.toJSON === "function") {
      value = value.toJSON();
    }
    if (!value || typeof value !== "object") {
      const node2 = new Scalar(value);
      if (ref)
        ref.node = node2;
      return node2;
    }
    tagObj = value instanceof Map ? schema4[MAP] : Symbol.iterator in Object(value) ? schema4[SEQ] : schema4[MAP];
  }
  if (onTagObj) {
    onTagObj(tagObj);
    delete ctx.onTagObj;
  }
  const node = (tagObj == null ? void 0 : tagObj.createNode) ? tagObj.createNode(ctx.schema, value, ctx) : typeof ((_d = tagObj == null ? void 0 : tagObj.nodeClass) == null ? void 0 : _d.from) === "function" ? tagObj.nodeClass.from(ctx.schema, value, ctx) : new Scalar(value);
  if (tagName)
    node.tag = tagName;
  else if (!tagObj.default)
    node.tag = tagObj.tag;
  if (ref)
    ref.node = node;
  return node;
}

// node_modules/yaml/browser/dist/nodes/Collection.js
function collectionFromPath(schema4, path, value) {
  let v = value;
  for (let i = path.length - 1; i >= 0; --i) {
    const k = path[i];
    if (typeof k === "number" && Number.isInteger(k) && k >= 0) {
      const a = [];
      a[k] = v;
      v = a;
    } else {
      v = /* @__PURE__ */ new Map([[k, v]]);
    }
  }
  return createNode(v, void 0, {
    aliasDuplicateObjects: false,
    keepUndefined: false,
    onAnchor: () => {
      throw new Error("This should not happen, please report a bug.");
    },
    schema: schema4,
    sourceObjects: /* @__PURE__ */ new Map()
  });
}
var isEmptyPath = (path) => path == null || typeof path === "object" && !!path[Symbol.iterator]().next().done;
var Collection = class extends NodeBase {
  constructor(type, schema4) {
    super(type);
    Object.defineProperty(this, "schema", {
      value: schema4,
      configurable: true,
      enumerable: false,
      writable: true
    });
  }
  /**
   * Create a copy of this collection.
   *
   * @param schema - If defined, overwrites the original's schema
   */
  clone(schema4) {
    const copy = Object.create(Object.getPrototypeOf(this), Object.getOwnPropertyDescriptors(this));
    if (schema4)
      copy.schema = schema4;
    copy.items = copy.items.map((it) => isNode(it) || isPair(it) ? it.clone(schema4) : it);
    if (this.range)
      copy.range = this.range.slice();
    return copy;
  }
  /**
   * Adds a value to the collection. For `!!map` and `!!omap` the value must
   * be a Pair instance or a `{ key, value }` object, which may not have a key
   * that already exists in the map.
   */
  addIn(path, value) {
    if (isEmptyPath(path))
      this.add(value);
    else {
      const [key, ...rest] = path;
      const node = this.get(key, true);
      if (isCollection(node))
        node.addIn(rest, value);
      else if (node === void 0 && this.schema)
        this.set(key, collectionFromPath(this.schema, rest, value));
      else
        throw new Error(`Expected YAML collection at ${key}. Remaining path: ${rest}`);
    }
  }
  /**
   * Removes a value from the collection.
   * @returns `true` if the item was found and removed.
   */
  deleteIn(path) {
    const [key, ...rest] = path;
    if (rest.length === 0)
      return this.delete(key);
    const node = this.get(key, true);
    if (isCollection(node))
      return node.deleteIn(rest);
    else
      throw new Error(`Expected YAML collection at ${key}. Remaining path: ${rest}`);
  }
  /**
   * Returns item at `key`, or `undefined` if not found. By default unwraps
   * scalar values from their surrounding node; to disable set `keepScalar` to
   * `true` (collections are always returned intact).
   */
  getIn(path, keepScalar) {
    const [key, ...rest] = path;
    const node = this.get(key, true);
    if (rest.length === 0)
      return !keepScalar && isScalar(node) ? node.value : node;
    else
      return isCollection(node) ? node.getIn(rest, keepScalar) : void 0;
  }
  hasAllNullValues(allowScalar) {
    return this.items.every((node) => {
      if (!isPair(node))
        return false;
      const n = node.value;
      return n == null || allowScalar && isScalar(n) && n.value == null && !n.commentBefore && !n.comment && !n.tag;
    });
  }
  /**
   * Checks if the collection includes a value with the key `key`.
   */
  hasIn(path) {
    const [key, ...rest] = path;
    if (rest.length === 0)
      return this.has(key);
    const node = this.get(key, true);
    return isCollection(node) ? node.hasIn(rest) : false;
  }
  /**
   * Sets a value in this collection. For `!!set`, `value` needs to be a
   * boolean to add/remove the item from the set.
   */
  setIn(path, value) {
    const [key, ...rest] = path;
    if (rest.length === 0) {
      this.set(key, value);
    } else {
      const node = this.get(key, true);
      if (isCollection(node))
        node.setIn(rest, value);
      else if (node === void 0 && this.schema)
        this.set(key, collectionFromPath(this.schema, rest, value));
      else
        throw new Error(`Expected YAML collection at ${key}. Remaining path: ${rest}`);
    }
  }
};

// node_modules/yaml/browser/dist/stringify/stringifyComment.js
var stringifyComment = (str) => str.replace(/^(?!$)(?: $)?/gm, "#");
function indentComment(comment, indent2) {
  if (/^\n+$/.test(comment))
    return comment.substring(1);
  return indent2 ? comment.replace(/^(?! *$)/gm, indent2) : comment;
}
var lineComment = (str, indent2, comment) => str.endsWith("\n") ? indentComment(comment, indent2) : comment.includes("\n") ? "\n" + indentComment(comment, indent2) : (str.endsWith(" ") ? "" : " ") + comment;

// node_modules/yaml/browser/dist/stringify/foldFlowLines.js
var FOLD_FLOW = "flow";
var FOLD_BLOCK = "block";
var FOLD_QUOTED = "quoted";
function foldFlowLines(text, indent2, mode = "flow", { indentAtStart, lineWidth = 80, minContentWidth = 20, onFold, onOverflow } = {}) {
  if (!lineWidth || lineWidth < 0)
    return text;
  if (lineWidth < minContentWidth)
    minContentWidth = 0;
  const endStep = Math.max(1 + minContentWidth, 1 + lineWidth - indent2.length);
  if (text.length <= endStep)
    return text;
  const folds = [];
  const escapedFolds = {};
  let end = lineWidth - indent2.length;
  if (typeof indentAtStart === "number") {
    if (indentAtStart > lineWidth - Math.max(2, minContentWidth))
      folds.push(0);
    else
      end = lineWidth - indentAtStart;
  }
  let split = void 0;
  let prev = void 0;
  let overflow = false;
  let i = -1;
  let escStart = -1;
  let escEnd = -1;
  if (mode === FOLD_BLOCK) {
    i = consumeMoreIndentedLines(text, i, indent2.length);
    if (i !== -1)
      end = i + endStep;
  }
  for (let ch; ch = text[i += 1]; ) {
    if (mode === FOLD_QUOTED && ch === "\\") {
      escStart = i;
      switch (text[i + 1]) {
        case "x":
          i += 3;
          break;
        case "u":
          i += 5;
          break;
        case "U":
          i += 9;
          break;
        default:
          i += 1;
      }
      escEnd = i;
    }
    if (ch === "\n") {
      if (mode === FOLD_BLOCK)
        i = consumeMoreIndentedLines(text, i, indent2.length);
      end = i + indent2.length + endStep;
      split = void 0;
    } else {
      if (ch === " " && prev && prev !== " " && prev !== "\n" && prev !== "	") {
        const next = text[i + 1];
        if (next && next !== " " && next !== "\n" && next !== "	")
          split = i;
      }
      if (i >= end) {
        if (split) {
          folds.push(split);
          end = split + endStep;
          split = void 0;
        } else if (mode === FOLD_QUOTED) {
          while (prev === " " || prev === "	") {
            prev = ch;
            ch = text[i += 1];
            overflow = true;
          }
          const j = i > escEnd + 1 ? i - 2 : escStart - 1;
          if (escapedFolds[j])
            return text;
          folds.push(j);
          escapedFolds[j] = true;
          end = j + endStep;
          split = void 0;
        } else {
          overflow = true;
        }
      }
    }
    prev = ch;
  }
  if (overflow && onOverflow)
    onOverflow();
  if (folds.length === 0)
    return text;
  if (onFold)
    onFold();
  let res = text.slice(0, folds[0]);
  for (let i2 = 0; i2 < folds.length; ++i2) {
    const fold = folds[i2];
    const end2 = folds[i2 + 1] || text.length;
    if (fold === 0)
      res = `
${indent2}${text.slice(0, end2)}`;
    else {
      if (mode === FOLD_QUOTED && escapedFolds[fold])
        res += `${text[fold]}\\`;
      res += `
${indent2}${text.slice(fold + 1, end2)}`;
    }
  }
  return res;
}
function consumeMoreIndentedLines(text, i, indent2) {
  let end = i;
  let start = i + 1;
  let ch = text[start];
  while (ch === " " || ch === "	") {
    if (i < start + indent2) {
      ch = text[++i];
    } else {
      do {
        ch = text[++i];
      } while (ch && ch !== "\n");
      end = i;
      start = i + 1;
      ch = text[start];
    }
  }
  return end;
}

// node_modules/yaml/browser/dist/stringify/stringifyString.js
var getFoldOptions = (ctx, isBlock2) => ({
  indentAtStart: isBlock2 ? ctx.indent.length : ctx.indentAtStart,
  lineWidth: ctx.options.lineWidth,
  minContentWidth: ctx.options.minContentWidth
});
var containsDocumentMarker = (str) => /^(%|---|\.\.\.)/m.test(str);
function lineLengthOverLimit(str, lineWidth, indentLength) {
  if (!lineWidth || lineWidth < 0)
    return false;
  const limit = lineWidth - indentLength;
  const strLen = str.length;
  if (strLen <= limit)
    return false;
  for (let i = 0, start = 0; i < strLen; ++i) {
    if (str[i] === "\n") {
      if (i - start > limit)
        return true;
      start = i + 1;
      if (strLen - start <= limit)
        return false;
    }
  }
  return true;
}
function doubleQuotedString(value, ctx) {
  const json = JSON.stringify(value);
  if (ctx.options.doubleQuotedAsJSON)
    return json;
  const { implicitKey } = ctx;
  const minMultiLineLength = ctx.options.doubleQuotedMinMultiLineLength;
  const indent2 = ctx.indent || (containsDocumentMarker(value) ? "  " : "");
  let str = "";
  let start = 0;
  for (let i = 0, ch = json[i]; ch; ch = json[++i]) {
    if (ch === " " && json[i + 1] === "\\" && json[i + 2] === "n") {
      str += json.slice(start, i) + "\\ ";
      i += 1;
      start = i;
      ch = "\\";
    }
    if (ch === "\\")
      switch (json[i + 1]) {
        case "u":
          {
            str += json.slice(start, i);
            const code = json.substr(i + 2, 4);
            switch (code) {
              case "0000":
                str += "\\0";
                break;
              case "0007":
                str += "\\a";
                break;
              case "000b":
                str += "\\v";
                break;
              case "001b":
                str += "\\e";
                break;
              case "0085":
                str += "\\N";
                break;
              case "00a0":
                str += "\\_";
                break;
              case "2028":
                str += "\\L";
                break;
              case "2029":
                str += "\\P";
                break;
              default:
                if (code.substr(0, 2) === "00")
                  str += "\\x" + code.substr(2);
                else
                  str += json.substr(i, 6);
            }
            i += 5;
            start = i + 1;
          }
          break;
        case "n":
          if (implicitKey || json[i + 2] === '"' || json.length < minMultiLineLength) {
            i += 1;
          } else {
            str += json.slice(start, i) + "\n\n";
            while (json[i + 2] === "\\" && json[i + 3] === "n" && json[i + 4] !== '"') {
              str += "\n";
              i += 2;
            }
            str += indent2;
            if (json[i + 2] === " ")
              str += "\\";
            i += 1;
            start = i + 1;
          }
          break;
        default:
          i += 1;
      }
  }
  str = start ? str + json.slice(start) : json;
  return implicitKey ? str : foldFlowLines(str, indent2, FOLD_QUOTED, getFoldOptions(ctx, false));
}
function singleQuotedString(value, ctx) {
  if (ctx.options.singleQuote === false || ctx.implicitKey && value.includes("\n") || /[ \t]\n|\n[ \t]/.test(value))
    return doubleQuotedString(value, ctx);
  const indent2 = ctx.indent || (containsDocumentMarker(value) ? "  " : "");
  const res = "'" + value.replace(/'/g, "''").replace(/\n+/g, `$&
${indent2}`) + "'";
  return ctx.implicitKey ? res : foldFlowLines(res, indent2, FOLD_FLOW, getFoldOptions(ctx, false));
}
function quotedString(value, ctx) {
  const { singleQuote } = ctx.options;
  let qs;
  if (singleQuote === false)
    qs = doubleQuotedString;
  else {
    const hasDouble = value.includes('"');
    const hasSingle = value.includes("'");
    if (hasDouble && !hasSingle)
      qs = singleQuotedString;
    else if (hasSingle && !hasDouble)
      qs = doubleQuotedString;
    else
      qs = singleQuote ? singleQuotedString : doubleQuotedString;
  }
  return qs(value, ctx);
}
var blockEndNewlines;
try {
  blockEndNewlines = new RegExp("(^|(?<!\n))\n+(?!\n|$)", "g");
} catch {
  blockEndNewlines = /\n+(?!\n|$)/g;
}
function blockString({ comment, type, value }, ctx, onComment, onChompKeep) {
  const { blockQuote, commentString, lineWidth } = ctx.options;
  if (!blockQuote || /\n[\t ]+$/.test(value)) {
    return quotedString(value, ctx);
  }
  const indent2 = ctx.indent || (ctx.forceBlockIndent || containsDocumentMarker(value) ? "  " : "");
  const literal = blockQuote === "literal" ? true : blockQuote === "folded" || type === Scalar.BLOCK_FOLDED ? false : type === Scalar.BLOCK_LITERAL ? true : !lineLengthOverLimit(value, lineWidth, indent2.length);
  if (!value)
    return literal ? "|\n" : ">\n";
  let chomp;
  let endStart;
  for (endStart = value.length; endStart > 0; --endStart) {
    const ch = value[endStart - 1];
    if (ch !== "\n" && ch !== "	" && ch !== " ")
      break;
  }
  let end = value.substring(endStart);
  const endNlPos = end.indexOf("\n");
  if (endNlPos === -1) {
    chomp = "-";
  } else if (value === end || endNlPos !== end.length - 1) {
    chomp = "+";
    if (onChompKeep)
      onChompKeep();
  } else {
    chomp = "";
  }
  if (end) {
    value = value.slice(0, -end.length);
    if (end[end.length - 1] === "\n")
      end = end.slice(0, -1);
    end = end.replace(blockEndNewlines, `$&${indent2}`);
  }
  let startWithSpace = false;
  let startEnd;
  let startNlPos = -1;
  for (startEnd = 0; startEnd < value.length; ++startEnd) {
    const ch = value[startEnd];
    if (ch === " ")
      startWithSpace = true;
    else if (ch === "\n")
      startNlPos = startEnd;
    else
      break;
  }
  let start = value.substring(0, startNlPos < startEnd ? startNlPos + 1 : startEnd);
  if (start) {
    value = value.substring(start.length);
    start = start.replace(/\n+/g, `$&${indent2}`);
  }
  const indentSize = indent2 ? "2" : "1";
  let header = (startWithSpace ? indentSize : "") + chomp;
  if (comment) {
    header += " " + commentString(comment.replace(/ ?[\r\n]+/g, " "));
    if (onComment)
      onComment();
  }
  if (!literal) {
    const foldedValue = value.replace(/\n+/g, "\n$&").replace(/(?:^|\n)([\t ].*)(?:([\n\t ]*)\n(?![\n\t ]))?/g, "$1$2").replace(/\n+/g, `$&${indent2}`);
    let literalFallback = false;
    const foldOptions = getFoldOptions(ctx, true);
    if (blockQuote !== "folded" && type !== Scalar.BLOCK_FOLDED) {
      foldOptions.onOverflow = () => {
        literalFallback = true;
      };
    }
    const body = foldFlowLines(`${start}${foldedValue}${end}`, indent2, FOLD_BLOCK, foldOptions);
    if (!literalFallback)
      return `>${header}
${indent2}${body}`;
  }
  value = value.replace(/\n+/g, `$&${indent2}`);
  return `|${header}
${indent2}${start}${value}${end}`;
}
function plainString(item, ctx, onComment, onChompKeep) {
  const { type, value } = item;
  const { actualString, implicitKey, indent: indent2, indentStep, inFlow } = ctx;
  if (implicitKey && value.includes("\n") || inFlow && /[[\]{},]/.test(value)) {
    return quotedString(value, ctx);
  }
  if (/^[\n\t ,[\]{}#&*!|>'"%@`]|^[?-]$|^[?-][ \t]|[\n:][ \t]|[ \t]\n|[\n\t ]#|[\n\t :]$/.test(value)) {
    return implicitKey || inFlow || !value.includes("\n") ? quotedString(value, ctx) : blockString(item, ctx, onComment, onChompKeep);
  }
  if (!implicitKey && !inFlow && type !== Scalar.PLAIN && value.includes("\n")) {
    return blockString(item, ctx, onComment, onChompKeep);
  }
  if (containsDocumentMarker(value)) {
    if (indent2 === "") {
      ctx.forceBlockIndent = true;
      return blockString(item, ctx, onComment, onChompKeep);
    } else if (implicitKey && indent2 === indentStep) {
      return quotedString(value, ctx);
    }
  }
  const str = value.replace(/\n+/g, `$&
${indent2}`);
  if (actualString) {
    const test = (tag) => {
      var _a;
      return tag.default && tag.tag !== "tag:yaml.org,2002:str" && ((_a = tag.test) == null ? void 0 : _a.test(str));
    };
    const { compat, tags } = ctx.doc.schema;
    if (tags.some(test) || (compat == null ? void 0 : compat.some(test)))
      return quotedString(value, ctx);
  }
  return implicitKey ? str : foldFlowLines(str, indent2, FOLD_FLOW, getFoldOptions(ctx, false));
}
function stringifyString(item, ctx, onComment, onChompKeep) {
  const { implicitKey, inFlow } = ctx;
  const ss = typeof item.value === "string" ? item : Object.assign({}, item, { value: String(item.value) });
  let { type } = item;
  if (type !== Scalar.QUOTE_DOUBLE) {
    if (/[\x00-\x08\x0b-\x1f\x7f-\x9f\u{D800}-\u{DFFF}]/u.test(ss.value))
      type = Scalar.QUOTE_DOUBLE;
  }
  const _stringify = (_type) => {
    switch (_type) {
      case Scalar.BLOCK_FOLDED:
      case Scalar.BLOCK_LITERAL:
        return implicitKey || inFlow ? quotedString(ss.value, ctx) : blockString(ss, ctx, onComment, onChompKeep);
      case Scalar.QUOTE_DOUBLE:
        return doubleQuotedString(ss.value, ctx);
      case Scalar.QUOTE_SINGLE:
        return singleQuotedString(ss.value, ctx);
      case Scalar.PLAIN:
        return plainString(ss, ctx, onComment, onChompKeep);
      default:
        return null;
    }
  };
  let res = _stringify(type);
  if (res === null) {
    const { defaultKeyType, defaultStringType } = ctx.options;
    const t = implicitKey && defaultKeyType || defaultStringType;
    res = _stringify(t);
    if (res === null)
      throw new Error(`Unsupported default string type ${t}`);
  }
  return res;
}

// node_modules/yaml/browser/dist/stringify/stringify.js
function createStringifyContext(doc, options) {
  const opt = Object.assign({
    blockQuote: true,
    commentString: stringifyComment,
    defaultKeyType: null,
    defaultStringType: "PLAIN",
    directives: null,
    doubleQuotedAsJSON: false,
    doubleQuotedMinMultiLineLength: 40,
    falseStr: "false",
    flowCollectionPadding: true,
    indentSeq: true,
    lineWidth: 80,
    minContentWidth: 20,
    nullStr: "null",
    simpleKeys: false,
    singleQuote: null,
    trailingComma: false,
    trueStr: "true",
    verifyAliasOrder: true
  }, doc.schema.toStringOptions, options);
  let inFlow;
  switch (opt.collectionStyle) {
    case "block":
      inFlow = false;
      break;
    case "flow":
      inFlow = true;
      break;
    default:
      inFlow = null;
  }
  return {
    anchors: /* @__PURE__ */ new Set(),
    doc,
    flowCollectionPadding: opt.flowCollectionPadding ? " " : "",
    indent: "",
    indentStep: typeof opt.indent === "number" ? " ".repeat(opt.indent) : "  ",
    inFlow,
    options: opt
  };
}
function getTagObject(tags, item) {
  var _a, _b, _c, _d;
  if (item.tag) {
    const match = tags.filter((t) => t.tag === item.tag);
    if (match.length > 0)
      return (_a = match.find((t) => t.format === item.format)) != null ? _a : match[0];
  }
  let tagObj = void 0;
  let obj;
  if (isScalar(item)) {
    obj = item.value;
    let match = tags.filter((t) => {
      var _a2;
      return (_a2 = t.identify) == null ? void 0 : _a2.call(t, obj);
    });
    if (match.length > 1) {
      const testMatch = match.filter((t) => t.test);
      if (testMatch.length > 0)
        match = testMatch;
    }
    tagObj = (_b = match.find((t) => t.format === item.format)) != null ? _b : match.find((t) => !t.format);
  } else {
    obj = item;
    tagObj = tags.find((t) => t.nodeClass && obj instanceof t.nodeClass);
  }
  if (!tagObj) {
    const name = (_d = (_c = obj == null ? void 0 : obj.constructor) == null ? void 0 : _c.name) != null ? _d : obj === null ? "null" : typeof obj;
    throw new Error(`Tag not resolved for ${name} value`);
  }
  return tagObj;
}
function stringifyProps(node, tagObj, { anchors, doc }) {
  var _a;
  if (!doc.directives)
    return "";
  const props = [];
  const anchor = (isScalar(node) || isCollection(node)) && node.anchor;
  if (anchor && anchorIsValid(anchor)) {
    anchors.add(anchor);
    props.push(`&${anchor}`);
  }
  const tag = (_a = node.tag) != null ? _a : tagObj.default ? null : tagObj.tag;
  if (tag)
    props.push(doc.directives.tagString(tag));
  return props.join(" ");
}
function stringify(item, ctx, onComment, onChompKeep) {
  var _a, _b;
  if (isPair(item))
    return item.toString(ctx, onComment, onChompKeep);
  if (isAlias(item)) {
    if (ctx.doc.directives)
      return item.toString(ctx);
    if ((_a = ctx.resolvedAliases) == null ? void 0 : _a.has(item)) {
      throw new TypeError(`Cannot stringify circular structure without alias nodes`);
    } else {
      if (ctx.resolvedAliases)
        ctx.resolvedAliases.add(item);
      else
        ctx.resolvedAliases = /* @__PURE__ */ new Set([item]);
      item = item.resolve(ctx.doc);
    }
  }
  let tagObj = void 0;
  const node = isNode(item) ? item : ctx.doc.createNode(item, { onTagObj: (o) => tagObj = o });
  tagObj != null ? tagObj : tagObj = getTagObject(ctx.doc.schema.tags, node);
  const props = stringifyProps(node, tagObj, ctx);
  if (props.length > 0)
    ctx.indentAtStart = ((_b = ctx.indentAtStart) != null ? _b : 0) + props.length + 1;
  const str = typeof tagObj.stringify === "function" ? tagObj.stringify(node, ctx, onComment, onChompKeep) : isScalar(node) ? stringifyString(node, ctx, onComment, onChompKeep) : node.toString(ctx, onComment, onChompKeep);
  if (!props)
    return str;
  return isScalar(node) || str[0] === "{" || str[0] === "[" ? `${props} ${str}` : `${props}
${ctx.indent}${str}`;
}

// node_modules/yaml/browser/dist/stringify/stringifyPair.js
function stringifyPair({ key, value }, ctx, onComment, onChompKeep) {
  var _a, _b;
  const { allNullValues, doc, indent: indent2, indentStep, options: { commentString, indentSeq, simpleKeys } } = ctx;
  let keyComment = isNode(key) && key.comment || null;
  if (simpleKeys) {
    if (keyComment) {
      throw new Error("With simple keys, key nodes cannot have comments");
    }
    if (isCollection(key) || !isNode(key) && typeof key === "object") {
      const msg = "With simple keys, collection cannot be used as a key value";
      throw new Error(msg);
    }
  }
  let explicitKey = !simpleKeys && (!key || keyComment && value == null && !ctx.inFlow || isCollection(key) || (isScalar(key) ? key.type === Scalar.BLOCK_FOLDED || key.type === Scalar.BLOCK_LITERAL : typeof key === "object"));
  ctx = Object.assign({}, ctx, {
    allNullValues: false,
    implicitKey: !explicitKey && (simpleKeys || !allNullValues),
    indent: indent2 + indentStep
  });
  let keyCommentDone = false;
  let chompKeep = false;
  let str = stringify(key, ctx, () => keyCommentDone = true, () => chompKeep = true);
  if (!explicitKey && !ctx.inFlow && str.length > 1024) {
    if (simpleKeys)
      throw new Error("With simple keys, single line scalar must not span more than 1024 characters");
    explicitKey = true;
  }
  if (ctx.inFlow) {
    if (allNullValues || value == null) {
      if (keyCommentDone && onComment)
        onComment();
      return str === "" ? "?" : explicitKey ? `? ${str}` : str;
    }
  } else if (allNullValues && !simpleKeys || value == null && explicitKey) {
    str = `? ${str}`;
    if (keyComment && !keyCommentDone) {
      str += lineComment(str, ctx.indent, commentString(keyComment));
    } else if (chompKeep && onChompKeep)
      onChompKeep();
    return str;
  }
  if (keyCommentDone)
    keyComment = null;
  if (explicitKey) {
    if (keyComment)
      str += lineComment(str, ctx.indent, commentString(keyComment));
    str = `? ${str}
${indent2}:`;
  } else {
    str = `${str}:`;
    if (keyComment)
      str += lineComment(str, ctx.indent, commentString(keyComment));
  }
  let vsb, vcb, valueComment;
  if (isNode(value)) {
    vsb = !!value.spaceBefore;
    vcb = value.commentBefore;
    valueComment = value.comment;
  } else {
    vsb = false;
    vcb = null;
    valueComment = null;
    if (value && typeof value === "object")
      value = doc.createNode(value);
  }
  ctx.implicitKey = false;
  if (!explicitKey && !keyComment && isScalar(value))
    ctx.indentAtStart = str.length + 1;
  chompKeep = false;
  if (!indentSeq && indentStep.length >= 2 && !ctx.inFlow && !explicitKey && isSeq(value) && !value.flow && !value.tag && !value.anchor) {
    ctx.indent = ctx.indent.substring(2);
  }
  let valueCommentDone = false;
  const valueStr = stringify(value, ctx, () => valueCommentDone = true, () => chompKeep = true);
  let ws = " ";
  if (keyComment || vsb || vcb) {
    ws = vsb ? "\n" : "";
    if (vcb) {
      const cs = commentString(vcb);
      ws += `
${indentComment(cs, ctx.indent)}`;
    }
    if (valueStr === "" && !ctx.inFlow) {
      if (ws === "\n" && valueComment)
        ws = "\n\n";
    } else {
      ws += `
${ctx.indent}`;
    }
  } else if (!explicitKey && isCollection(value)) {
    const vs0 = valueStr[0];
    const nl0 = valueStr.indexOf("\n");
    const hasNewline = nl0 !== -1;
    const flow = (_b = (_a = ctx.inFlow) != null ? _a : value.flow) != null ? _b : value.items.length === 0;
    if (hasNewline || !flow) {
      let hasPropsLine = false;
      if (hasNewline && (vs0 === "&" || vs0 === "!")) {
        let sp0 = valueStr.indexOf(" ");
        if (vs0 === "&" && sp0 !== -1 && sp0 < nl0 && valueStr[sp0 + 1] === "!") {
          sp0 = valueStr.indexOf(" ", sp0 + 1);
        }
        if (sp0 === -1 || nl0 < sp0)
          hasPropsLine = true;
      }
      if (!hasPropsLine)
        ws = `
${ctx.indent}`;
    }
  } else if (valueStr === "" || valueStr[0] === "\n") {
    ws = "";
  }
  str += ws + valueStr;
  if (ctx.inFlow) {
    if (valueCommentDone && onComment)
      onComment();
  } else if (valueComment && !valueCommentDone) {
    str += lineComment(str, ctx.indent, commentString(valueComment));
  } else if (chompKeep && onChompKeep) {
    onChompKeep();
  }
  return str;
}

// node_modules/yaml/browser/dist/log.js
function warn(logLevel, warning) {
  if (logLevel === "debug" || logLevel === "warn") {
    console.warn(warning);
  }
}

// node_modules/yaml/browser/dist/schema/yaml-1.1/merge.js
var MERGE_KEY = "<<";
var merge = {
  identify: (value) => value === MERGE_KEY || typeof value === "symbol" && value.description === MERGE_KEY,
  default: "key",
  tag: "tag:yaml.org,2002:merge",
  test: /^<<$/,
  resolve: () => Object.assign(new Scalar(Symbol(MERGE_KEY)), {
    addToJSMap: addMergeToJSMap
  }),
  stringify: () => MERGE_KEY
};
var isMergeKey = (ctx, key) => (merge.identify(key) || isScalar(key) && (!key.type || key.type === Scalar.PLAIN) && merge.identify(key.value)) && (ctx == null ? void 0 : ctx.doc.schema.tags.some((tag) => tag.tag === merge.tag && tag.default));
function addMergeToJSMap(ctx, map2, value) {
  const source = resolveAliasValue(ctx, value);
  if (isSeq(source))
    for (const it of source.items)
      mergeValue(ctx, map2, it);
  else if (Array.isArray(source))
    for (const it of source)
      mergeValue(ctx, map2, it);
  else
    mergeValue(ctx, map2, source);
}
function mergeValue(ctx, map2, value) {
  const source = resolveAliasValue(ctx, value);
  if (!isMap2(source))
    throw new Error("Merge sources must be maps or map aliases");
  const srcMap = source.toJSON(null, ctx, Map);
  for (const [key, value2] of srcMap) {
    if (map2 instanceof Map) {
      if (!map2.has(key))
        map2.set(key, value2);
    } else if (map2 instanceof Set) {
      map2.add(key);
    } else if (!Object.prototype.hasOwnProperty.call(map2, key)) {
      Object.defineProperty(map2, key, {
        value: value2,
        writable: true,
        enumerable: true,
        configurable: true
      });
    }
  }
  return map2;
}
function resolveAliasValue(ctx, value) {
  return ctx && isAlias(value) ? value.resolve(ctx.doc, ctx) : value;
}

// node_modules/yaml/browser/dist/nodes/addPairToJSMap.js
function addPairToJSMap(ctx, map2, { key, value }) {
  if (isNode(key) && key.addToJSMap)
    key.addToJSMap(ctx, map2, value);
  else if (isMergeKey(ctx, key))
    addMergeToJSMap(ctx, map2, value);
  else {
    const jsKey = toJS(key, "", ctx);
    if (map2 instanceof Map) {
      map2.set(jsKey, toJS(value, jsKey, ctx));
    } else if (map2 instanceof Set) {
      map2.add(jsKey);
    } else {
      const stringKey = stringifyKey(key, jsKey, ctx);
      const jsValue = toJS(value, stringKey, ctx);
      if (stringKey in map2)
        Object.defineProperty(map2, stringKey, {
          value: jsValue,
          writable: true,
          enumerable: true,
          configurable: true
        });
      else
        map2[stringKey] = jsValue;
    }
  }
  return map2;
}
function stringifyKey(key, jsKey, ctx) {
  if (jsKey === null)
    return "";
  if (typeof jsKey !== "object")
    return String(jsKey);
  if (isNode(key) && (ctx == null ? void 0 : ctx.doc)) {
    const strCtx = createStringifyContext(ctx.doc, {});
    strCtx.anchors = /* @__PURE__ */ new Set();
    for (const node of ctx.anchors.keys())
      strCtx.anchors.add(node.anchor);
    strCtx.inFlow = true;
    strCtx.inStringifyKey = true;
    const strKey = key.toString(strCtx);
    if (!ctx.mapKeyWarned) {
      let jsonStr = JSON.stringify(strKey);
      if (jsonStr.length > 40)
        jsonStr = jsonStr.substring(0, 36) + '..."';
      warn(ctx.doc.options.logLevel, `Keys with collection values will be stringified due to JS Object restrictions: ${jsonStr}. Set mapAsMap: true to use object keys.`);
      ctx.mapKeyWarned = true;
    }
    return strKey;
  }
  return JSON.stringify(jsKey);
}

// node_modules/yaml/browser/dist/nodes/Pair.js
function createPair(key, value, ctx) {
  const k = createNode(key, void 0, ctx);
  const v = createNode(value, void 0, ctx);
  return new Pair(k, v);
}
var Pair = class _Pair {
  constructor(key, value = null) {
    Object.defineProperty(this, NODE_TYPE, { value: PAIR });
    this.key = key;
    this.value = value;
  }
  clone(schema4) {
    let { key, value } = this;
    if (isNode(key))
      key = key.clone(schema4);
    if (isNode(value))
      value = value.clone(schema4);
    return new _Pair(key, value);
  }
  toJSON(_, ctx) {
    const pair = (ctx == null ? void 0 : ctx.mapAsMap) ? /* @__PURE__ */ new Map() : {};
    return addPairToJSMap(ctx, pair, this);
  }
  toString(ctx, onComment, onChompKeep) {
    return (ctx == null ? void 0 : ctx.doc) ? stringifyPair(this, ctx, onComment, onChompKeep) : JSON.stringify(this);
  }
};

// node_modules/yaml/browser/dist/stringify/stringifyCollection.js
function stringifyCollection(collection, ctx, options) {
  var _a;
  const flow = (_a = ctx.inFlow) != null ? _a : collection.flow;
  const stringify4 = flow ? stringifyFlowCollection : stringifyBlockCollection;
  return stringify4(collection, ctx, options);
}
function stringifyBlockCollection({ comment, items }, ctx, { blockItemPrefix, flowChars, itemIndent, onChompKeep, onComment }) {
  const { indent: indent2, options: { commentString } } = ctx;
  const itemCtx = Object.assign({}, ctx, { indent: itemIndent, type: null });
  let chompKeep = false;
  const lines = [];
  for (let i = 0; i < items.length; ++i) {
    const item = items[i];
    let comment2 = null;
    if (isNode(item)) {
      if (!chompKeep && item.spaceBefore)
        lines.push("");
      addCommentBefore(ctx, lines, item.commentBefore, chompKeep);
      if (item.comment)
        comment2 = item.comment;
    } else if (isPair(item)) {
      const ik = isNode(item.key) ? item.key : null;
      if (ik) {
        if (!chompKeep && ik.spaceBefore)
          lines.push("");
        addCommentBefore(ctx, lines, ik.commentBefore, chompKeep);
      }
    }
    chompKeep = false;
    let str2 = stringify(item, itemCtx, () => comment2 = null, () => chompKeep = true);
    if (comment2)
      str2 += lineComment(str2, itemIndent, commentString(comment2));
    if (chompKeep && comment2)
      chompKeep = false;
    lines.push(blockItemPrefix + str2);
  }
  let str;
  if (lines.length === 0) {
    str = flowChars.start + flowChars.end;
  } else {
    str = lines[0];
    for (let i = 1; i < lines.length; ++i) {
      const line = lines[i];
      str += line ? `
${indent2}${line}` : "\n";
    }
  }
  if (comment) {
    str += "\n" + indentComment(commentString(comment), indent2);
    if (onComment)
      onComment();
  } else if (chompKeep && onChompKeep)
    onChompKeep();
  return str;
}
function stringifyFlowCollection({ items }, ctx, { flowChars, itemIndent }) {
  const { indent: indent2, indentStep, flowCollectionPadding: fcPadding, options: { commentString } } = ctx;
  itemIndent += indentStep;
  const itemCtx = Object.assign({}, ctx, {
    indent: itemIndent,
    inFlow: true,
    type: null
  });
  let reqNewline = false;
  let linesAtValue = 0;
  const lines = [];
  for (let i = 0; i < items.length; ++i) {
    const item = items[i];
    let comment = null;
    if (isNode(item)) {
      if (item.spaceBefore)
        lines.push("");
      addCommentBefore(ctx, lines, item.commentBefore, false);
      if (item.comment)
        comment = item.comment;
    } else if (isPair(item)) {
      const ik = isNode(item.key) ? item.key : null;
      if (ik) {
        if (ik.spaceBefore)
          lines.push("");
        addCommentBefore(ctx, lines, ik.commentBefore, false);
        if (ik.comment)
          reqNewline = true;
      }
      const iv = isNode(item.value) ? item.value : null;
      if (iv) {
        if (iv.comment)
          comment = iv.comment;
        if (iv.commentBefore)
          reqNewline = true;
      } else if (item.value == null && (ik == null ? void 0 : ik.comment)) {
        comment = ik.comment;
      }
    }
    if (comment)
      reqNewline = true;
    let str = stringify(item, itemCtx, () => comment = null);
    reqNewline || (reqNewline = lines.length > linesAtValue || str.includes("\n"));
    if (i < items.length - 1) {
      str += ",";
    } else if (ctx.options.trailingComma) {
      if (ctx.options.lineWidth > 0) {
        reqNewline || (reqNewline = lines.reduce((sum, line) => sum + line.length + 2, 2) + (str.length + 2) > ctx.options.lineWidth);
      }
      if (reqNewline) {
        str += ",";
      }
    }
    if (comment)
      str += lineComment(str, itemIndent, commentString(comment));
    lines.push(str);
    linesAtValue = lines.length;
  }
  const { start, end } = flowChars;
  if (lines.length === 0) {
    return start + end;
  } else {
    if (!reqNewline) {
      const len = lines.reduce((sum, line) => sum + line.length + 2, 2);
      reqNewline = ctx.options.lineWidth > 0 && len > ctx.options.lineWidth;
    }
    if (reqNewline) {
      let str = start;
      for (const line of lines)
        str += line ? `
${indentStep}${indent2}${line}` : "\n";
      return `${str}
${indent2}${end}`;
    } else {
      return `${start}${fcPadding}${lines.join(" ")}${fcPadding}${end}`;
    }
  }
}
function addCommentBefore({ indent: indent2, options: { commentString } }, lines, comment, chompKeep) {
  if (comment && chompKeep)
    comment = comment.replace(/^\n+/, "");
  if (comment) {
    const ic = indentComment(commentString(comment), indent2);
    lines.push(ic.trimStart());
  }
}

// node_modules/yaml/browser/dist/nodes/YAMLMap.js
function findPair(items, key) {
  const k = isScalar(key) ? key.value : key;
  for (const it of items) {
    if (isPair(it)) {
      if (it.key === key || it.key === k)
        return it;
      if (isScalar(it.key) && it.key.value === k)
        return it;
    }
  }
  return void 0;
}
var YAMLMap = class extends Collection {
  static get tagName() {
    return "tag:yaml.org,2002:map";
  }
  constructor(schema4) {
    super(MAP, schema4);
    this.items = [];
  }
  /**
   * A generic collection parsing method that can be extended
   * to other node classes that inherit from YAMLMap
   */
  static from(schema4, obj, ctx) {
    const { keepUndefined, replacer } = ctx;
    const map2 = new this(schema4);
    const add = (key, value) => {
      if (typeof replacer === "function")
        value = replacer.call(obj, key, value);
      else if (Array.isArray(replacer) && !replacer.includes(key))
        return;
      if (value !== void 0 || keepUndefined)
        map2.items.push(createPair(key, value, ctx));
    };
    if (obj instanceof Map) {
      for (const [key, value] of obj)
        add(key, value);
    } else if (obj && typeof obj === "object") {
      for (const key of Object.keys(obj))
        add(key, obj[key]);
    }
    if (typeof schema4.sortMapEntries === "function") {
      map2.items.sort(schema4.sortMapEntries);
    }
    return map2;
  }
  /**
   * Adds a value to the collection.
   *
   * @param overwrite - If not set `true`, using a key that is already in the
   *   collection will throw. Otherwise, overwrites the previous value.
   */
  add(pair, overwrite) {
    var _a;
    let _pair;
    if (isPair(pair))
      _pair = pair;
    else if (!pair || typeof pair !== "object" || !("key" in pair)) {
      _pair = new Pair(pair, pair == null ? void 0 : pair.value);
    } else
      _pair = new Pair(pair.key, pair.value);
    const prev = findPair(this.items, _pair.key);
    const sortEntries = (_a = this.schema) == null ? void 0 : _a.sortMapEntries;
    if (prev) {
      if (!overwrite)
        throw new Error(`Key ${_pair.key} already set`);
      if (isScalar(prev.value) && isScalarValue(_pair.value))
        prev.value.value = _pair.value;
      else
        prev.value = _pair.value;
    } else if (sortEntries) {
      const i = this.items.findIndex((item) => sortEntries(_pair, item) < 0);
      if (i === -1)
        this.items.push(_pair);
      else
        this.items.splice(i, 0, _pair);
    } else {
      this.items.push(_pair);
    }
  }
  delete(key) {
    const it = findPair(this.items, key);
    if (!it)
      return false;
    const del = this.items.splice(this.items.indexOf(it), 1);
    return del.length > 0;
  }
  get(key, keepScalar) {
    var _a;
    const it = findPair(this.items, key);
    const node = it == null ? void 0 : it.value;
    return (_a = !keepScalar && isScalar(node) ? node.value : node) != null ? _a : void 0;
  }
  has(key) {
    return !!findPair(this.items, key);
  }
  set(key, value) {
    this.add(new Pair(key, value), true);
  }
  /**
   * @param ctx - Conversion context, originally set in Document#toJS()
   * @param {Class} Type - If set, forces the returned collection type
   * @returns Instance of Type, Map, or Object
   */
  toJSON(_, ctx, Type) {
    const map2 = Type ? new Type() : (ctx == null ? void 0 : ctx.mapAsMap) ? /* @__PURE__ */ new Map() : {};
    if (ctx == null ? void 0 : ctx.onCreate)
      ctx.onCreate(map2);
    for (const item of this.items)
      addPairToJSMap(ctx, map2, item);
    return map2;
  }
  toString(ctx, onComment, onChompKeep) {
    if (!ctx)
      return JSON.stringify(this);
    for (const item of this.items) {
      if (!isPair(item))
        throw new Error(`Map items must all be pairs; found ${JSON.stringify(item)} instead`);
    }
    if (!ctx.allNullValues && this.hasAllNullValues(false))
      ctx = Object.assign({}, ctx, { allNullValues: true });
    return stringifyCollection(this, ctx, {
      blockItemPrefix: "",
      flowChars: { start: "{", end: "}" },
      itemIndent: ctx.indent || "",
      onChompKeep,
      onComment
    });
  }
};

// node_modules/yaml/browser/dist/schema/common/map.js
var map = {
  collection: "map",
  default: true,
  nodeClass: YAMLMap,
  tag: "tag:yaml.org,2002:map",
  resolve(map2, onError) {
    if (!isMap2(map2))
      onError("Expected a mapping for this tag");
    return map2;
  },
  createNode: (schema4, obj, ctx) => YAMLMap.from(schema4, obj, ctx)
};

// node_modules/yaml/browser/dist/nodes/YAMLSeq.js
var YAMLSeq = class extends Collection {
  static get tagName() {
    return "tag:yaml.org,2002:seq";
  }
  constructor(schema4) {
    super(SEQ, schema4);
    this.items = [];
  }
  add(value) {
    this.items.push(value);
  }
  /**
   * Removes a value from the collection.
   *
   * `key` must contain a representation of an integer for this to succeed.
   * It may be wrapped in a `Scalar`.
   *
   * @returns `true` if the item was found and removed.
   */
  delete(key) {
    const idx = asItemIndex(key);
    if (typeof idx !== "number")
      return false;
    const del = this.items.splice(idx, 1);
    return del.length > 0;
  }
  get(key, keepScalar) {
    const idx = asItemIndex(key);
    if (typeof idx !== "number")
      return void 0;
    const it = this.items[idx];
    return !keepScalar && isScalar(it) ? it.value : it;
  }
  /**
   * Checks if the collection includes a value with the key `key`.
   *
   * `key` must contain a representation of an integer for this to succeed.
   * It may be wrapped in a `Scalar`.
   */
  has(key) {
    const idx = asItemIndex(key);
    return typeof idx === "number" && idx < this.items.length;
  }
  /**
   * Sets a value in this collection. For `!!set`, `value` needs to be a
   * boolean to add/remove the item from the set.
   *
   * If `key` does not contain a representation of an integer, this will throw.
   * It may be wrapped in a `Scalar`.
   */
  set(key, value) {
    const idx = asItemIndex(key);
    if (typeof idx !== "number")
      throw new Error(`Expected a valid index, not ${key}.`);
    const prev = this.items[idx];
    if (isScalar(prev) && isScalarValue(value))
      prev.value = value;
    else
      this.items[idx] = value;
  }
  toJSON(_, ctx) {
    const seq2 = [];
    if (ctx == null ? void 0 : ctx.onCreate)
      ctx.onCreate(seq2);
    let i = 0;
    for (const item of this.items)
      seq2.push(toJS(item, String(i++), ctx));
    return seq2;
  }
  toString(ctx, onComment, onChompKeep) {
    if (!ctx)
      return JSON.stringify(this);
    return stringifyCollection(this, ctx, {
      blockItemPrefix: "- ",
      flowChars: { start: "[", end: "]" },
      itemIndent: (ctx.indent || "") + "  ",
      onChompKeep,
      onComment
    });
  }
  static from(schema4, obj, ctx) {
    const { replacer } = ctx;
    const seq2 = new this(schema4);
    if (obj && Symbol.iterator in Object(obj)) {
      let i = 0;
      for (let it of obj) {
        if (typeof replacer === "function") {
          const key = obj instanceof Set ? it : String(i++);
          it = replacer.call(obj, key, it);
        }
        seq2.items.push(createNode(it, void 0, ctx));
      }
    }
    return seq2;
  }
};
function asItemIndex(key) {
  let idx = isScalar(key) ? key.value : key;
  if (idx && typeof idx === "string")
    idx = Number(idx);
  return typeof idx === "number" && Number.isInteger(idx) && idx >= 0 ? idx : null;
}

// node_modules/yaml/browser/dist/schema/common/seq.js
var seq = {
  collection: "seq",
  default: true,
  nodeClass: YAMLSeq,
  tag: "tag:yaml.org,2002:seq",
  resolve(seq2, onError) {
    if (!isSeq(seq2))
      onError("Expected a sequence for this tag");
    return seq2;
  },
  createNode: (schema4, obj, ctx) => YAMLSeq.from(schema4, obj, ctx)
};

// node_modules/yaml/browser/dist/schema/common/string.js
var string = {
  identify: (value) => typeof value === "string",
  default: true,
  tag: "tag:yaml.org,2002:str",
  resolve: (str) => str,
  stringify(item, ctx, onComment, onChompKeep) {
    ctx = Object.assign({ actualString: true }, ctx);
    return stringifyString(item, ctx, onComment, onChompKeep);
  }
};

// node_modules/yaml/browser/dist/schema/common/null.js
var nullTag = {
  identify: (value) => value == null,
  createNode: () => new Scalar(null),
  default: true,
  tag: "tag:yaml.org,2002:null",
  test: /^(?:~|[Nn]ull|NULL)?$/,
  resolve: () => new Scalar(null),
  stringify: ({ source }, ctx) => typeof source === "string" && nullTag.test.test(source) ? source : ctx.options.nullStr
};

// node_modules/yaml/browser/dist/schema/core/bool.js
var boolTag = {
  identify: (value) => typeof value === "boolean",
  default: true,
  tag: "tag:yaml.org,2002:bool",
  test: /^(?:[Tt]rue|TRUE|[Ff]alse|FALSE)$/,
  resolve: (str) => new Scalar(str[0] === "t" || str[0] === "T"),
  stringify({ source, value }, ctx) {
    if (source && boolTag.test.test(source)) {
      const sv = source[0] === "t" || source[0] === "T";
      if (value === sv)
        return source;
    }
    return value ? ctx.options.trueStr : ctx.options.falseStr;
  }
};

// node_modules/yaml/browser/dist/stringify/stringifyNumber.js
function stringifyNumber({ format, minFractionDigits, tag, value }) {
  if (typeof value === "bigint")
    return String(value);
  const num = typeof value === "number" ? value : Number(value);
  if (!isFinite(num))
    return isNaN(num) ? ".nan" : num < 0 ? "-.inf" : ".inf";
  let n = Object.is(value, -0) ? "-0" : JSON.stringify(value);
  if (!format && minFractionDigits && (!tag || tag === "tag:yaml.org,2002:float") && /^-?\d/.test(n) && !n.includes("e")) {
    let i = n.indexOf(".");
    if (i < 0) {
      i = n.length;
      n += ".";
    }
    let d = minFractionDigits - (n.length - i - 1);
    while (d-- > 0)
      n += "0";
  }
  return n;
}

// node_modules/yaml/browser/dist/schema/core/float.js
var floatNaN = {
  identify: (value) => typeof value === "number",
  default: true,
  tag: "tag:yaml.org,2002:float",
  test: /^(?:[-+]?\.(?:inf|Inf|INF)|\.nan|\.NaN|\.NAN)$/,
  resolve: (str) => str.slice(-3).toLowerCase() === "nan" ? NaN : str[0] === "-" ? Number.NEGATIVE_INFINITY : Number.POSITIVE_INFINITY,
  stringify: stringifyNumber
};
var floatExp = {
  identify: (value) => typeof value === "number",
  default: true,
  tag: "tag:yaml.org,2002:float",
  format: "EXP",
  test: /^[-+]?(?:\.[0-9]+|[0-9]+(?:\.[0-9]*)?)[eE][-+]?[0-9]+$/,
  resolve: (str) => parseFloat(str),
  stringify(node) {
    const num = Number(node.value);
    return isFinite(num) ? num.toExponential() : stringifyNumber(node);
  }
};
var float = {
  identify: (value) => typeof value === "number",
  default: true,
  tag: "tag:yaml.org,2002:float",
  test: /^[-+]?(?:\.[0-9]+|[0-9]+\.[0-9]*)$/,
  resolve(str) {
    const node = new Scalar(parseFloat(str));
    const dot = str.indexOf(".");
    if (dot !== -1 && str[str.length - 1] === "0")
      node.minFractionDigits = str.length - dot - 1;
    return node;
  },
  stringify: stringifyNumber
};

// node_modules/yaml/browser/dist/schema/core/int.js
var intIdentify = (value) => typeof value === "bigint" || Number.isInteger(value);
var intResolve = (str, offset, radix, { intAsBigInt }) => intAsBigInt ? BigInt(str) : parseInt(str.substring(offset), radix);
function intStringify(node, radix, prefix) {
  const { value } = node;
  if (intIdentify(value) && value >= 0)
    return prefix + value.toString(radix);
  return stringifyNumber(node);
}
var intOct = {
  identify: (value) => intIdentify(value) && value >= 0,
  default: true,
  tag: "tag:yaml.org,2002:int",
  format: "OCT",
  test: /^0o[0-7]+$/,
  resolve: (str, _onError, opt) => intResolve(str, 2, 8, opt),
  stringify: (node) => intStringify(node, 8, "0o")
};
var int = {
  identify: intIdentify,
  default: true,
  tag: "tag:yaml.org,2002:int",
  test: /^[-+]?[0-9]+$/,
  resolve: (str, _onError, opt) => intResolve(str, 0, 10, opt),
  stringify: stringifyNumber
};
var intHex = {
  identify: (value) => intIdentify(value) && value >= 0,
  default: true,
  tag: "tag:yaml.org,2002:int",
  format: "HEX",
  test: /^0x[0-9a-fA-F]+$/,
  resolve: (str, _onError, opt) => intResolve(str, 2, 16, opt),
  stringify: (node) => intStringify(node, 16, "0x")
};

// node_modules/yaml/browser/dist/schema/core/schema.js
var schema = [
  map,
  seq,
  string,
  nullTag,
  boolTag,
  intOct,
  int,
  intHex,
  floatNaN,
  floatExp,
  float
];

// node_modules/yaml/browser/dist/schema/json/schema.js
function intIdentify2(value) {
  return typeof value === "bigint" || Number.isInteger(value);
}
var stringifyJSON = ({ value }) => JSON.stringify(value);
var jsonScalars = [
  {
    identify: (value) => typeof value === "string",
    default: true,
    tag: "tag:yaml.org,2002:str",
    resolve: (str) => str,
    stringify: stringifyJSON
  },
  {
    identify: (value) => value == null,
    createNode: () => new Scalar(null),
    default: true,
    tag: "tag:yaml.org,2002:null",
    test: /^null$/,
    resolve: () => null,
    stringify: stringifyJSON
  },
  {
    identify: (value) => typeof value === "boolean",
    default: true,
    tag: "tag:yaml.org,2002:bool",
    test: /^true$|^false$/,
    resolve: (str) => str === "true",
    stringify: stringifyJSON
  },
  {
    identify: intIdentify2,
    default: true,
    tag: "tag:yaml.org,2002:int",
    test: /^-?(?:0|[1-9][0-9]*)$/,
    resolve: (str, _onError, { intAsBigInt }) => intAsBigInt ? BigInt(str) : parseInt(str, 10),
    stringify: ({ value }) => intIdentify2(value) ? value.toString() : JSON.stringify(value)
  },
  {
    identify: (value) => typeof value === "number",
    default: true,
    tag: "tag:yaml.org,2002:float",
    test: /^-?(?:0|[1-9][0-9]*)(?:\.[0-9]*)?(?:[eE][-+]?[0-9]+)?$/,
    resolve: (str) => parseFloat(str),
    stringify: stringifyJSON
  }
];
var jsonError = {
  default: true,
  tag: "",
  test: /^/,
  resolve(str, onError) {
    onError(`Unresolved plain scalar ${JSON.stringify(str)}`);
    return str;
  }
};
var schema2 = [map, seq].concat(jsonScalars, jsonError);

// node_modules/yaml/browser/dist/schema/yaml-1.1/binary.js
var binary = {
  identify: (value) => value instanceof Uint8Array,
  // Buffer inherits from Uint8Array
  default: false,
  tag: "tag:yaml.org,2002:binary",
  /**
   * Returns a Buffer in node and an Uint8Array in browsers
   *
   * To use the resulting buffer as an image, you'll want to do something like:
   *
   *   const blob = new Blob([buffer], { type: 'image/jpeg' })
   *   document.querySelector('#photo').src = URL.createObjectURL(blob)
   */
  resolve(src, onError) {
    if (typeof atob === "function") {
      const str = atob(src.replace(/[\n\r]/g, ""));
      const buffer = new Uint8Array(str.length);
      for (let i = 0; i < str.length; ++i)
        buffer[i] = str.charCodeAt(i);
      return buffer;
    } else {
      onError("This environment does not support reading binary tags; either Buffer or atob is required");
      return src;
    }
  },
  stringify({ comment, type, value }, ctx, onComment, onChompKeep) {
    if (!value)
      return "";
    const buf = value;
    let str;
    if (typeof btoa === "function") {
      let s = "";
      for (let i = 0; i < buf.length; ++i)
        s += String.fromCharCode(buf[i]);
      str = btoa(s);
    } else {
      throw new Error("This environment does not support writing binary tags; either Buffer or btoa is required");
    }
    type != null ? type : type = Scalar.BLOCK_LITERAL;
    if (type !== Scalar.QUOTE_DOUBLE) {
      const lineWidth = Math.max(ctx.options.lineWidth - ctx.indent.length, ctx.options.minContentWidth);
      const n = Math.ceil(str.length / lineWidth);
      const lines = new Array(n);
      for (let i = 0, o = 0; i < n; ++i, o += lineWidth) {
        lines[i] = str.substr(o, lineWidth);
      }
      str = lines.join(type === Scalar.BLOCK_LITERAL ? "\n" : " ");
    }
    return stringifyString({ comment, type, value: str }, ctx, onComment, onChompKeep);
  }
};

// node_modules/yaml/browser/dist/schema/yaml-1.1/pairs.js
function resolvePairs(seq2, onError) {
  var _a;
  if (isSeq(seq2)) {
    for (let i = 0; i < seq2.items.length; ++i) {
      let item = seq2.items[i];
      if (isPair(item))
        continue;
      else if (isMap2(item)) {
        if (item.items.length > 1)
          onError("Each pair must have its own sequence indicator");
        const pair = item.items[0] || new Pair(new Scalar(null));
        if (item.commentBefore)
          pair.key.commentBefore = pair.key.commentBefore ? `${item.commentBefore}
${pair.key.commentBefore}` : item.commentBefore;
        if (item.comment) {
          const cn = (_a = pair.value) != null ? _a : pair.key;
          cn.comment = cn.comment ? `${item.comment}
${cn.comment}` : item.comment;
        }
        item = pair;
      }
      seq2.items[i] = isPair(item) ? item : new Pair(item);
    }
  } else
    onError("Expected a sequence for this tag");
  return seq2;
}
function createPairs(schema4, iterable, ctx) {
  const { replacer } = ctx;
  const pairs2 = new YAMLSeq(schema4);
  pairs2.tag = "tag:yaml.org,2002:pairs";
  let i = 0;
  if (iterable && Symbol.iterator in Object(iterable))
    for (let it of iterable) {
      if (typeof replacer === "function")
        it = replacer.call(iterable, String(i++), it);
      let key, value;
      if (Array.isArray(it)) {
        if (it.length === 2) {
          key = it[0];
          value = it[1];
        } else
          throw new TypeError(`Expected [key, value] tuple: ${it}`);
      } else if (it && it instanceof Object) {
        const keys = Object.keys(it);
        if (keys.length === 1) {
          key = keys[0];
          value = it[key];
        } else {
          throw new TypeError(`Expected tuple with one key, not ${keys.length} keys`);
        }
      } else {
        key = it;
      }
      pairs2.items.push(createPair(key, value, ctx));
    }
  return pairs2;
}
var pairs = {
  collection: "seq",
  default: false,
  tag: "tag:yaml.org,2002:pairs",
  resolve: resolvePairs,
  createNode: createPairs
};

// node_modules/yaml/browser/dist/schema/yaml-1.1/omap.js
var YAMLOMap = class _YAMLOMap extends YAMLSeq {
  constructor() {
    super();
    this.add = YAMLMap.prototype.add.bind(this);
    this.delete = YAMLMap.prototype.delete.bind(this);
    this.get = YAMLMap.prototype.get.bind(this);
    this.has = YAMLMap.prototype.has.bind(this);
    this.set = YAMLMap.prototype.set.bind(this);
    this.tag = _YAMLOMap.tag;
  }
  /**
   * If `ctx` is given, the return type is actually `Map<unknown, unknown>`,
   * but TypeScript won't allow widening the signature of a child method.
   */
  toJSON(_, ctx) {
    if (!ctx)
      return super.toJSON(_);
    const map2 = /* @__PURE__ */ new Map();
    if (ctx == null ? void 0 : ctx.onCreate)
      ctx.onCreate(map2);
    for (const pair of this.items) {
      let key, value;
      if (isPair(pair)) {
        key = toJS(pair.key, "", ctx);
        value = toJS(pair.value, key, ctx);
      } else {
        key = toJS(pair, "", ctx);
      }
      if (map2.has(key))
        throw new Error("Ordered maps must not include duplicate keys");
      map2.set(key, value);
    }
    return map2;
  }
  static from(schema4, iterable, ctx) {
    const pairs2 = createPairs(schema4, iterable, ctx);
    const omap2 = new this();
    omap2.items = pairs2.items;
    return omap2;
  }
};
YAMLOMap.tag = "tag:yaml.org,2002:omap";
var omap = {
  collection: "seq",
  identify: (value) => value instanceof Map,
  nodeClass: YAMLOMap,
  default: false,
  tag: "tag:yaml.org,2002:omap",
  resolve(seq2, onError) {
    const pairs2 = resolvePairs(seq2, onError);
    const seenKeys = [];
    for (const { key } of pairs2.items) {
      if (isScalar(key)) {
        if (seenKeys.includes(key.value)) {
          onError(`Ordered maps must not include duplicate keys: ${key.value}`);
        } else {
          seenKeys.push(key.value);
        }
      }
    }
    return Object.assign(new YAMLOMap(), pairs2);
  },
  createNode: (schema4, iterable, ctx) => YAMLOMap.from(schema4, iterable, ctx)
};

// node_modules/yaml/browser/dist/schema/yaml-1.1/bool.js
function boolStringify({ value, source }, ctx) {
  const boolObj = value ? trueTag : falseTag;
  if (source && boolObj.test.test(source))
    return source;
  return value ? ctx.options.trueStr : ctx.options.falseStr;
}
var trueTag = {
  identify: (value) => value === true,
  default: true,
  tag: "tag:yaml.org,2002:bool",
  test: /^(?:Y|y|[Yy]es|YES|[Tt]rue|TRUE|[Oo]n|ON)$/,
  resolve: () => new Scalar(true),
  stringify: boolStringify
};
var falseTag = {
  identify: (value) => value === false,
  default: true,
  tag: "tag:yaml.org,2002:bool",
  test: /^(?:N|n|[Nn]o|NO|[Ff]alse|FALSE|[Oo]ff|OFF)$/,
  resolve: () => new Scalar(false),
  stringify: boolStringify
};

// node_modules/yaml/browser/dist/schema/yaml-1.1/float.js
var floatNaN2 = {
  identify: (value) => typeof value === "number",
  default: true,
  tag: "tag:yaml.org,2002:float",
  test: /^(?:[-+]?\.(?:inf|Inf|INF)|\.nan|\.NaN|\.NAN)$/,
  resolve: (str) => str.slice(-3).toLowerCase() === "nan" ? NaN : str[0] === "-" ? Number.NEGATIVE_INFINITY : Number.POSITIVE_INFINITY,
  stringify: stringifyNumber
};
var floatExp2 = {
  identify: (value) => typeof value === "number",
  default: true,
  tag: "tag:yaml.org,2002:float",
  format: "EXP",
  test: /^[-+]?(?:[0-9][0-9_]*)?(?:\.[0-9_]*)?[eE][-+]?[0-9]+$/,
  resolve: (str) => parseFloat(str.replace(/_/g, "")),
  stringify(node) {
    const num = Number(node.value);
    return isFinite(num) ? num.toExponential() : stringifyNumber(node);
  }
};
var float2 = {
  identify: (value) => typeof value === "number",
  default: true,
  tag: "tag:yaml.org,2002:float",
  test: /^[-+]?(?:[0-9][0-9_]*)?\.[0-9_]*$/,
  resolve(str) {
    const node = new Scalar(parseFloat(str.replace(/_/g, "")));
    const dot = str.indexOf(".");
    if (dot !== -1) {
      const f = str.substring(dot + 1).replace(/_/g, "");
      if (f[f.length - 1] === "0")
        node.minFractionDigits = f.length;
    }
    return node;
  },
  stringify: stringifyNumber
};

// node_modules/yaml/browser/dist/schema/yaml-1.1/int.js
var intIdentify3 = (value) => typeof value === "bigint" || Number.isInteger(value);
function intResolve2(str, offset, radix, { intAsBigInt }) {
  const sign = str[0];
  if (sign === "-" || sign === "+")
    offset += 1;
  str = str.substring(offset).replace(/_/g, "");
  if (intAsBigInt) {
    switch (radix) {
      case 2:
        str = `0b${str}`;
        break;
      case 8:
        str = `0o${str}`;
        break;
      case 16:
        str = `0x${str}`;
        break;
    }
    const n2 = BigInt(str);
    return sign === "-" ? BigInt(-1) * n2 : n2;
  }
  const n = parseInt(str, radix);
  return sign === "-" ? -1 * n : n;
}
function intStringify2(node, radix, prefix) {
  const { value } = node;
  if (intIdentify3(value)) {
    const str = value.toString(radix);
    return value < 0 ? "-" + prefix + str.substr(1) : prefix + str;
  }
  return stringifyNumber(node);
}
var intBin = {
  identify: intIdentify3,
  default: true,
  tag: "tag:yaml.org,2002:int",
  format: "BIN",
  test: /^[-+]?0b[0-1_]+$/,
  resolve: (str, _onError, opt) => intResolve2(str, 2, 2, opt),
  stringify: (node) => intStringify2(node, 2, "0b")
};
var intOct2 = {
  identify: intIdentify3,
  default: true,
  tag: "tag:yaml.org,2002:int",
  format: "OCT",
  test: /^[-+]?0[0-7_]+$/,
  resolve: (str, _onError, opt) => intResolve2(str, 1, 8, opt),
  stringify: (node) => intStringify2(node, 8, "0")
};
var int2 = {
  identify: intIdentify3,
  default: true,
  tag: "tag:yaml.org,2002:int",
  test: /^[-+]?[0-9][0-9_]*$/,
  resolve: (str, _onError, opt) => intResolve2(str, 0, 10, opt),
  stringify: stringifyNumber
};
var intHex2 = {
  identify: intIdentify3,
  default: true,
  tag: "tag:yaml.org,2002:int",
  format: "HEX",
  test: /^[-+]?0x[0-9a-fA-F_]+$/,
  resolve: (str, _onError, opt) => intResolve2(str, 2, 16, opt),
  stringify: (node) => intStringify2(node, 16, "0x")
};

// node_modules/yaml/browser/dist/schema/yaml-1.1/set.js
var YAMLSet = class _YAMLSet extends YAMLMap {
  constructor(schema4) {
    super(schema4);
    this.tag = _YAMLSet.tag;
  }
  add(key) {
    let pair;
    if (isPair(key))
      pair = key;
    else if (key && typeof key === "object" && "key" in key && "value" in key && key.value === null)
      pair = new Pair(key.key, null);
    else
      pair = new Pair(key, null);
    const prev = findPair(this.items, pair.key);
    if (!prev)
      this.items.push(pair);
  }
  /**
   * If `keepPair` is `true`, returns the Pair matching `key`.
   * Otherwise, returns the value of that Pair's key.
   */
  get(key, keepPair) {
    const pair = findPair(this.items, key);
    return !keepPair && isPair(pair) ? isScalar(pair.key) ? pair.key.value : pair.key : pair;
  }
  set(key, value) {
    if (typeof value !== "boolean")
      throw new Error(`Expected boolean value for set(key, value) in a YAML set, not ${typeof value}`);
    const prev = findPair(this.items, key);
    if (prev && !value) {
      this.items.splice(this.items.indexOf(prev), 1);
    } else if (!prev && value) {
      this.items.push(new Pair(key));
    }
  }
  toJSON(_, ctx) {
    return super.toJSON(_, ctx, Set);
  }
  toString(ctx, onComment, onChompKeep) {
    if (!ctx)
      return JSON.stringify(this);
    if (this.hasAllNullValues(true))
      return super.toString(Object.assign({}, ctx, { allNullValues: true }), onComment, onChompKeep);
    else
      throw new Error("Set items must all have null values");
  }
  static from(schema4, iterable, ctx) {
    const { replacer } = ctx;
    const set2 = new this(schema4);
    if (iterable && Symbol.iterator in Object(iterable))
      for (let value of iterable) {
        if (typeof replacer === "function")
          value = replacer.call(iterable, value, value);
        set2.items.push(createPair(value, null, ctx));
      }
    return set2;
  }
};
YAMLSet.tag = "tag:yaml.org,2002:set";
var set = {
  collection: "map",
  identify: (value) => value instanceof Set,
  nodeClass: YAMLSet,
  default: false,
  tag: "tag:yaml.org,2002:set",
  createNode: (schema4, iterable, ctx) => YAMLSet.from(schema4, iterable, ctx),
  resolve(map2, onError) {
    if (isMap2(map2)) {
      if (map2.hasAllNullValues(true))
        return Object.assign(new YAMLSet(), map2);
      else
        onError("Set items must all have null values");
    } else
      onError("Expected a mapping for this tag");
    return map2;
  }
};

// node_modules/yaml/browser/dist/schema/yaml-1.1/timestamp.js
function parseSexagesimal(str, asBigInt) {
  const sign = str[0];
  const parts = sign === "-" || sign === "+" ? str.substring(1) : str;
  const num = (n) => asBigInt ? BigInt(n) : Number(n);
  const res = parts.replace(/_/g, "").split(":").reduce((res2, p) => res2 * num(60) + num(p), num(0));
  return sign === "-" ? num(-1) * res : res;
}
function stringifySexagesimal(node) {
  let { value } = node;
  let num = (n) => n;
  if (typeof value === "bigint")
    num = (n) => BigInt(n);
  else if (isNaN(value) || !isFinite(value))
    return stringifyNumber(node);
  let sign = "";
  if (value < 0) {
    sign = "-";
    value *= num(-1);
  }
  const _60 = num(60);
  const parts = [value % _60];
  if (value < 60) {
    parts.unshift(0);
  } else {
    value = (value - parts[0]) / _60;
    parts.unshift(value % _60);
    if (value >= 60) {
      value = (value - parts[0]) / _60;
      parts.unshift(value);
    }
  }
  return sign + parts.map((n) => String(n).padStart(2, "0")).join(":").replace(/000000\d*$/, "");
}
var intTime = {
  identify: (value) => typeof value === "bigint" || Number.isInteger(value),
  default: true,
  tag: "tag:yaml.org,2002:int",
  format: "TIME",
  test: /^[-+]?[0-9][0-9_]*(?::[0-5]?[0-9])+$/,
  resolve: (str, _onError, { intAsBigInt }) => parseSexagesimal(str, intAsBigInt),
  stringify: stringifySexagesimal
};
var floatTime = {
  identify: (value) => typeof value === "number",
  default: true,
  tag: "tag:yaml.org,2002:float",
  format: "TIME",
  test: /^[-+]?[0-9][0-9_]*(?::[0-5]?[0-9])+\.[0-9_]*$/,
  resolve: (str) => parseSexagesimal(str, false),
  stringify: stringifySexagesimal
};
var timestamp = {
  identify: (value) => value instanceof Date,
  default: true,
  tag: "tag:yaml.org,2002:timestamp",
  // If the time zone is omitted, the timestamp is assumed to be specified in UTC. The time part
  // may be omitted altogether, resulting in a date format. In such a case, the time part is
  // assumed to be 00:00:00Z (start of day, UTC).
  test: RegExp("^([0-9]{4})-([0-9]{1,2})-([0-9]{1,2})(?:(?:t|T|[ \\t]+)([0-9]{1,2}):([0-9]{1,2}):([0-9]{1,2}(\\.[0-9]+)?)(?:[ \\t]*(Z|[-+][012]?[0-9](?::[0-9]{2})?))?)?$"),
  resolve(str) {
    const match = str.match(timestamp.test);
    if (!match)
      throw new Error("!!timestamp expects a date, starting with yyyy-mm-dd");
    const [, year, month, day, hour, minute, second] = match.map(Number);
    const millisec = match[7] ? Number((match[7] + "00").substr(1, 3)) : 0;
    let date = Date.UTC(year, month - 1, day, hour || 0, minute || 0, second || 0, millisec);
    const tz = match[8];
    if (tz && tz !== "Z") {
      let d = parseSexagesimal(tz, false);
      if (Math.abs(d) < 30)
        d *= 60;
      date -= 6e4 * d;
    }
    return new Date(date);
  },
  stringify: ({ value }) => {
    var _a;
    return (_a = value == null ? void 0 : value.toISOString().replace(/(T00:00:00)?\.000Z$/, "")) != null ? _a : "";
  }
};

// node_modules/yaml/browser/dist/schema/yaml-1.1/schema.js
var schema3 = [
  map,
  seq,
  string,
  nullTag,
  trueTag,
  falseTag,
  intBin,
  intOct2,
  int2,
  intHex2,
  floatNaN2,
  floatExp2,
  float2,
  binary,
  merge,
  omap,
  pairs,
  set,
  intTime,
  floatTime,
  timestamp
];

// node_modules/yaml/browser/dist/schema/tags.js
var schemas = /* @__PURE__ */ new Map([
  ["core", schema],
  ["failsafe", [map, seq, string]],
  ["json", schema2],
  ["yaml11", schema3],
  ["yaml-1.1", schema3]
]);
var tagsByName = {
  binary,
  bool: boolTag,
  float,
  floatExp,
  floatNaN,
  floatTime,
  int,
  intHex,
  intOct,
  intTime,
  map,
  merge,
  null: nullTag,
  omap,
  pairs,
  seq,
  set,
  timestamp
};
var coreKnownTags = {
  "tag:yaml.org,2002:binary": binary,
  "tag:yaml.org,2002:merge": merge,
  "tag:yaml.org,2002:omap": omap,
  "tag:yaml.org,2002:pairs": pairs,
  "tag:yaml.org,2002:set": set,
  "tag:yaml.org,2002:timestamp": timestamp
};
function getTags(customTags, schemaName, addMergeTag) {
  const schemaTags = schemas.get(schemaName);
  if (schemaTags && !customTags) {
    return addMergeTag && !schemaTags.includes(merge) ? schemaTags.concat(merge) : schemaTags.slice();
  }
  let tags = schemaTags;
  if (!tags) {
    if (Array.isArray(customTags))
      tags = [];
    else {
      const keys = Array.from(schemas.keys()).filter((key) => key !== "yaml11").map((key) => JSON.stringify(key)).join(", ");
      throw new Error(`Unknown schema "${schemaName}"; use one of ${keys} or define customTags array`);
    }
  }
  if (Array.isArray(customTags)) {
    for (const tag of customTags)
      tags = tags.concat(tag);
  } else if (typeof customTags === "function") {
    tags = customTags(tags.slice());
  }
  if (addMergeTag)
    tags = tags.concat(merge);
  return tags.reduce((tags2, tag) => {
    const tagObj = typeof tag === "string" ? tagsByName[tag] : tag;
    if (!tagObj) {
      const tagName = JSON.stringify(tag);
      const keys = Object.keys(tagsByName).map((key) => JSON.stringify(key)).join(", ");
      throw new Error(`Unknown custom tag ${tagName}; use one of ${keys}`);
    }
    if (!tags2.includes(tagObj))
      tags2.push(tagObj);
    return tags2;
  }, []);
}

// node_modules/yaml/browser/dist/schema/Schema.js
var sortMapEntriesByKey = (a, b) => a.key < b.key ? -1 : a.key > b.key ? 1 : 0;
var Schema = class _Schema {
  constructor({ compat, customTags, merge: merge2, resolveKnownTags, schema: schema4, sortMapEntries, toStringDefaults }) {
    this.compat = Array.isArray(compat) ? getTags(compat, "compat") : compat ? getTags(null, compat) : null;
    this.name = typeof schema4 === "string" && schema4 || "core";
    this.knownTags = resolveKnownTags ? coreKnownTags : {};
    this.tags = getTags(customTags, this.name, merge2);
    this.toStringOptions = toStringDefaults != null ? toStringDefaults : null;
    Object.defineProperty(this, MAP, { value: map });
    Object.defineProperty(this, SCALAR, { value: string });
    Object.defineProperty(this, SEQ, { value: seq });
    this.sortMapEntries = typeof sortMapEntries === "function" ? sortMapEntries : sortMapEntries === true ? sortMapEntriesByKey : null;
  }
  clone() {
    const copy = Object.create(_Schema.prototype, Object.getOwnPropertyDescriptors(this));
    copy.tags = this.tags.slice();
    return copy;
  }
};

// node_modules/yaml/browser/dist/stringify/stringifyDocument.js
function stringifyDocument(doc, options) {
  var _a;
  const lines = [];
  let hasDirectives = options.directives === true;
  if (options.directives !== false && doc.directives) {
    const dir = doc.directives.toString(doc);
    if (dir) {
      lines.push(dir);
      hasDirectives = true;
    } else if (doc.directives.docStart)
      hasDirectives = true;
  }
  if (hasDirectives)
    lines.push("---");
  const ctx = createStringifyContext(doc, options);
  const { commentString } = ctx.options;
  if (doc.commentBefore) {
    if (lines.length !== 1)
      lines.unshift("");
    const cs = commentString(doc.commentBefore);
    lines.unshift(indentComment(cs, ""));
  }
  let chompKeep = false;
  let contentComment = null;
  if (doc.contents) {
    if (isNode(doc.contents)) {
      if (doc.contents.spaceBefore && hasDirectives)
        lines.push("");
      if (doc.contents.commentBefore) {
        const cs = commentString(doc.contents.commentBefore);
        lines.push(indentComment(cs, ""));
      }
      ctx.forceBlockIndent = !!doc.comment;
      contentComment = doc.contents.comment;
    }
    const onChompKeep = contentComment ? void 0 : () => chompKeep = true;
    let body = stringify(doc.contents, ctx, () => contentComment = null, onChompKeep);
    if (contentComment)
      body += lineComment(body, "", commentString(contentComment));
    if ((body[0] === "|" || body[0] === ">") && lines[lines.length - 1] === "---") {
      lines[lines.length - 1] = `--- ${body}`;
    } else
      lines.push(body);
  } else {
    lines.push(stringify(doc.contents, ctx));
  }
  if ((_a = doc.directives) == null ? void 0 : _a.docEnd) {
    if (doc.comment) {
      const cs = commentString(doc.comment);
      if (cs.includes("\n")) {
        lines.push("...");
        lines.push(indentComment(cs, ""));
      } else {
        lines.push(`... ${cs}`);
      }
    } else {
      lines.push("...");
    }
  } else {
    let dc = doc.comment;
    if (dc && chompKeep)
      dc = dc.replace(/^\n+/, "");
    if (dc) {
      if ((!chompKeep || contentComment) && lines[lines.length - 1] !== "")
        lines.push("");
      lines.push(indentComment(commentString(dc), ""));
    }
  }
  return lines.join("\n") + "\n";
}

// node_modules/yaml/browser/dist/doc/Document.js
var Document = class _Document {
  constructor(value, replacer, options) {
    this.commentBefore = null;
    this.comment = null;
    this.errors = [];
    this.warnings = [];
    Object.defineProperty(this, NODE_TYPE, { value: DOC });
    let _replacer = null;
    if (typeof replacer === "function" || Array.isArray(replacer)) {
      _replacer = replacer;
    } else if (options === void 0 && replacer) {
      options = replacer;
      replacer = void 0;
    }
    const opt = Object.assign({
      intAsBigInt: false,
      keepSourceTokens: false,
      logLevel: "warn",
      prettyErrors: true,
      strict: true,
      stringKeys: false,
      uniqueKeys: true,
      version: "1.2"
    }, options);
    this.options = opt;
    let { version } = opt;
    if (options == null ? void 0 : options._directives) {
      this.directives = options._directives.atDocument();
      if (this.directives.yaml.explicit)
        version = this.directives.yaml.version;
    } else
      this.directives = new Directives({ version });
    this.setSchema(version, options);
    this.contents = value === void 0 ? null : this.createNode(value, _replacer, options);
  }
  /**
   * Create a deep copy of this Document and its contents.
   *
   * Custom Node values that inherit from `Object` still refer to their original instances.
   */
  clone() {
    const copy = Object.create(_Document.prototype, {
      [NODE_TYPE]: { value: DOC }
    });
    copy.commentBefore = this.commentBefore;
    copy.comment = this.comment;
    copy.errors = this.errors.slice();
    copy.warnings = this.warnings.slice();
    copy.options = Object.assign({}, this.options);
    if (this.directives)
      copy.directives = this.directives.clone();
    copy.schema = this.schema.clone();
    copy.contents = isNode(this.contents) ? this.contents.clone(copy.schema) : this.contents;
    if (this.range)
      copy.range = this.range.slice();
    return copy;
  }
  /** Adds a value to the document. */
  add(value) {
    if (assertCollection(this.contents))
      this.contents.add(value);
  }
  /** Adds a value to the document. */
  addIn(path, value) {
    if (assertCollection(this.contents))
      this.contents.addIn(path, value);
  }
  /**
   * Create a new `Alias` node, ensuring that the target `node` has the required anchor.
   *
   * If `node` already has an anchor, `name` is ignored.
   * Otherwise, the `node.anchor` value will be set to `name`,
   * or if an anchor with that name is already present in the document,
   * `name` will be used as a prefix for a new unique anchor.
   * If `name` is undefined, the generated anchor will use 'a' as a prefix.
   */
  createAlias(node, name) {
    if (!node.anchor) {
      const prev = anchorNames(this);
      node.anchor = // eslint-disable-next-line @typescript-eslint/prefer-nullish-coalescing
      !name || prev.has(name) ? findNewAnchor(name || "a", prev) : name;
    }
    return new Alias(node.anchor);
  }
  createNode(value, replacer, options) {
    let _replacer = void 0;
    if (typeof replacer === "function") {
      value = replacer.call({ "": value }, "", value);
      _replacer = replacer;
    } else if (Array.isArray(replacer)) {
      const keyToStr = (v) => typeof v === "number" || v instanceof String || v instanceof Number;
      const asStr = replacer.filter(keyToStr).map(String);
      if (asStr.length > 0)
        replacer = replacer.concat(asStr);
      _replacer = replacer;
    } else if (options === void 0 && replacer) {
      options = replacer;
      replacer = void 0;
    }
    const { aliasDuplicateObjects, anchorPrefix, flow, keepUndefined, onTagObj, tag } = options != null ? options : {};
    const { onAnchor, setAnchors, sourceObjects } = createNodeAnchors(
      this,
      // eslint-disable-next-line @typescript-eslint/prefer-nullish-coalescing
      anchorPrefix || "a"
    );
    const ctx = {
      aliasDuplicateObjects: aliasDuplicateObjects != null ? aliasDuplicateObjects : true,
      keepUndefined: keepUndefined != null ? keepUndefined : false,
      onAnchor,
      onTagObj,
      replacer: _replacer,
      schema: this.schema,
      sourceObjects
    };
    const node = createNode(value, tag, ctx);
    if (flow && isCollection(node))
      node.flow = true;
    setAnchors();
    return node;
  }
  /**
   * Convert a key and a value into a `Pair` using the current schema,
   * recursively wrapping all values as `Scalar` or `Collection` nodes.
   */
  createPair(key, value, options = {}) {
    const k = this.createNode(key, null, options);
    const v = this.createNode(value, null, options);
    return new Pair(k, v);
  }
  /**
   * Removes a value from the document.
   * @returns `true` if the item was found and removed.
   */
  delete(key) {
    return assertCollection(this.contents) ? this.contents.delete(key) : false;
  }
  /**
   * Removes a value from the document.
   * @returns `true` if the item was found and removed.
   */
  deleteIn(path) {
    if (isEmptyPath(path)) {
      if (this.contents == null)
        return false;
      this.contents = null;
      return true;
    }
    return assertCollection(this.contents) ? this.contents.deleteIn(path) : false;
  }
  /**
   * Returns item at `key`, or `undefined` if not found. By default unwraps
   * scalar values from their surrounding node; to disable set `keepScalar` to
   * `true` (collections are always returned intact).
   */
  get(key, keepScalar) {
    return isCollection(this.contents) ? this.contents.get(key, keepScalar) : void 0;
  }
  /**
   * Returns item at `path`, or `undefined` if not found. By default unwraps
   * scalar values from their surrounding node; to disable set `keepScalar` to
   * `true` (collections are always returned intact).
   */
  getIn(path, keepScalar) {
    if (isEmptyPath(path))
      return !keepScalar && isScalar(this.contents) ? this.contents.value : this.contents;
    return isCollection(this.contents) ? this.contents.getIn(path, keepScalar) : void 0;
  }
  /**
   * Checks if the document includes a value with the key `key`.
   */
  has(key) {
    return isCollection(this.contents) ? this.contents.has(key) : false;
  }
  /**
   * Checks if the document includes a value at `path`.
   */
  hasIn(path) {
    if (isEmptyPath(path))
      return this.contents !== void 0;
    return isCollection(this.contents) ? this.contents.hasIn(path) : false;
  }
  /**
   * Sets a value in this document. For `!!set`, `value` needs to be a
   * boolean to add/remove the item from the set.
   */
  set(key, value) {
    if (this.contents == null) {
      this.contents = collectionFromPath(this.schema, [key], value);
    } else if (assertCollection(this.contents)) {
      this.contents.set(key, value);
    }
  }
  /**
   * Sets a value in this document. For `!!set`, `value` needs to be a
   * boolean to add/remove the item from the set.
   */
  setIn(path, value) {
    if (isEmptyPath(path)) {
      this.contents = value;
    } else if (this.contents == null) {
      this.contents = collectionFromPath(this.schema, Array.from(path), value);
    } else if (assertCollection(this.contents)) {
      this.contents.setIn(path, value);
    }
  }
  /**
   * Change the YAML version and schema used by the document.
   * A `null` version disables support for directives, explicit tags, anchors, and aliases.
   * It also requires the `schema` option to be given as a `Schema` instance value.
   *
   * Overrides all previously set schema options.
   */
  setSchema(version, options = {}) {
    if (typeof version === "number")
      version = String(version);
    let opt;
    switch (version) {
      case "1.1":
        if (this.directives)
          this.directives.yaml.version = "1.1";
        else
          this.directives = new Directives({ version: "1.1" });
        opt = { resolveKnownTags: false, schema: "yaml-1.1" };
        break;
      case "1.2":
      case "next":
        if (this.directives)
          this.directives.yaml.version = version;
        else
          this.directives = new Directives({ version });
        opt = { resolveKnownTags: true, schema: "core" };
        break;
      case null:
        if (this.directives)
          delete this.directives;
        opt = null;
        break;
      default: {
        const sv = JSON.stringify(version);
        throw new Error(`Expected '1.1', '1.2' or null as first argument, but found: ${sv}`);
      }
    }
    if (options.schema instanceof Object)
      this.schema = options.schema;
    else if (opt)
      this.schema = new Schema(Object.assign(opt, options));
    else
      throw new Error(`With a null YAML version, the { schema: Schema } option is required`);
  }
  // json & jsonArg are only used from toJSON()
  toJS({ json, jsonArg, mapAsMap, maxAliasCount, onAnchor, reviver } = {}) {
    const ctx = {
      anchors: /* @__PURE__ */ new Map(),
      doc: this,
      keep: !json,
      mapAsMap: mapAsMap === true,
      mapKeyWarned: false,
      maxAliasCount: typeof maxAliasCount === "number" ? maxAliasCount : 100
    };
    const res = toJS(this.contents, jsonArg != null ? jsonArg : "", ctx);
    if (typeof onAnchor === "function")
      for (const { count, res: res2 } of ctx.anchors.values())
        onAnchor(res2, count);
    return typeof reviver === "function" ? applyReviver(reviver, { "": res }, "", res) : res;
  }
  /**
   * A JSON representation of the document `contents`.
   *
   * @param jsonArg Used by `JSON.stringify` to indicate the array index or
   *   property name.
   */
  toJSON(jsonArg, onAnchor) {
    return this.toJS({ json: true, jsonArg, mapAsMap: false, onAnchor });
  }
  /** A YAML representation of the document. */
  toString(options = {}) {
    if (this.errors.length > 0)
      throw new Error("Document with errors cannot be stringified");
    if ("indent" in options && (!Number.isInteger(options.indent) || Number(options.indent) <= 0)) {
      const s = JSON.stringify(options.indent);
      throw new Error(`"indent" option must be a positive integer, not ${s}`);
    }
    return stringifyDocument(this, options);
  }
};
function assertCollection(contents) {
  if (isCollection(contents))
    return true;
  throw new Error("Expected a YAML collection as document contents");
}

// node_modules/yaml/browser/dist/errors.js
var YAMLError = class extends Error {
  constructor(name, pos, code, message) {
    super();
    this.name = name;
    this.code = code;
    this.message = message;
    this.pos = pos;
  }
};
var YAMLParseError = class extends YAMLError {
  constructor(pos, code, message) {
    super("YAMLParseError", pos, code, message);
  }
};
var YAMLWarning = class extends YAMLError {
  constructor(pos, code, message) {
    super("YAMLWarning", pos, code, message);
  }
};
var prettifyError = (src, lc) => (error) => {
  if (error.pos[0] === -1)
    return;
  error.linePos = error.pos.map((pos) => lc.linePos(pos));
  const { line, col } = error.linePos[0];
  error.message += ` at line ${line}, column ${col}`;
  let ci = col - 1;
  let lineStr = src.substring(lc.lineStarts[line - 1], lc.lineStarts[line]).replace(/[\n\r]+$/, "");
  if (ci >= 60 && lineStr.length > 80) {
    const trimStart = Math.min(ci - 39, lineStr.length - 79);
    lineStr = "\u2026" + lineStr.substring(trimStart);
    ci -= trimStart - 1;
  }
  if (lineStr.length > 80)
    lineStr = lineStr.substring(0, 79) + "\u2026";
  if (line > 1 && /^ *$/.test(lineStr.substring(0, ci))) {
    let prev = src.substring(lc.lineStarts[line - 2], lc.lineStarts[line - 1]);
    if (prev.length > 80)
      prev = prev.substring(0, 79) + "\u2026\n";
    lineStr = prev + lineStr;
  }
  if (/[^ ]/.test(lineStr)) {
    let count = 1;
    const end = error.linePos[1];
    if ((end == null ? void 0 : end.line) === line && end.col > col) {
      count = Math.max(1, Math.min(end.col - col, 80 - ci));
    }
    const pointer = " ".repeat(ci) + "^".repeat(count);
    error.message += `:

${lineStr}
${pointer}
`;
  }
};

// node_modules/yaml/browser/dist/compose/resolve-props.js
function resolveProps(tokens, { flow, indicator, next, offset, onError, parentIndent, startOnNewline }) {
  let spaceBefore = false;
  let atNewline = startOnNewline;
  let hasSpace = startOnNewline;
  let comment = "";
  let commentSep = "";
  let hasNewline = false;
  let reqSpace = false;
  let tab = null;
  let anchor = null;
  let tag = null;
  let newlineAfterProp = null;
  let comma = null;
  let found = null;
  let start = null;
  for (const token of tokens) {
    if (reqSpace) {
      if (token.type !== "space" && token.type !== "newline" && token.type !== "comma")
        onError(token.offset, "MISSING_CHAR", "Tags and anchors must be separated from the next token by white space");
      reqSpace = false;
    }
    if (tab) {
      if (atNewline && token.type !== "comment" && token.type !== "newline") {
        onError(tab, "TAB_AS_INDENT", "Tabs are not allowed as indentation");
      }
      tab = null;
    }
    switch (token.type) {
      case "space":
        if (!flow && (indicator !== "doc-start" || (next == null ? void 0 : next.type) !== "flow-collection") && token.source.includes("	")) {
          tab = token;
        }
        hasSpace = true;
        break;
      case "comment": {
        if (!hasSpace)
          onError(token, "MISSING_CHAR", "Comments must be separated from other tokens by white space characters");
        const cb = token.source.substring(1) || " ";
        if (!comment)
          comment = cb;
        else
          comment += commentSep + cb;
        commentSep = "";
        atNewline = false;
        break;
      }
      case "newline":
        if (atNewline) {
          if (comment)
            comment += token.source;
          else if (!found || indicator !== "seq-item-ind")
            spaceBefore = true;
        } else
          commentSep += token.source;
        atNewline = true;
        hasNewline = true;
        if (anchor || tag)
          newlineAfterProp = token;
        hasSpace = true;
        break;
      case "anchor":
        if (anchor)
          onError(token, "MULTIPLE_ANCHORS", "A node can have at most one anchor");
        if (token.source.endsWith(":"))
          onError(token.offset + token.source.length - 1, "BAD_ALIAS", "Anchor ending in : is ambiguous", true);
        anchor = token;
        start != null ? start : start = token.offset;
        atNewline = false;
        hasSpace = false;
        reqSpace = true;
        break;
      case "tag": {
        if (tag)
          onError(token, "MULTIPLE_TAGS", "A node can have at most one tag");
        tag = token;
        start != null ? start : start = token.offset;
        atNewline = false;
        hasSpace = false;
        reqSpace = true;
        break;
      }
      case indicator:
        if (anchor || tag)
          onError(token, "BAD_PROP_ORDER", `Anchors and tags must be after the ${token.source} indicator`);
        if (found)
          onError(token, "UNEXPECTED_TOKEN", `Unexpected ${token.source} in ${flow != null ? flow : "collection"}`);
        found = token;
        atNewline = indicator === "seq-item-ind" || indicator === "explicit-key-ind";
        hasSpace = false;
        break;
      case "comma":
        if (flow) {
          if (comma)
            onError(token, "UNEXPECTED_TOKEN", `Unexpected , in ${flow}`);
          comma = token;
          atNewline = false;
          hasSpace = false;
          break;
        }
      // else fallthrough
      default:
        onError(token, "UNEXPECTED_TOKEN", `Unexpected ${token.type} token`);
        atNewline = false;
        hasSpace = false;
    }
  }
  const last = tokens[tokens.length - 1];
  const end = last ? last.offset + last.source.length : offset;
  if (reqSpace && next && next.type !== "space" && next.type !== "newline" && next.type !== "comma" && (next.type !== "scalar" || next.source !== "")) {
    onError(next.offset, "MISSING_CHAR", "Tags and anchors must be separated from the next token by white space");
  }
  if (tab && (atNewline && tab.indent <= parentIndent || (next == null ? void 0 : next.type) === "block-map" || (next == null ? void 0 : next.type) === "block-seq"))
    onError(tab, "TAB_AS_INDENT", "Tabs are not allowed as indentation");
  return {
    comma,
    found,
    spaceBefore,
    comment,
    hasNewline,
    anchor,
    tag,
    newlineAfterProp,
    end,
    start: start != null ? start : end
  };
}

// node_modules/yaml/browser/dist/compose/util-contains-newline.js
function containsNewline(key) {
  if (!key)
    return null;
  switch (key.type) {
    case "alias":
    case "scalar":
    case "double-quoted-scalar":
    case "single-quoted-scalar":
      if (key.source.includes("\n"))
        return true;
      if (key.end) {
        for (const st of key.end)
          if (st.type === "newline")
            return true;
      }
      return false;
    case "flow-collection":
      for (const it of key.items) {
        for (const st of it.start)
          if (st.type === "newline")
            return true;
        if (it.sep) {
          for (const st of it.sep)
            if (st.type === "newline")
              return true;
        }
        if (containsNewline(it.key) || containsNewline(it.value))
          return true;
      }
      return false;
    default:
      return true;
  }
}

// node_modules/yaml/browser/dist/compose/util-flow-indent-check.js
function flowIndentCheck(indent2, fc, onError) {
  if ((fc == null ? void 0 : fc.type) === "flow-collection") {
    const end = fc.end[0];
    if (end.indent === indent2 && (end.source === "]" || end.source === "}") && containsNewline(fc)) {
      const msg = "Flow end indicator should be more indented than parent";
      onError(end, "BAD_INDENT", msg, true);
    }
  }
}

// node_modules/yaml/browser/dist/compose/util-map-includes.js
function mapIncludes(ctx, items, search) {
  const { uniqueKeys } = ctx.options;
  if (uniqueKeys === false)
    return false;
  const isEqual = typeof uniqueKeys === "function" ? uniqueKeys : (a, b) => a === b || isScalar(a) && isScalar(b) && a.value === b.value;
  return items.some((pair) => isEqual(pair.key, search));
}

// node_modules/yaml/browser/dist/compose/resolve-block-map.js
var startColMsg = "All mapping items must start at the same column";
function resolveBlockMap({ composeNode: composeNode2, composeEmptyNode: composeEmptyNode2 }, ctx, bm, onError, tag) {
  var _a, _b;
  const NodeClass = (_a = tag == null ? void 0 : tag.nodeClass) != null ? _a : YAMLMap;
  const map2 = new NodeClass(ctx.schema);
  if (ctx.atRoot)
    ctx.atRoot = false;
  let offset = bm.offset;
  let commentEnd = null;
  for (const collItem of bm.items) {
    const { start, key, sep, value } = collItem;
    const keyProps = resolveProps(start, {
      indicator: "explicit-key-ind",
      next: key != null ? key : sep == null ? void 0 : sep[0],
      offset,
      onError,
      parentIndent: bm.indent,
      startOnNewline: true
    });
    const implicitKey = !keyProps.found;
    if (implicitKey) {
      if (key) {
        if (key.type === "block-seq")
          onError(offset, "BLOCK_AS_IMPLICIT_KEY", "A block sequence may not be used as an implicit map key");
        else if ("indent" in key && key.indent !== bm.indent)
          onError(offset, "BAD_INDENT", startColMsg);
      }
      if (!keyProps.anchor && !keyProps.tag && !sep) {
        commentEnd = keyProps.end;
        if (keyProps.comment) {
          if (map2.comment)
            map2.comment += "\n" + keyProps.comment;
          else
            map2.comment = keyProps.comment;
        }
        continue;
      }
      if (keyProps.newlineAfterProp || containsNewline(key)) {
        onError(key != null ? key : start[start.length - 1], "MULTILINE_IMPLICIT_KEY", "Implicit keys need to be on a single line");
      }
    } else if (((_b = keyProps.found) == null ? void 0 : _b.indent) !== bm.indent) {
      onError(offset, "BAD_INDENT", startColMsg);
    }
    ctx.atKey = true;
    const keyStart = keyProps.end;
    const keyNode = key ? composeNode2(ctx, key, keyProps, onError) : composeEmptyNode2(ctx, keyStart, start, null, keyProps, onError);
    if (ctx.schema.compat)
      flowIndentCheck(bm.indent, key, onError);
    ctx.atKey = false;
    if (mapIncludes(ctx, map2.items, keyNode))
      onError(keyStart, "DUPLICATE_KEY", "Map keys must be unique");
    const valueProps = resolveProps(sep != null ? sep : [], {
      indicator: "map-value-ind",
      next: value,
      offset: keyNode.range[2],
      onError,
      parentIndent: bm.indent,
      startOnNewline: !key || key.type === "block-scalar"
    });
    offset = valueProps.end;
    if (valueProps.found) {
      if (implicitKey) {
        if ((value == null ? void 0 : value.type) === "block-map" && !valueProps.hasNewline)
          onError(offset, "BLOCK_AS_IMPLICIT_KEY", "Nested mappings are not allowed in compact mappings");
        if (ctx.options.strict && keyProps.start < valueProps.found.offset - 1024)
          onError(keyNode.range, "KEY_OVER_1024_CHARS", "The : indicator must be at most 1024 chars after the start of an implicit block mapping key");
      }
      const valueNode = value ? composeNode2(ctx, value, valueProps, onError) : composeEmptyNode2(ctx, offset, sep, null, valueProps, onError);
      if (ctx.schema.compat)
        flowIndentCheck(bm.indent, value, onError);
      offset = valueNode.range[2];
      const pair = new Pair(keyNode, valueNode);
      if (ctx.options.keepSourceTokens)
        pair.srcToken = collItem;
      map2.items.push(pair);
    } else {
      if (implicitKey)
        onError(keyNode.range, "MISSING_CHAR", "Implicit map keys need to be followed by map values");
      if (valueProps.comment) {
        if (keyNode.comment)
          keyNode.comment += "\n" + valueProps.comment;
        else
          keyNode.comment = valueProps.comment;
      }
      const pair = new Pair(keyNode);
      if (ctx.options.keepSourceTokens)
        pair.srcToken = collItem;
      map2.items.push(pair);
    }
  }
  if (commentEnd && commentEnd < offset)
    onError(commentEnd, "IMPOSSIBLE", "Map comment with trailing content");
  map2.range = [bm.offset, offset, commentEnd != null ? commentEnd : offset];
  return map2;
}

// node_modules/yaml/browser/dist/compose/resolve-block-seq.js
function resolveBlockSeq({ composeNode: composeNode2, composeEmptyNode: composeEmptyNode2 }, ctx, bs, onError, tag) {
  var _a;
  const NodeClass = (_a = tag == null ? void 0 : tag.nodeClass) != null ? _a : YAMLSeq;
  const seq2 = new NodeClass(ctx.schema);
  if (ctx.atRoot)
    ctx.atRoot = false;
  if (ctx.atKey)
    ctx.atKey = false;
  let offset = bs.offset;
  let commentEnd = null;
  for (const { start, value } of bs.items) {
    const props = resolveProps(start, {
      indicator: "seq-item-ind",
      next: value,
      offset,
      onError,
      parentIndent: bs.indent,
      startOnNewline: true
    });
    if (!props.found) {
      if (props.anchor || props.tag || value) {
        if ((value == null ? void 0 : value.type) === "block-seq")
          onError(props.end, "BAD_INDENT", "All sequence items must start at the same column");
        else
          onError(offset, "MISSING_CHAR", "Sequence item without - indicator");
      } else {
        commentEnd = props.end;
        if (props.comment)
          seq2.comment = props.comment;
        continue;
      }
    }
    const node = value ? composeNode2(ctx, value, props, onError) : composeEmptyNode2(ctx, props.end, start, null, props, onError);
    if (ctx.schema.compat)
      flowIndentCheck(bs.indent, value, onError);
    offset = node.range[2];
    seq2.items.push(node);
  }
  seq2.range = [bs.offset, offset, commentEnd != null ? commentEnd : offset];
  return seq2;
}

// node_modules/yaml/browser/dist/compose/resolve-end.js
function resolveEnd(end, offset, reqSpace, onError) {
  let comment = "";
  if (end) {
    let hasSpace = false;
    let sep = "";
    for (const token of end) {
      const { source, type } = token;
      switch (type) {
        case "space":
          hasSpace = true;
          break;
        case "comment": {
          if (reqSpace && !hasSpace)
            onError(token, "MISSING_CHAR", "Comments must be separated from other tokens by white space characters");
          const cb = source.substring(1) || " ";
          if (!comment)
            comment = cb;
          else
            comment += sep + cb;
          sep = "";
          break;
        }
        case "newline":
          if (comment)
            sep += source;
          hasSpace = true;
          break;
        default:
          onError(token, "UNEXPECTED_TOKEN", `Unexpected ${type} at node end`);
      }
      offset += source.length;
    }
  }
  return { comment, offset };
}

// node_modules/yaml/browser/dist/compose/resolve-flow-collection.js
var blockMsg = "Block collections are not allowed within flow collections";
var isBlock = (token) => token && (token.type === "block-map" || token.type === "block-seq");
function resolveFlowCollection({ composeNode: composeNode2, composeEmptyNode: composeEmptyNode2 }, ctx, fc, onError, tag) {
  var _a, _b, _c;
  const isMap3 = fc.start.source === "{";
  const fcName = isMap3 ? "flow map" : "flow sequence";
  const NodeClass = (_a = tag == null ? void 0 : tag.nodeClass) != null ? _a : isMap3 ? YAMLMap : YAMLSeq;
  const coll = new NodeClass(ctx.schema);
  coll.flow = true;
  const atRoot = ctx.atRoot;
  if (atRoot)
    ctx.atRoot = false;
  if (ctx.atKey)
    ctx.atKey = false;
  let offset = fc.offset + fc.start.source.length;
  for (let i = 0; i < fc.items.length; ++i) {
    const collItem = fc.items[i];
    const { start, key, sep, value } = collItem;
    const props = resolveProps(start, {
      flow: fcName,
      indicator: "explicit-key-ind",
      next: key != null ? key : sep == null ? void 0 : sep[0],
      offset,
      onError,
      parentIndent: fc.indent,
      startOnNewline: false
    });
    if (!props.found) {
      if (!props.anchor && !props.tag && !sep && !value) {
        if (i === 0 && props.comma)
          onError(props.comma, "UNEXPECTED_TOKEN", `Unexpected , in ${fcName}`);
        else if (i < fc.items.length - 1)
          onError(props.start, "UNEXPECTED_TOKEN", `Unexpected empty item in ${fcName}`);
        if (props.comment) {
          if (coll.comment)
            coll.comment += "\n" + props.comment;
          else
            coll.comment = props.comment;
        }
        offset = props.end;
        continue;
      }
      if (!isMap3 && ctx.options.strict && containsNewline(key))
        onError(
          key,
          // checked by containsNewline()
          "MULTILINE_IMPLICIT_KEY",
          "Implicit keys of flow sequence pairs need to be on a single line"
        );
    }
    if (i === 0) {
      if (props.comma)
        onError(props.comma, "UNEXPECTED_TOKEN", `Unexpected , in ${fcName}`);
    } else {
      if (!props.comma)
        onError(props.start, "MISSING_CHAR", `Missing , between ${fcName} items`);
      if (props.comment) {
        let prevItemComment = "";
        loop: for (const st of start) {
          switch (st.type) {
            case "comma":
            case "space":
              break;
            case "comment":
              prevItemComment = st.source.substring(1);
              break loop;
            default:
              break loop;
          }
        }
        if (prevItemComment) {
          let prev = coll.items[coll.items.length - 1];
          if (isPair(prev))
            prev = (_b = prev.value) != null ? _b : prev.key;
          if (prev.comment)
            prev.comment += "\n" + prevItemComment;
          else
            prev.comment = prevItemComment;
          props.comment = props.comment.substring(prevItemComment.length + 1);
        }
      }
    }
    if (!isMap3 && !sep && !props.found) {
      const valueNode = value ? composeNode2(ctx, value, props, onError) : composeEmptyNode2(ctx, props.end, sep, null, props, onError);
      coll.items.push(valueNode);
      offset = valueNode.range[2];
      if (isBlock(value))
        onError(valueNode.range, "BLOCK_IN_FLOW", blockMsg);
    } else {
      ctx.atKey = true;
      const keyStart = props.end;
      const keyNode = key ? composeNode2(ctx, key, props, onError) : composeEmptyNode2(ctx, keyStart, start, null, props, onError);
      if (isBlock(key))
        onError(keyNode.range, "BLOCK_IN_FLOW", blockMsg);
      ctx.atKey = false;
      const valueProps = resolveProps(sep != null ? sep : [], {
        flow: fcName,
        indicator: "map-value-ind",
        next: value,
        offset: keyNode.range[2],
        onError,
        parentIndent: fc.indent,
        startOnNewline: false
      });
      if (valueProps.found) {
        if (!isMap3 && !props.found && ctx.options.strict) {
          if (sep)
            for (const st of sep) {
              if (st === valueProps.found)
                break;
              if (st.type === "newline") {
                onError(st, "MULTILINE_IMPLICIT_KEY", "Implicit keys of flow sequence pairs need to be on a single line");
                break;
              }
            }
          if (props.start < valueProps.found.offset - 1024)
            onError(valueProps.found, "KEY_OVER_1024_CHARS", "The : indicator must be at most 1024 chars after the start of an implicit flow sequence key");
        }
      } else if (value) {
        if ("source" in value && ((_c = value.source) == null ? void 0 : _c[0]) === ":")
          onError(value, "MISSING_CHAR", `Missing space after : in ${fcName}`);
        else
          onError(valueProps.start, "MISSING_CHAR", `Missing , or : between ${fcName} items`);
      }
      const valueNode = value ? composeNode2(ctx, value, valueProps, onError) : valueProps.found ? composeEmptyNode2(ctx, valueProps.end, sep, null, valueProps, onError) : null;
      if (valueNode) {
        if (isBlock(value))
          onError(valueNode.range, "BLOCK_IN_FLOW", blockMsg);
      } else if (valueProps.comment) {
        if (keyNode.comment)
          keyNode.comment += "\n" + valueProps.comment;
        else
          keyNode.comment = valueProps.comment;
      }
      const pair = new Pair(keyNode, valueNode);
      if (ctx.options.keepSourceTokens)
        pair.srcToken = collItem;
      if (isMap3) {
        const map2 = coll;
        if (mapIncludes(ctx, map2.items, keyNode))
          onError(keyStart, "DUPLICATE_KEY", "Map keys must be unique");
        map2.items.push(pair);
      } else {
        const map2 = new YAMLMap(ctx.schema);
        map2.flow = true;
        map2.items.push(pair);
        const endRange = (valueNode != null ? valueNode : keyNode).range;
        map2.range = [keyNode.range[0], endRange[1], endRange[2]];
        coll.items.push(map2);
      }
      offset = valueNode ? valueNode.range[2] : valueProps.end;
    }
  }
  const expectedEnd = isMap3 ? "}" : "]";
  const [ce, ...ee] = fc.end;
  let cePos = offset;
  if ((ce == null ? void 0 : ce.source) === expectedEnd)
    cePos = ce.offset + ce.source.length;
  else {
    const name = fcName[0].toUpperCase() + fcName.substring(1);
    const msg = atRoot ? `${name} must end with a ${expectedEnd}` : `${name} in block collection must be sufficiently indented and end with a ${expectedEnd}`;
    onError(offset, atRoot ? "MISSING_CHAR" : "BAD_INDENT", msg);
    if (ce && ce.source.length !== 1)
      ee.unshift(ce);
  }
  if (ee.length > 0) {
    const end = resolveEnd(ee, cePos, ctx.options.strict, onError);
    if (end.comment) {
      if (coll.comment)
        coll.comment += "\n" + end.comment;
      else
        coll.comment = end.comment;
    }
    coll.range = [fc.offset, cePos, end.offset];
  } else {
    coll.range = [fc.offset, cePos, cePos];
  }
  return coll;
}

// node_modules/yaml/browser/dist/compose/compose-collection.js
function resolveCollection(CN2, ctx, token, onError, tagName, tag) {
  const coll = token.type === "block-map" ? resolveBlockMap(CN2, ctx, token, onError, tag) : token.type === "block-seq" ? resolveBlockSeq(CN2, ctx, token, onError, tag) : resolveFlowCollection(CN2, ctx, token, onError, tag);
  const Coll = coll.constructor;
  if (tagName === "!" || tagName === Coll.tagName) {
    coll.tag = Coll.tagName;
    return coll;
  }
  if (tagName)
    coll.tag = tagName;
  return coll;
}
function composeCollection(CN2, ctx, token, props, onError) {
  var _a, _b, _c;
  const tagToken = props.tag;
  const tagName = !tagToken ? null : ctx.directives.tagName(tagToken.source, (msg) => onError(tagToken, "TAG_RESOLVE_FAILED", msg));
  if (token.type === "block-seq") {
    const { anchor, newlineAfterProp: nl } = props;
    const lastProp = anchor && tagToken ? anchor.offset > tagToken.offset ? anchor : tagToken : anchor != null ? anchor : tagToken;
    if (lastProp && (!nl || nl.offset < lastProp.offset)) {
      const message = "Missing newline after block sequence props";
      onError(lastProp, "MISSING_CHAR", message);
    }
  }
  const expType = token.type === "block-map" ? "map" : token.type === "block-seq" ? "seq" : token.start.source === "{" ? "map" : "seq";
  if (!tagToken || !tagName || tagName === "!" || tagName === YAMLMap.tagName && expType === "map" || tagName === YAMLSeq.tagName && expType === "seq") {
    return resolveCollection(CN2, ctx, token, onError, tagName);
  }
  let tag = ctx.schema.tags.find((t) => t.tag === tagName && t.collection === expType);
  if (!tag) {
    const kt = ctx.schema.knownTags[tagName];
    if ((kt == null ? void 0 : kt.collection) === expType) {
      ctx.schema.tags.push(Object.assign({}, kt, { default: false }));
      tag = kt;
    } else {
      if (kt) {
        onError(tagToken, "BAD_COLLECTION_TYPE", `${kt.tag} used for ${expType} collection, but expects ${(_a = kt.collection) != null ? _a : "scalar"}`, true);
      } else {
        onError(tagToken, "TAG_RESOLVE_FAILED", `Unresolved tag: ${tagName}`, true);
      }
      return resolveCollection(CN2, ctx, token, onError, tagName);
    }
  }
  const coll = resolveCollection(CN2, ctx, token, onError, tagName, tag);
  const res = (_c = (_b = tag.resolve) == null ? void 0 : _b.call(tag, coll, (msg) => onError(tagToken, "TAG_RESOLVE_FAILED", msg), ctx.options)) != null ? _c : coll;
  const node = isNode(res) ? res : new Scalar(res);
  node.range = coll.range;
  node.tag = tagName;
  if (tag == null ? void 0 : tag.format)
    node.format = tag.format;
  return node;
}

// node_modules/yaml/browser/dist/compose/resolve-block-scalar.js
function resolveBlockScalar(ctx, scalar, onError) {
  const start = scalar.offset;
  const header = parseBlockScalarHeader(scalar, ctx.options.strict, onError);
  if (!header)
    return { value: "", type: null, comment: "", range: [start, start, start] };
  const type = header.mode === ">" ? Scalar.BLOCK_FOLDED : Scalar.BLOCK_LITERAL;
  const lines = scalar.source ? splitLines(scalar.source) : [];
  let chompStart = lines.length;
  for (let i = lines.length - 1; i >= 0; --i) {
    const content = lines[i][1];
    if (content === "" || content === "\r")
      chompStart = i;
    else
      break;
  }
  if (chompStart === 0) {
    const value2 = header.chomp === "+" && lines.length > 0 ? "\n".repeat(Math.max(1, lines.length - 1)) : "";
    let end2 = start + header.length;
    if (scalar.source)
      end2 += scalar.source.length;
    return { value: value2, type, comment: header.comment, range: [start, end2, end2] };
  }
  let trimIndent = scalar.indent + header.indent;
  let offset = scalar.offset + header.length;
  let contentStart = 0;
  for (let i = 0; i < chompStart; ++i) {
    const [indent2, content] = lines[i];
    if (content === "" || content === "\r") {
      if (header.indent === 0 && indent2.length > trimIndent)
        trimIndent = indent2.length;
    } else {
      if (indent2.length < trimIndent) {
        const message = "Block scalars with more-indented leading empty lines must use an explicit indentation indicator";
        onError(offset + indent2.length, "MISSING_CHAR", message);
      }
      if (header.indent === 0)
        trimIndent = indent2.length;
      contentStart = i;
      if (trimIndent === 0 && !ctx.atRoot) {
        const message = "Block scalar values in collections must be indented";
        onError(offset, "BAD_INDENT", message);
      }
      break;
    }
    offset += indent2.length + content.length + 1;
  }
  for (let i = lines.length - 1; i >= chompStart; --i) {
    if (lines[i][0].length > trimIndent)
      chompStart = i + 1;
  }
  let value = "";
  let sep = "";
  let prevMoreIndented = false;
  for (let i = 0; i < contentStart; ++i)
    value += lines[i][0].slice(trimIndent) + "\n";
  for (let i = contentStart; i < chompStart; ++i) {
    let [indent2, content] = lines[i];
    offset += indent2.length + content.length + 1;
    const crlf = content[content.length - 1] === "\r";
    if (crlf)
      content = content.slice(0, -1);
    if (content && indent2.length < trimIndent) {
      const src = header.indent ? "explicit indentation indicator" : "first line";
      const message = `Block scalar lines must not be less indented than their ${src}`;
      onError(offset - content.length - (crlf ? 2 : 1), "BAD_INDENT", message);
      indent2 = "";
    }
    if (type === Scalar.BLOCK_LITERAL) {
      value += sep + indent2.slice(trimIndent) + content;
      sep = "\n";
    } else if (indent2.length > trimIndent || content[0] === "	") {
      if (sep === " ")
        sep = "\n";
      else if (!prevMoreIndented && sep === "\n")
        sep = "\n\n";
      value += sep + indent2.slice(trimIndent) + content;
      sep = "\n";
      prevMoreIndented = true;
    } else if (content === "") {
      if (sep === "\n")
        value += "\n";
      else
        sep = "\n";
    } else {
      value += sep + content;
      sep = " ";
      prevMoreIndented = false;
    }
  }
  switch (header.chomp) {
    case "-":
      break;
    case "+":
      for (let i = chompStart; i < lines.length; ++i)
        value += "\n" + lines[i][0].slice(trimIndent);
      if (value[value.length - 1] !== "\n")
        value += "\n";
      break;
    default:
      value += "\n";
  }
  const end = start + header.length + scalar.source.length;
  return { value, type, comment: header.comment, range: [start, end, end] };
}
function parseBlockScalarHeader({ offset, props }, strict, onError) {
  if (props[0].type !== "block-scalar-header") {
    onError(props[0], "IMPOSSIBLE", "Block scalar header not found");
    return null;
  }
  const { source } = props[0];
  const mode = source[0];
  let indent2 = 0;
  let chomp = "";
  let error = -1;
  for (let i = 1; i < source.length; ++i) {
    const ch = source[i];
    if (!chomp && (ch === "-" || ch === "+"))
      chomp = ch;
    else {
      const n = Number(ch);
      if (!indent2 && n)
        indent2 = n;
      else if (error === -1)
        error = offset + i;
    }
  }
  if (error !== -1)
    onError(error, "UNEXPECTED_TOKEN", `Block scalar header includes extra characters: ${source}`);
  let hasSpace = false;
  let comment = "";
  let length = source.length;
  for (let i = 1; i < props.length; ++i) {
    const token = props[i];
    switch (token.type) {
      case "space":
        hasSpace = true;
      // fallthrough
      case "newline":
        length += token.source.length;
        break;
      case "comment":
        if (strict && !hasSpace) {
          const message = "Comments must be separated from other tokens by white space characters";
          onError(token, "MISSING_CHAR", message);
        }
        length += token.source.length;
        comment = token.source.substring(1);
        break;
      case "error":
        onError(token, "UNEXPECTED_TOKEN", token.message);
        length += token.source.length;
        break;
      /* istanbul ignore next should not happen */
      default: {
        const message = `Unexpected token in block scalar header: ${token.type}`;
        onError(token, "UNEXPECTED_TOKEN", message);
        const ts = token.source;
        if (ts && typeof ts === "string")
          length += ts.length;
      }
    }
  }
  return { mode, indent: indent2, chomp, comment, length };
}
function splitLines(source) {
  const split = source.split(/\n( *)/);
  const first = split[0];
  const m = first.match(/^( *)/);
  const line0 = (m == null ? void 0 : m[1]) ? [m[1], first.slice(m[1].length)] : ["", first];
  const lines = [line0];
  for (let i = 1; i < split.length; i += 2)
    lines.push([split[i], split[i + 1]]);
  return lines;
}

// node_modules/yaml/browser/dist/compose/resolve-flow-scalar.js
function resolveFlowScalar(scalar, strict, onError) {
  const { offset, type, source, end } = scalar;
  let _type;
  let value;
  const _onError = (rel, code, msg) => onError(offset + rel, code, msg);
  switch (type) {
    case "scalar":
      _type = Scalar.PLAIN;
      value = plainValue(source, _onError);
      break;
    case "single-quoted-scalar":
      _type = Scalar.QUOTE_SINGLE;
      value = singleQuotedValue(source, _onError);
      break;
    case "double-quoted-scalar":
      _type = Scalar.QUOTE_DOUBLE;
      value = doubleQuotedValue(source, _onError);
      break;
    /* istanbul ignore next should not happen */
    default:
      onError(scalar, "UNEXPECTED_TOKEN", `Expected a flow scalar value, but found: ${type}`);
      return {
        value: "",
        type: null,
        comment: "",
        range: [offset, offset + source.length, offset + source.length]
      };
  }
  const valueEnd = offset + source.length;
  const re = resolveEnd(end, valueEnd, strict, onError);
  return {
    value,
    type: _type,
    comment: re.comment,
    range: [offset, valueEnd, re.offset]
  };
}
function plainValue(source, onError) {
  let badChar = "";
  switch (source[0]) {
    /* istanbul ignore next should not happen */
    case "	":
      badChar = "a tab character";
      break;
    case ",":
      badChar = "flow indicator character ,";
      break;
    case "%":
      badChar = "directive indicator character %";
      break;
    case "|":
    case ">": {
      badChar = `block scalar indicator ${source[0]}`;
      break;
    }
    case "@":
    case "`": {
      badChar = `reserved character ${source[0]}`;
      break;
    }
  }
  if (badChar)
    onError(0, "BAD_SCALAR_START", `Plain value cannot start with ${badChar}`);
  return foldLines(source);
}
function singleQuotedValue(source, onError) {
  if (source[source.length - 1] !== "'" || source.length === 1)
    onError(source.length, "MISSING_CHAR", "Missing closing 'quote");
  return foldLines(source.slice(1, -1)).replace(/''/g, "'");
}
function foldLines(source) {
  var _a;
  let first, line;
  try {
    first = new RegExp("(.*?)(?<![ 	])[ 	]*\r?\n", "sy");
    line = new RegExp("[ 	]*(.*?)(?:(?<![ 	])[ 	]*)?\r?\n", "sy");
  } catch {
    first = /(.*?)[ \t]*\r?\n/sy;
    line = /[ \t]*(.*?)[ \t]*\r?\n/sy;
  }
  let match = first.exec(source);
  if (!match)
    return source;
  let res = match[1];
  let sep = " ";
  let pos = first.lastIndex;
  line.lastIndex = pos;
  while (match = line.exec(source)) {
    if (match[1] === "") {
      if (sep === "\n")
        res += sep;
      else
        sep = "\n";
    } else {
      res += sep + match[1];
      sep = " ";
    }
    pos = line.lastIndex;
  }
  const last = /[ \t]*(.*)/sy;
  last.lastIndex = pos;
  match = last.exec(source);
  return res + sep + ((_a = match == null ? void 0 : match[1]) != null ? _a : "");
}
function doubleQuotedValue(source, onError) {
  let res = "";
  for (let i = 1; i < source.length - 1; ++i) {
    const ch = source[i];
    if (ch === "\r" && source[i + 1] === "\n")
      continue;
    if (ch === "\n") {
      const { fold, offset } = foldNewline(source, i);
      res += fold;
      i = offset;
    } else if (ch === "\\") {
      let next = source[++i];
      const cc = escapeCodes[next];
      if (cc)
        res += cc;
      else if (next === "\n") {
        next = source[i + 1];
        while (next === " " || next === "	")
          next = source[++i + 1];
      } else if (next === "\r" && source[i + 1] === "\n") {
        next = source[++i + 1];
        while (next === " " || next === "	")
          next = source[++i + 1];
      } else if (next === "x" || next === "u" || next === "U") {
        const length = next === "x" ? 2 : next === "u" ? 4 : 8;
        res += parseCharCode(source, i + 1, length, onError);
        i += length;
      } else {
        const raw = source.substr(i - 1, 2);
        onError(i - 1, "BAD_DQ_ESCAPE", `Invalid escape sequence ${raw}`);
        res += raw;
      }
    } else if (ch === " " || ch === "	") {
      const wsStart = i;
      let next = source[i + 1];
      while (next === " " || next === "	")
        next = source[++i + 1];
      if (next !== "\n" && !(next === "\r" && source[i + 2] === "\n"))
        res += i > wsStart ? source.slice(wsStart, i + 1) : ch;
    } else {
      res += ch;
    }
  }
  if (source[source.length - 1] !== '"' || source.length === 1)
    onError(source.length, "MISSING_CHAR", 'Missing closing "quote');
  return res;
}
function foldNewline(source, offset) {
  let fold = "";
  let ch = source[offset + 1];
  while (ch === " " || ch === "	" || ch === "\n" || ch === "\r") {
    if (ch === "\r" && source[offset + 2] !== "\n")
      break;
    if (ch === "\n")
      fold += "\n";
    offset += 1;
    ch = source[offset + 1];
  }
  if (!fold)
    fold = " ";
  return { fold, offset };
}
var escapeCodes = {
  "0": "\0",
  // null character
  a: "\x07",
  // bell character
  b: "\b",
  // backspace
  e: "\x1B",
  // escape character
  f: "\f",
  // form feed
  n: "\n",
  // line feed
  r: "\r",
  // carriage return
  t: "	",
  // horizontal tab
  v: "\v",
  // vertical tab
  N: "\x85",
  // Unicode next line
  _: "\xA0",
  // Unicode non-breaking space
  L: "\u2028",
  // Unicode line separator
  P: "\u2029",
  // Unicode paragraph separator
  " ": " ",
  '"': '"',
  "/": "/",
  "\\": "\\",
  "	": "	"
};
function parseCharCode(source, offset, length, onError) {
  const cc = source.substr(offset, length);
  const ok = cc.length === length && /^[0-9a-fA-F]+$/.test(cc);
  const code = ok ? parseInt(cc, 16) : NaN;
  try {
    return String.fromCodePoint(code);
  } catch {
    const raw = source.substr(offset - 2, length + 2);
    onError(offset - 2, "BAD_DQ_ESCAPE", `Invalid escape sequence ${raw}`);
    return raw;
  }
}

// node_modules/yaml/browser/dist/compose/compose-scalar.js
function composeScalar(ctx, token, tagToken, onError) {
  const { value, type, comment, range } = token.type === "block-scalar" ? resolveBlockScalar(ctx, token, onError) : resolveFlowScalar(token, ctx.options.strict, onError);
  const tagName = tagToken ? ctx.directives.tagName(tagToken.source, (msg) => onError(tagToken, "TAG_RESOLVE_FAILED", msg)) : null;
  let tag;
  if (ctx.options.stringKeys && ctx.atKey) {
    tag = ctx.schema[SCALAR];
  } else if (tagName)
    tag = findScalarTagByName(ctx.schema, value, tagName, tagToken, onError);
  else if (token.type === "scalar")
    tag = findScalarTagByTest(ctx, value, token, onError);
  else
    tag = ctx.schema[SCALAR];
  let scalar;
  try {
    const res = tag.resolve(value, (msg) => onError(tagToken != null ? tagToken : token, "TAG_RESOLVE_FAILED", msg), ctx.options);
    scalar = isScalar(res) ? res : new Scalar(res);
  } catch (error) {
    const msg = error instanceof Error ? error.message : String(error);
    onError(tagToken != null ? tagToken : token, "TAG_RESOLVE_FAILED", msg);
    scalar = new Scalar(value);
  }
  scalar.range = range;
  scalar.source = value;
  if (type)
    scalar.type = type;
  if (tagName)
    scalar.tag = tagName;
  if (tag.format)
    scalar.format = tag.format;
  if (comment)
    scalar.comment = comment;
  return scalar;
}
function findScalarTagByName(schema4, value, tagName, tagToken, onError) {
  var _a;
  if (tagName === "!")
    return schema4[SCALAR];
  const matchWithTest = [];
  for (const tag of schema4.tags) {
    if (!tag.collection && tag.tag === tagName) {
      if (tag.default && tag.test)
        matchWithTest.push(tag);
      else
        return tag;
    }
  }
  for (const tag of matchWithTest)
    if ((_a = tag.test) == null ? void 0 : _a.test(value))
      return tag;
  const kt = schema4.knownTags[tagName];
  if (kt && !kt.collection) {
    schema4.tags.push(Object.assign({}, kt, { default: false, test: void 0 }));
    return kt;
  }
  onError(tagToken, "TAG_RESOLVE_FAILED", `Unresolved tag: ${tagName}`, tagName !== "tag:yaml.org,2002:str");
  return schema4[SCALAR];
}
function findScalarTagByTest({ atKey, directives, schema: schema4 }, value, token, onError) {
  var _a;
  const tag = schema4.tags.find((tag2) => {
    var _a2;
    return (tag2.default === true || atKey && tag2.default === "key") && ((_a2 = tag2.test) == null ? void 0 : _a2.test(value));
  }) || schema4[SCALAR];
  if (schema4.compat) {
    const compat = (_a = schema4.compat.find((tag2) => {
      var _a2;
      return tag2.default && ((_a2 = tag2.test) == null ? void 0 : _a2.test(value));
    })) != null ? _a : schema4[SCALAR];
    if (tag.tag !== compat.tag) {
      const ts = directives.tagString(tag.tag);
      const cs = directives.tagString(compat.tag);
      const msg = `Value may be parsed as either ${ts} or ${cs}`;
      onError(token, "TAG_RESOLVE_FAILED", msg, true);
    }
  }
  return tag;
}

// node_modules/yaml/browser/dist/compose/util-empty-scalar-position.js
function emptyScalarPosition(offset, before, pos) {
  if (before) {
    pos != null ? pos : pos = before.length;
    for (let i = pos - 1; i >= 0; --i) {
      let st = before[i];
      switch (st.type) {
        case "space":
        case "comment":
        case "newline":
          offset -= st.source.length;
          continue;
      }
      st = before[++i];
      while ((st == null ? void 0 : st.type) === "space") {
        offset += st.source.length;
        st = before[++i];
      }
      break;
    }
  }
  return offset;
}

// node_modules/yaml/browser/dist/compose/compose-node.js
var CN = { composeNode, composeEmptyNode };
function composeNode(ctx, token, props, onError) {
  const atKey = ctx.atKey;
  const { spaceBefore, comment, anchor, tag } = props;
  let node;
  let isSrcToken = true;
  switch (token.type) {
    case "alias":
      node = composeAlias(ctx, token, onError);
      if (anchor || tag)
        onError(token, "ALIAS_PROPS", "An alias node must not specify any properties");
      break;
    case "scalar":
    case "single-quoted-scalar":
    case "double-quoted-scalar":
    case "block-scalar":
      node = composeScalar(ctx, token, tag, onError);
      if (anchor)
        node.anchor = anchor.source.substring(1);
      break;
    case "block-map":
    case "block-seq":
    case "flow-collection":
      try {
        node = composeCollection(CN, ctx, token, props, onError);
        if (anchor)
          node.anchor = anchor.source.substring(1);
      } catch (error) {
        const message = error instanceof Error ? error.message : String(error);
        onError(token, "RESOURCE_EXHAUSTION", message);
      }
      break;
    default: {
      const message = token.type === "error" ? token.message : `Unsupported token (type: ${token.type})`;
      onError(token, "UNEXPECTED_TOKEN", message);
      isSrcToken = false;
    }
  }
  node != null ? node : node = composeEmptyNode(ctx, token.offset, void 0, null, props, onError);
  if (anchor && node.anchor === "")
    onError(anchor, "BAD_ALIAS", "Anchor cannot be an empty string");
  if (atKey && ctx.options.stringKeys && (!isScalar(node) || typeof node.value !== "string" || node.tag && node.tag !== "tag:yaml.org,2002:str")) {
    const msg = "With stringKeys, all keys must be strings";
    onError(tag != null ? tag : token, "NON_STRING_KEY", msg);
  }
  if (spaceBefore)
    node.spaceBefore = true;
  if (comment) {
    if (token.type === "scalar" && token.source === "")
      node.comment = comment;
    else
      node.commentBefore = comment;
  }
  if (ctx.options.keepSourceTokens && isSrcToken)
    node.srcToken = token;
  return node;
}
function composeEmptyNode(ctx, offset, before, pos, { spaceBefore, comment, anchor, tag, end }, onError) {
  const token = {
    type: "scalar",
    offset: emptyScalarPosition(offset, before, pos),
    indent: -1,
    source: ""
  };
  const node = composeScalar(ctx, token, tag, onError);
  if (anchor) {
    node.anchor = anchor.source.substring(1);
    if (node.anchor === "")
      onError(anchor, "BAD_ALIAS", "Anchor cannot be an empty string");
  }
  if (spaceBefore)
    node.spaceBefore = true;
  if (comment) {
    node.comment = comment;
    node.range[2] = end;
  }
  return node;
}
function composeAlias({ options }, { offset, source, end }, onError) {
  const alias = new Alias(source.substring(1));
  if (alias.source === "")
    onError(offset, "BAD_ALIAS", "Alias cannot be an empty string");
  if (alias.source.endsWith(":"))
    onError(offset + source.length - 1, "BAD_ALIAS", "Alias ending in : is ambiguous", true);
  const valueEnd = offset + source.length;
  const re = resolveEnd(end, valueEnd, options.strict, onError);
  alias.range = [offset, valueEnd, re.offset];
  if (re.comment)
    alias.comment = re.comment;
  return alias;
}

// node_modules/yaml/browser/dist/compose/compose-doc.js
function composeDoc(options, directives, { offset, start, value, end }, onError) {
  const opts = Object.assign({ _directives: directives }, options);
  const doc = new Document(void 0, opts);
  const ctx = {
    atKey: false,
    atRoot: true,
    directives: doc.directives,
    options: doc.options,
    schema: doc.schema
  };
  const props = resolveProps(start, {
    indicator: "doc-start",
    next: value != null ? value : end == null ? void 0 : end[0],
    offset,
    onError,
    parentIndent: 0,
    startOnNewline: true
  });
  if (props.found) {
    doc.directives.docStart = true;
    if (value && (value.type === "block-map" || value.type === "block-seq") && !props.hasNewline)
      onError(props.end, "MISSING_CHAR", "Block collection cannot start on same line with directives-end marker");
  }
  doc.contents = value ? composeNode(ctx, value, props, onError) : composeEmptyNode(ctx, props.end, start, null, props, onError);
  const contentEnd = doc.contents.range[2];
  const re = resolveEnd(end, contentEnd, false, onError);
  if (re.comment)
    doc.comment = re.comment;
  doc.range = [offset, contentEnd, re.offset];
  return doc;
}

// node_modules/yaml/browser/dist/compose/composer.js
function getErrorPos(src) {
  if (typeof src === "number")
    return [src, src + 1];
  if (Array.isArray(src))
    return src.length === 2 ? src : [src[0], src[1]];
  const { offset, source } = src;
  return [offset, offset + (typeof source === "string" ? source.length : 1)];
}
function parsePrelude(prelude) {
  var _a;
  let comment = "";
  let atComment = false;
  let afterEmptyLine = false;
  for (let i = 0; i < prelude.length; ++i) {
    const source = prelude[i];
    switch (source[0]) {
      case "#":
        comment += (comment === "" ? "" : afterEmptyLine ? "\n\n" : "\n") + (source.substring(1) || " ");
        atComment = true;
        afterEmptyLine = false;
        break;
      case "%":
        if (((_a = prelude[i + 1]) == null ? void 0 : _a[0]) !== "#")
          i += 1;
        atComment = false;
        break;
      default:
        if (!atComment)
          afterEmptyLine = true;
        atComment = false;
    }
  }
  return { comment, afterEmptyLine };
}
var Composer = class {
  constructor(options = {}) {
    this.doc = null;
    this.atDirectives = false;
    this.prelude = [];
    this.errors = [];
    this.warnings = [];
    this.onError = (source, code, message, warning) => {
      const pos = getErrorPos(source);
      if (warning)
        this.warnings.push(new YAMLWarning(pos, code, message));
      else
        this.errors.push(new YAMLParseError(pos, code, message));
    };
    this.directives = new Directives({ version: options.version || "1.2" });
    this.options = options;
  }
  decorate(doc, afterDoc) {
    const { comment, afterEmptyLine } = parsePrelude(this.prelude);
    if (comment) {
      const dc = doc.contents;
      if (afterDoc) {
        doc.comment = doc.comment ? `${doc.comment}
${comment}` : comment;
      } else if (afterEmptyLine || doc.directives.docStart || !dc) {
        doc.commentBefore = comment;
      } else if (isCollection(dc) && !dc.flow && dc.items.length > 0) {
        let it = dc.items[0];
        if (isPair(it))
          it = it.key;
        const cb = it.commentBefore;
        it.commentBefore = cb ? `${comment}
${cb}` : comment;
      } else {
        const cb = dc.commentBefore;
        dc.commentBefore = cb ? `${comment}
${cb}` : comment;
      }
    }
    if (afterDoc) {
      for (let i = 0; i < this.errors.length; ++i)
        doc.errors.push(this.errors[i]);
      for (let i = 0; i < this.warnings.length; ++i)
        doc.warnings.push(this.warnings[i]);
    } else {
      doc.errors = this.errors;
      doc.warnings = this.warnings;
    }
    this.prelude = [];
    this.errors = [];
    this.warnings = [];
  }
  /**
   * Current stream status information.
   *
   * Mostly useful at the end of input for an empty stream.
   */
  streamInfo() {
    return {
      comment: parsePrelude(this.prelude).comment,
      directives: this.directives,
      errors: this.errors,
      warnings: this.warnings
    };
  }
  /**
   * Compose tokens into documents.
   *
   * @param forceDoc - If the stream contains no document, still emit a final document including any comments and directives that would be applied to a subsequent document.
   * @param endOffset - Should be set if `forceDoc` is also set, to set the document range end and to indicate errors correctly.
   */
  *compose(tokens, forceDoc = false, endOffset = -1) {
    for (const token of tokens)
      yield* this.next(token);
    yield* this.end(forceDoc, endOffset);
  }
  /** Advance the composer by one CST token. */
  *next(token) {
    switch (token.type) {
      case "directive":
        this.directives.add(token.source, (offset, message, warning) => {
          const pos = getErrorPos(token);
          pos[0] += offset;
          this.onError(pos, "BAD_DIRECTIVE", message, warning);
        });
        this.prelude.push(token.source);
        this.atDirectives = true;
        break;
      case "document": {
        const doc = composeDoc(this.options, this.directives, token, this.onError);
        if (this.atDirectives && !doc.directives.docStart)
          this.onError(token, "MISSING_CHAR", "Missing directives-end/doc-start indicator line");
        this.decorate(doc, false);
        if (this.doc)
          yield this.doc;
        this.doc = doc;
        this.atDirectives = false;
        break;
      }
      case "byte-order-mark":
      case "space":
        break;
      case "comment":
      case "newline":
        this.prelude.push(token.source);
        break;
      case "error": {
        const msg = token.source ? `${token.message}: ${JSON.stringify(token.source)}` : token.message;
        const error = new YAMLParseError(getErrorPos(token), "UNEXPECTED_TOKEN", msg);
        if (this.atDirectives || !this.doc)
          this.errors.push(error);
        else
          this.doc.errors.push(error);
        break;
      }
      case "doc-end": {
        if (!this.doc) {
          const msg = "Unexpected doc-end without preceding document";
          this.errors.push(new YAMLParseError(getErrorPos(token), "UNEXPECTED_TOKEN", msg));
          break;
        }
        this.doc.directives.docEnd = true;
        const end = resolveEnd(token.end, token.offset + token.source.length, this.doc.options.strict, this.onError);
        this.decorate(this.doc, true);
        if (end.comment) {
          const dc = this.doc.comment;
          this.doc.comment = dc ? `${dc}
${end.comment}` : end.comment;
        }
        this.doc.range[2] = end.offset;
        break;
      }
      default:
        this.errors.push(new YAMLParseError(getErrorPos(token), "UNEXPECTED_TOKEN", `Unsupported token ${token.type}`));
    }
  }
  /**
   * Call at end of input to yield any remaining document.
   *
   * @param forceDoc - If the stream contains no document, still emit a final document including any comments and directives that would be applied to a subsequent document.
   * @param endOffset - Should be set if `forceDoc` is also set, to set the document range end and to indicate errors correctly.
   */
  *end(forceDoc = false, endOffset = -1) {
    if (this.doc) {
      this.decorate(this.doc, true);
      yield this.doc;
      this.doc = null;
    } else if (forceDoc) {
      const opts = Object.assign({ _directives: this.directives }, this.options);
      const doc = new Document(void 0, opts);
      if (this.atDirectives)
        this.onError(endOffset, "MISSING_CHAR", "Missing directives-end indicator line");
      doc.range = [0, endOffset, endOffset];
      this.decorate(doc, false);
      yield doc;
    }
  }
};

// node_modules/yaml/browser/dist/parse/cst.js
var cst_exports = {};
__export(cst_exports, {
  BOM: () => BOM,
  DOCUMENT: () => DOCUMENT,
  FLOW_END: () => FLOW_END,
  SCALAR: () => SCALAR2,
  createScalarToken: () => createScalarToken,
  isCollection: () => isCollection2,
  isScalar: () => isScalar2,
  prettyToken: () => prettyToken,
  resolveAsScalar: () => resolveAsScalar,
  setScalarValue: () => setScalarValue,
  stringify: () => stringify2,
  tokenType: () => tokenType,
  visit: () => visit2
});

// node_modules/yaml/browser/dist/parse/cst-scalar.js
function resolveAsScalar(token, strict = true, onError) {
  if (token) {
    const _onError = (pos, code, message) => {
      const offset = typeof pos === "number" ? pos : Array.isArray(pos) ? pos[0] : pos.offset;
      if (onError)
        onError(offset, code, message);
      else
        throw new YAMLParseError([offset, offset + 1], code, message);
    };
    switch (token.type) {
      case "scalar":
      case "single-quoted-scalar":
      case "double-quoted-scalar":
        return resolveFlowScalar(token, strict, _onError);
      case "block-scalar":
        return resolveBlockScalar({ options: { strict } }, token, _onError);
    }
  }
  return null;
}
function createScalarToken(value, context) {
  var _a;
  const { implicitKey = false, indent: indent2, inFlow = false, offset = -1, type = "PLAIN" } = context;
  const source = stringifyString({ type, value }, {
    implicitKey,
    indent: indent2 > 0 ? " ".repeat(indent2) : "",
    inFlow,
    options: { blockQuote: true, lineWidth: -1 }
  });
  const end = (_a = context.end) != null ? _a : [
    { type: "newline", offset: -1, indent: indent2, source: "\n" }
  ];
  switch (source[0]) {
    case "|":
    case ">": {
      const he = source.indexOf("\n");
      const head = source.substring(0, he);
      const body = source.substring(he + 1) + "\n";
      const props = [
        { type: "block-scalar-header", offset, indent: indent2, source: head }
      ];
      if (!addEndtoBlockProps(props, end))
        props.push({ type: "newline", offset: -1, indent: indent2, source: "\n" });
      return { type: "block-scalar", offset, indent: indent2, props, source: body };
    }
    case '"':
      return { type: "double-quoted-scalar", offset, indent: indent2, source, end };
    case "'":
      return { type: "single-quoted-scalar", offset, indent: indent2, source, end };
    default:
      return { type: "scalar", offset, indent: indent2, source, end };
  }
}
function setScalarValue(token, value, context = {}) {
  let { afterKey = false, implicitKey = false, inFlow = false, type } = context;
  let indent2 = "indent" in token ? token.indent : null;
  if (afterKey && typeof indent2 === "number")
    indent2 += 2;
  if (!type)
    switch (token.type) {
      case "single-quoted-scalar":
        type = "QUOTE_SINGLE";
        break;
      case "double-quoted-scalar":
        type = "QUOTE_DOUBLE";
        break;
      case "block-scalar": {
        const header = token.props[0];
        if (header.type !== "block-scalar-header")
          throw new Error("Invalid block scalar header");
        type = header.source[0] === ">" ? "BLOCK_FOLDED" : "BLOCK_LITERAL";
        break;
      }
      default:
        type = "PLAIN";
    }
  const source = stringifyString({ type, value }, {
    implicitKey: implicitKey || indent2 === null,
    indent: indent2 !== null && indent2 > 0 ? " ".repeat(indent2) : "",
    inFlow,
    options: { blockQuote: true, lineWidth: -1 }
  });
  switch (source[0]) {
    case "|":
    case ">":
      setBlockScalarValue(token, source);
      break;
    case '"':
      setFlowScalarValue(token, source, "double-quoted-scalar");
      break;
    case "'":
      setFlowScalarValue(token, source, "single-quoted-scalar");
      break;
    default:
      setFlowScalarValue(token, source, "scalar");
  }
}
function setBlockScalarValue(token, source) {
  const he = source.indexOf("\n");
  const head = source.substring(0, he);
  const body = source.substring(he + 1) + "\n";
  if (token.type === "block-scalar") {
    const header = token.props[0];
    if (header.type !== "block-scalar-header")
      throw new Error("Invalid block scalar header");
    header.source = head;
    token.source = body;
  } else {
    const { offset } = token;
    const indent2 = "indent" in token ? token.indent : -1;
    const props = [
      { type: "block-scalar-header", offset, indent: indent2, source: head }
    ];
    if (!addEndtoBlockProps(props, "end" in token ? token.end : void 0))
      props.push({ type: "newline", offset: -1, indent: indent2, source: "\n" });
    for (const key of Object.keys(token))
      if (key !== "type" && key !== "offset")
        delete token[key];
    Object.assign(token, { type: "block-scalar", indent: indent2, props, source: body });
  }
}
function addEndtoBlockProps(props, end) {
  if (end)
    for (const st of end)
      switch (st.type) {
        case "space":
        case "comment":
          props.push(st);
          break;
        case "newline":
          props.push(st);
          return true;
      }
  return false;
}
function setFlowScalarValue(token, source, type) {
  switch (token.type) {
    case "scalar":
    case "double-quoted-scalar":
    case "single-quoted-scalar":
      token.type = type;
      token.source = source;
      break;
    case "block-scalar": {
      const end = token.props.slice(1);
      let oa = source.length;
      if (token.props[0].type === "block-scalar-header")
        oa -= token.props[0].source.length;
      for (const tok of end)
        tok.offset += oa;
      delete token.props;
      Object.assign(token, { type, source, end });
      break;
    }
    case "block-map":
    case "block-seq": {
      const offset = token.offset + source.length;
      const nl = { type: "newline", offset, indent: token.indent, source: "\n" };
      delete token.items;
      Object.assign(token, { type, source, end: [nl] });
      break;
    }
    default: {
      const indent2 = "indent" in token ? token.indent : -1;
      const end = "end" in token && Array.isArray(token.end) ? token.end.filter((st) => st.type === "space" || st.type === "comment" || st.type === "newline") : [];
      for (const key of Object.keys(token))
        if (key !== "type" && key !== "offset")
          delete token[key];
      Object.assign(token, { type, indent: indent2, source, end });
    }
  }
}

// node_modules/yaml/browser/dist/parse/cst-stringify.js
var stringify2 = (cst) => "type" in cst ? stringifyToken(cst) : stringifyItem(cst);
function stringifyToken(token) {
  switch (token.type) {
    case "block-scalar": {
      let res = "";
      for (const tok of token.props)
        res += stringifyToken(tok);
      return res + token.source;
    }
    case "block-map":
    case "block-seq": {
      let res = "";
      for (const item of token.items)
        res += stringifyItem(item);
      return res;
    }
    case "flow-collection": {
      let res = token.start.source;
      for (const item of token.items)
        res += stringifyItem(item);
      for (const st of token.end)
        res += st.source;
      return res;
    }
    case "document": {
      let res = stringifyItem(token);
      if (token.end)
        for (const st of token.end)
          res += st.source;
      return res;
    }
    default: {
      let res = token.source;
      if ("end" in token && token.end)
        for (const st of token.end)
          res += st.source;
      return res;
    }
  }
}
function stringifyItem({ start, key, sep, value }) {
  let res = "";
  for (const st of start)
    res += st.source;
  if (key)
    res += stringifyToken(key);
  if (sep)
    for (const st of sep)
      res += st.source;
  if (value)
    res += stringifyToken(value);
  return res;
}

// node_modules/yaml/browser/dist/parse/cst-visit.js
var BREAK2 = /* @__PURE__ */ Symbol("break visit");
var SKIP2 = /* @__PURE__ */ Symbol("skip children");
var REMOVE2 = /* @__PURE__ */ Symbol("remove item");
function visit2(cst, visitor) {
  if ("type" in cst && cst.type === "document")
    cst = { start: cst.start, value: cst.value };
  _visit(Object.freeze([]), cst, visitor);
}
visit2.BREAK = BREAK2;
visit2.SKIP = SKIP2;
visit2.REMOVE = REMOVE2;
visit2.itemAtPath = (cst, path) => {
  let item = cst;
  for (const [field, index] of path) {
    const tok = item == null ? void 0 : item[field];
    if (tok && "items" in tok) {
      item = tok.items[index];
    } else
      return void 0;
  }
  return item;
};
visit2.parentCollection = (cst, path) => {
  const parent = visit2.itemAtPath(cst, path.slice(0, -1));
  const field = path[path.length - 1][0];
  const coll = parent == null ? void 0 : parent[field];
  if (coll && "items" in coll)
    return coll;
  throw new Error("Parent collection not found");
};
function _visit(path, item, visitor) {
  let ctrl = visitor(item, path);
  if (typeof ctrl === "symbol")
    return ctrl;
  for (const field of ["key", "value"]) {
    const token = item[field];
    if (token && "items" in token) {
      for (let i = 0; i < token.items.length; ++i) {
        const ci = _visit(Object.freeze(path.concat([[field, i]])), token.items[i], visitor);
        if (typeof ci === "number")
          i = ci - 1;
        else if (ci === BREAK2)
          return BREAK2;
        else if (ci === REMOVE2) {
          token.items.splice(i, 1);
          i -= 1;
        }
      }
      if (typeof ctrl === "function" && field === "key")
        ctrl = ctrl(item, path);
    }
  }
  return typeof ctrl === "function" ? ctrl(item, path) : ctrl;
}

// node_modules/yaml/browser/dist/parse/cst.js
var BOM = "\uFEFF";
var DOCUMENT = "";
var FLOW_END = "";
var SCALAR2 = "";
var isCollection2 = (token) => !!token && "items" in token;
var isScalar2 = (token) => !!token && (token.type === "scalar" || token.type === "single-quoted-scalar" || token.type === "double-quoted-scalar" || token.type === "block-scalar");
function prettyToken(token) {
  switch (token) {
    case BOM:
      return "<BOM>";
    case DOCUMENT:
      return "<DOC>";
    case FLOW_END:
      return "<FLOW_END>";
    case SCALAR2:
      return "<SCALAR>";
    default:
      return JSON.stringify(token);
  }
}
function tokenType(source) {
  switch (source) {
    case BOM:
      return "byte-order-mark";
    case DOCUMENT:
      return "doc-mode";
    case FLOW_END:
      return "flow-error-end";
    case SCALAR2:
      return "scalar";
    case "---":
      return "doc-start";
    case "...":
      return "doc-end";
    case "":
    case "\n":
    case "\r\n":
      return "newline";
    case "-":
      return "seq-item-ind";
    case "?":
      return "explicit-key-ind";
    case ":":
      return "map-value-ind";
    case "{":
      return "flow-map-start";
    case "}":
      return "flow-map-end";
    case "[":
      return "flow-seq-start";
    case "]":
      return "flow-seq-end";
    case ",":
      return "comma";
  }
  switch (source[0]) {
    case " ":
    case "	":
      return "space";
    case "#":
      return "comment";
    case "%":
      return "directive-line";
    case "*":
      return "alias";
    case "&":
      return "anchor";
    case "!":
      return "tag";
    case "'":
      return "single-quoted-scalar";
    case '"':
      return "double-quoted-scalar";
    case "|":
    case ">":
      return "block-scalar-header";
  }
  return null;
}

// node_modules/yaml/browser/dist/parse/lexer.js
function isEmpty(ch) {
  switch (ch) {
    case void 0:
    case " ":
    case "\n":
    case "\r":
    case "	":
      return true;
    default:
      return false;
  }
}
var hexDigits = new Set("0123456789ABCDEFabcdef");
var tagChars = new Set("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-#;/?:@&=+$_.!~*'()");
var flowIndicatorChars = new Set(",[]{}");
var invalidAnchorChars = new Set(" ,[]{}\n\r	");
var isNotAnchorChar = (ch) => !ch || invalidAnchorChars.has(ch);
var Lexer = class {
  constructor() {
    this.atEnd = false;
    this.blockScalarIndent = -1;
    this.blockScalarKeep = false;
    this.buffer = "";
    this.flowKey = false;
    this.flowLevel = 0;
    this.indentNext = 0;
    this.indentValue = 0;
    this.lineEndPos = null;
    this.next = null;
    this.pos = 0;
  }
  /**
   * Generate YAML tokens from the `source` string. If `incomplete`,
   * a part of the last line may be left as a buffer for the next call.
   *
   * @returns A generator of lexical tokens
   */
  *lex(source, incomplete = false) {
    var _a;
    if (source) {
      if (typeof source !== "string")
        throw TypeError("source is not a string");
      this.buffer = this.buffer ? this.buffer + source : source;
      this.lineEndPos = null;
    }
    this.atEnd = !incomplete;
    let next = (_a = this.next) != null ? _a : "stream";
    while (next && (incomplete || this.hasChars(1)))
      next = yield* this.parseNext(next);
  }
  atLineEnd() {
    let i = this.pos;
    let ch = this.buffer[i];
    while (ch === " " || ch === "	")
      ch = this.buffer[++i];
    if (!ch || ch === "#" || ch === "\n")
      return true;
    if (ch === "\r")
      return this.buffer[i + 1] === "\n";
    return false;
  }
  charAt(n) {
    return this.buffer[this.pos + n];
  }
  continueScalar(offset) {
    let ch = this.buffer[offset];
    if (this.indentNext > 0) {
      let indent2 = 0;
      while (ch === " ")
        ch = this.buffer[++indent2 + offset];
      if (ch === "\r") {
        const next = this.buffer[indent2 + offset + 1];
        if (next === "\n" || !next && !this.atEnd)
          return offset + indent2 + 1;
      }
      return ch === "\n" || indent2 >= this.indentNext || !ch && !this.atEnd ? offset + indent2 : -1;
    }
    if (ch === "-" || ch === ".") {
      const dt = this.buffer.substr(offset, 3);
      if ((dt === "---" || dt === "...") && isEmpty(this.buffer[offset + 3]))
        return -1;
    }
    return offset;
  }
  getLine() {
    let end = this.lineEndPos;
    if (typeof end !== "number" || end !== -1 && end < this.pos) {
      end = this.buffer.indexOf("\n", this.pos);
      this.lineEndPos = end;
    }
    if (end === -1)
      return this.atEnd ? this.buffer.substring(this.pos) : null;
    if (this.buffer[end - 1] === "\r")
      end -= 1;
    return this.buffer.substring(this.pos, end);
  }
  hasChars(n) {
    return this.pos + n <= this.buffer.length;
  }
  setNext(state) {
    this.buffer = this.buffer.substring(this.pos);
    this.pos = 0;
    this.lineEndPos = null;
    this.next = state;
    return null;
  }
  peek(n) {
    return this.buffer.substr(this.pos, n);
  }
  *parseNext(next) {
    switch (next) {
      case "stream":
        return yield* this.parseStream();
      case "line-start":
        return yield* this.parseLineStart();
      case "block-start":
        return yield* this.parseBlockStart();
      case "doc":
        return yield* this.parseDocument();
      case "flow":
        return yield* this.parseFlowCollection();
      case "quoted-scalar":
        return yield* this.parseQuotedScalar();
      case "block-scalar":
        return yield* this.parseBlockScalar();
      case "plain-scalar":
        return yield* this.parsePlainScalar();
    }
  }
  *parseStream() {
    let line = this.getLine();
    if (line === null)
      return this.setNext("stream");
    if (line[0] === BOM) {
      yield* this.pushCount(1);
      line = line.substring(1);
    }
    if (line[0] === "%") {
      let dirEnd = line.length;
      let cs = line.indexOf("#");
      while (cs !== -1) {
        const ch = line[cs - 1];
        if (ch === " " || ch === "	") {
          dirEnd = cs - 1;
          break;
        } else {
          cs = line.indexOf("#", cs + 1);
        }
      }
      while (true) {
        const ch = line[dirEnd - 1];
        if (ch === " " || ch === "	")
          dirEnd -= 1;
        else
          break;
      }
      const n = (yield* this.pushCount(dirEnd)) + (yield* this.pushSpaces(true));
      yield* this.pushCount(line.length - n);
      this.pushNewline();
      return "stream";
    }
    if (this.atLineEnd()) {
      const sp = yield* this.pushSpaces(true);
      yield* this.pushCount(line.length - sp);
      yield* this.pushNewline();
      return "stream";
    }
    yield DOCUMENT;
    return yield* this.parseLineStart();
  }
  *parseLineStart() {
    const ch = this.charAt(0);
    if (!ch && !this.atEnd)
      return this.setNext("line-start");
    if (ch === "-" || ch === ".") {
      if (!this.atEnd && !this.hasChars(4))
        return this.setNext("line-start");
      const s = this.peek(3);
      if ((s === "---" || s === "...") && isEmpty(this.charAt(3))) {
        yield* this.pushCount(3);
        this.indentValue = 0;
        this.indentNext = 0;
        return s === "---" ? "doc" : "stream";
      }
    }
    this.indentValue = yield* this.pushSpaces(false);
    if (this.indentNext > this.indentValue && !isEmpty(this.charAt(1)))
      this.indentNext = this.indentValue;
    return yield* this.parseBlockStart();
  }
  *parseBlockStart() {
    const [ch0, ch1] = this.peek(2);
    if (!ch1 && !this.atEnd)
      return this.setNext("block-start");
    if ((ch0 === "-" || ch0 === "?" || ch0 === ":") && isEmpty(ch1)) {
      const n = (yield* this.pushCount(1)) + (yield* this.pushSpaces(true));
      this.indentNext = this.indentValue + 1;
      this.indentValue += n;
      return "block-start";
    }
    return "doc";
  }
  *parseDocument() {
    yield* this.pushSpaces(true);
    const line = this.getLine();
    if (line === null)
      return this.setNext("doc");
    let n = yield* this.pushIndicators();
    switch (line[n]) {
      case "#":
        yield* this.pushCount(line.length - n);
      // fallthrough
      case void 0:
        yield* this.pushNewline();
        return yield* this.parseLineStart();
      case "{":
      case "[":
        yield* this.pushCount(1);
        this.flowKey = false;
        this.flowLevel = 1;
        return "flow";
      case "}":
      case "]":
        yield* this.pushCount(1);
        return "doc";
      case "*":
        yield* this.pushUntil(isNotAnchorChar);
        return "doc";
      case '"':
      case "'":
        return yield* this.parseQuotedScalar();
      case "|":
      case ">":
        n += yield* this.parseBlockScalarHeader();
        n += yield* this.pushSpaces(true);
        yield* this.pushCount(line.length - n);
        yield* this.pushNewline();
        return yield* this.parseBlockScalar();
      default:
        return yield* this.parsePlainScalar();
    }
  }
  *parseFlowCollection() {
    let nl, sp;
    let indent2 = -1;
    do {
      nl = yield* this.pushNewline();
      if (nl > 0) {
        sp = yield* this.pushSpaces(false);
        this.indentValue = indent2 = sp;
      } else {
        sp = 0;
      }
      sp += yield* this.pushSpaces(true);
    } while (nl + sp > 0);
    const line = this.getLine();
    if (line === null)
      return this.setNext("flow");
    if (indent2 !== -1 && indent2 < this.indentNext && line[0] !== "#" || indent2 === 0 && (line.startsWith("---") || line.startsWith("...")) && isEmpty(line[3])) {
      const atFlowEndMarker = indent2 === this.indentNext - 1 && this.flowLevel === 1 && (line[0] === "]" || line[0] === "}");
      if (!atFlowEndMarker) {
        this.flowLevel = 0;
        yield FLOW_END;
        return yield* this.parseLineStart();
      }
    }
    let n = 0;
    while (line[n] === ",") {
      n += yield* this.pushCount(1);
      n += yield* this.pushSpaces(true);
      this.flowKey = false;
    }
    n += yield* this.pushIndicators();
    switch (line[n]) {
      case void 0:
        return "flow";
      case "#":
        yield* this.pushCount(line.length - n);
        return "flow";
      case "{":
      case "[":
        yield* this.pushCount(1);
        this.flowKey = false;
        this.flowLevel += 1;
        return "flow";
      case "}":
      case "]":
        yield* this.pushCount(1);
        this.flowKey = true;
        this.flowLevel -= 1;
        return this.flowLevel ? "flow" : "doc";
      case "*":
        yield* this.pushUntil(isNotAnchorChar);
        return "flow";
      case '"':
      case "'":
        this.flowKey = true;
        return yield* this.parseQuotedScalar();
      case ":": {
        const next = this.charAt(1);
        if (this.flowKey || isEmpty(next) || next === ",") {
          this.flowKey = false;
          yield* this.pushCount(1);
          yield* this.pushSpaces(true);
          return "flow";
        }
      }
      // fallthrough
      default:
        this.flowKey = false;
        return yield* this.parsePlainScalar();
    }
  }
  *parseQuotedScalar() {
    const quote = this.charAt(0);
    let end = this.buffer.indexOf(quote, this.pos + 1);
    if (quote === "'") {
      while (end !== -1 && this.buffer[end + 1] === "'")
        end = this.buffer.indexOf("'", end + 2);
    } else {
      while (end !== -1) {
        let n = 0;
        while (this.buffer[end - 1 - n] === "\\")
          n += 1;
        if (n % 2 === 0)
          break;
        end = this.buffer.indexOf('"', end + 1);
      }
    }
    const qb = this.buffer.substring(0, end);
    let nl = qb.indexOf("\n", this.pos);
    if (nl !== -1) {
      while (nl !== -1) {
        const cs = this.continueScalar(nl + 1);
        if (cs === -1)
          break;
        nl = qb.indexOf("\n", cs);
      }
      if (nl !== -1) {
        end = nl - (qb[nl - 1] === "\r" ? 2 : 1);
      }
    }
    if (end === -1) {
      if (!this.atEnd)
        return this.setNext("quoted-scalar");
      end = this.buffer.length;
    }
    yield* this.pushToIndex(end + 1, false);
    return this.flowLevel ? "flow" : "doc";
  }
  *parseBlockScalarHeader() {
    this.blockScalarIndent = -1;
    this.blockScalarKeep = false;
    let i = this.pos;
    while (true) {
      const ch = this.buffer[++i];
      if (ch === "+")
        this.blockScalarKeep = true;
      else if (ch > "0" && ch <= "9")
        this.blockScalarIndent = Number(ch) - 1;
      else if (ch !== "-")
        break;
    }
    return yield* this.pushUntil((ch) => isEmpty(ch) || ch === "#");
  }
  *parseBlockScalar() {
    let nl = this.pos - 1;
    let indent2 = 0;
    let ch;
    loop: for (let i2 = this.pos; ch = this.buffer[i2]; ++i2) {
      switch (ch) {
        case " ":
          indent2 += 1;
          break;
        case "\n":
          nl = i2;
          indent2 = 0;
          break;
        case "\r": {
          const next = this.buffer[i2 + 1];
          if (!next && !this.atEnd)
            return this.setNext("block-scalar");
          if (next === "\n")
            break;
        }
        // fallthrough
        default:
          break loop;
      }
    }
    if (!ch && !this.atEnd)
      return this.setNext("block-scalar");
    if (indent2 >= this.indentNext) {
      if (this.blockScalarIndent === -1)
        this.indentNext = indent2;
      else {
        this.indentNext = this.blockScalarIndent + (this.indentNext === 0 ? 1 : this.indentNext);
      }
      do {
        const cs = this.continueScalar(nl + 1);
        if (cs === -1)
          break;
        nl = this.buffer.indexOf("\n", cs);
      } while (nl !== -1);
      if (nl === -1) {
        if (!this.atEnd)
          return this.setNext("block-scalar");
        nl = this.buffer.length;
      }
    }
    let i = nl + 1;
    ch = this.buffer[i];
    while (ch === " ")
      ch = this.buffer[++i];
    if (ch === "	") {
      while (ch === "	" || ch === " " || ch === "\r" || ch === "\n")
        ch = this.buffer[++i];
      nl = i - 1;
    } else if (!this.blockScalarKeep) {
      do {
        let i2 = nl - 1;
        let ch2 = this.buffer[i2];
        if (ch2 === "\r")
          ch2 = this.buffer[--i2];
        const lastChar = i2;
        while (ch2 === " ")
          ch2 = this.buffer[--i2];
        if (ch2 === "\n" && i2 >= this.pos && i2 + 1 + indent2 > lastChar)
          nl = i2;
        else
          break;
      } while (true);
    }
    yield SCALAR2;
    yield* this.pushToIndex(nl + 1, true);
    return yield* this.parseLineStart();
  }
  *parsePlainScalar() {
    const inFlow = this.flowLevel > 0;
    let end = this.pos - 1;
    let i = this.pos - 1;
    let ch;
    while (ch = this.buffer[++i]) {
      if (ch === ":") {
        const next = this.buffer[i + 1];
        if (isEmpty(next) || inFlow && flowIndicatorChars.has(next))
          break;
        end = i;
      } else if (isEmpty(ch)) {
        let next = this.buffer[i + 1];
        if (ch === "\r") {
          if (next === "\n") {
            i += 1;
            ch = "\n";
            next = this.buffer[i + 1];
          } else
            end = i;
        }
        if (next === "#" || inFlow && flowIndicatorChars.has(next))
          break;
        if (ch === "\n") {
          const cs = this.continueScalar(i + 1);
          if (cs === -1)
            break;
          i = Math.max(i, cs - 2);
        }
      } else {
        if (inFlow && flowIndicatorChars.has(ch))
          break;
        end = i;
      }
    }
    if (!ch && !this.atEnd)
      return this.setNext("plain-scalar");
    yield SCALAR2;
    yield* this.pushToIndex(end + 1, true);
    return inFlow ? "flow" : "doc";
  }
  *pushCount(n) {
    if (n > 0) {
      yield this.buffer.substr(this.pos, n);
      this.pos += n;
      return n;
    }
    return 0;
  }
  *pushToIndex(i, allowEmpty) {
    const s = this.buffer.slice(this.pos, i);
    if (s) {
      yield s;
      this.pos += s.length;
      return s.length;
    } else if (allowEmpty)
      yield "";
    return 0;
  }
  *pushIndicators() {
    let n = 0;
    loop: while (true) {
      switch (this.charAt(0)) {
        case "!":
          n += yield* this.pushTag();
          n += yield* this.pushSpaces(true);
          continue loop;
        case "&":
          n += yield* this.pushUntil(isNotAnchorChar);
          n += yield* this.pushSpaces(true);
          continue loop;
        case "-":
        // this is an error
        case "?":
        // this is an error outside flow collections
        case ":": {
          const inFlow = this.flowLevel > 0;
          const ch1 = this.charAt(1);
          if (isEmpty(ch1) || inFlow && flowIndicatorChars.has(ch1)) {
            if (!inFlow)
              this.indentNext = this.indentValue + 1;
            else if (this.flowKey)
              this.flowKey = false;
            n += yield* this.pushCount(1);
            n += yield* this.pushSpaces(true);
            continue loop;
          }
        }
      }
      break loop;
    }
    return n;
  }
  *pushTag() {
    if (this.charAt(1) === "<") {
      let i = this.pos + 2;
      let ch = this.buffer[i];
      while (!isEmpty(ch) && ch !== ">")
        ch = this.buffer[++i];
      return yield* this.pushToIndex(ch === ">" ? i + 1 : i, false);
    } else {
      let i = this.pos + 1;
      let ch = this.buffer[i];
      while (ch) {
        if (tagChars.has(ch))
          ch = this.buffer[++i];
        else if (ch === "%" && hexDigits.has(this.buffer[i + 1]) && hexDigits.has(this.buffer[i + 2])) {
          ch = this.buffer[i += 3];
        } else
          break;
      }
      return yield* this.pushToIndex(i, false);
    }
  }
  *pushNewline() {
    const ch = this.buffer[this.pos];
    if (ch === "\n")
      return yield* this.pushCount(1);
    else if (ch === "\r" && this.charAt(1) === "\n")
      return yield* this.pushCount(2);
    else
      return 0;
  }
  *pushSpaces(allowTabs) {
    let i = this.pos - 1;
    let ch;
    do {
      ch = this.buffer[++i];
    } while (ch === " " || allowTabs && ch === "	");
    const n = i - this.pos;
    if (n > 0) {
      yield this.buffer.substr(this.pos, n);
      this.pos = i;
    }
    return n;
  }
  *pushUntil(test) {
    let i = this.pos;
    let ch = this.buffer[i];
    while (!test(ch))
      ch = this.buffer[++i];
    return yield* this.pushToIndex(i, false);
  }
};

// node_modules/yaml/browser/dist/parse/line-counter.js
var LineCounter = class {
  constructor() {
    this.lineStarts = [];
    this.addNewLine = (offset) => this.lineStarts.push(offset);
    this.linePos = (offset) => {
      let low = 0;
      let high = this.lineStarts.length;
      while (low < high) {
        const mid = low + high >> 1;
        if (this.lineStarts[mid] < offset)
          low = mid + 1;
        else
          high = mid;
      }
      if (this.lineStarts[low] === offset)
        return { line: low + 1, col: 1 };
      if (low === 0)
        return { line: 0, col: offset };
      const start = this.lineStarts[low - 1];
      return { line: low, col: offset - start + 1 };
    };
  }
};

// node_modules/yaml/browser/dist/parse/parser.js
function includesToken(list, type) {
  for (let i = 0; i < list.length; ++i)
    if (list[i].type === type)
      return true;
  return false;
}
function findNonEmptyIndex(list) {
  for (let i = 0; i < list.length; ++i) {
    switch (list[i].type) {
      case "space":
      case "comment":
      case "newline":
        break;
      default:
        return i;
    }
  }
  return -1;
}
function isFlowToken(token) {
  switch (token == null ? void 0 : token.type) {
    case "alias":
    case "scalar":
    case "single-quoted-scalar":
    case "double-quoted-scalar":
    case "flow-collection":
      return true;
    default:
      return false;
  }
}
function getPrevProps(parent) {
  var _a;
  switch (parent.type) {
    case "document":
      return parent.start;
    case "block-map": {
      const it = parent.items[parent.items.length - 1];
      return (_a = it.sep) != null ? _a : it.start;
    }
    case "block-seq":
      return parent.items[parent.items.length - 1].start;
    /* istanbul ignore next should not happen */
    default:
      return [];
  }
}
function getFirstKeyStartProps(prev) {
  var _a;
  if (prev.length === 0)
    return [];
  let i = prev.length;
  loop: while (--i >= 0) {
    switch (prev[i].type) {
      case "doc-start":
      case "explicit-key-ind":
      case "map-value-ind":
      case "seq-item-ind":
      case "newline":
        break loop;
    }
  }
  while (((_a = prev[++i]) == null ? void 0 : _a.type) === "space") {
  }
  return prev.splice(i, prev.length);
}
function arrayPushArray(target, source) {
  if (source.length < 1e5)
    Array.prototype.push.apply(target, source);
  else
    for (let i = 0; i < source.length; ++i)
      target.push(source[i]);
}
function fixFlowSeqItems(fc) {
  if (fc.start.type === "flow-seq-start") {
    for (const it of fc.items) {
      if (it.sep && !it.value && !includesToken(it.start, "explicit-key-ind") && !includesToken(it.sep, "map-value-ind")) {
        if (it.key)
          it.value = it.key;
        delete it.key;
        if (isFlowToken(it.value)) {
          if (it.value.end)
            arrayPushArray(it.value.end, it.sep);
          else
            it.value.end = it.sep;
        } else
          arrayPushArray(it.start, it.sep);
        delete it.sep;
      }
    }
  }
}
var Parser = class {
  /**
   * @param onNewLine - If defined, called separately with the start position of
   *   each new line (in `parse()`, including the start of input).
   */
  constructor(onNewLine) {
    this.atNewLine = true;
    this.atScalar = false;
    this.indent = 0;
    this.offset = 0;
    this.onKeyLine = false;
    this.stack = [];
    this.source = "";
    this.type = "";
    this.lexer = new Lexer();
    this.onNewLine = onNewLine;
  }
  /**
   * Parse `source` as a YAML stream.
   * If `incomplete`, a part of the last line may be left as a buffer for the next call.
   *
   * Errors are not thrown, but yielded as `{ type: 'error', message }` tokens.
   *
   * @returns A generator of tokens representing each directive, document, and other structure.
   */
  *parse(source, incomplete = false) {
    if (this.onNewLine && this.offset === 0)
      this.onNewLine(0);
    for (const lexeme of this.lexer.lex(source, incomplete))
      yield* this.next(lexeme);
    if (!incomplete)
      yield* this.end();
  }
  /**
   * Advance the parser by the `source` of one lexical token.
   */
  *next(source) {
    this.source = source;
    if (this.atScalar) {
      this.atScalar = false;
      yield* this.step();
      this.offset += source.length;
      return;
    }
    const type = tokenType(source);
    if (!type) {
      const message = `Not a YAML token: ${source}`;
      yield* this.pop({ type: "error", offset: this.offset, message, source });
      this.offset += source.length;
    } else if (type === "scalar") {
      this.atNewLine = false;
      this.atScalar = true;
      this.type = "scalar";
    } else {
      this.type = type;
      yield* this.step();
      switch (type) {
        case "newline":
          this.atNewLine = true;
          this.indent = 0;
          if (this.onNewLine)
            this.onNewLine(this.offset + source.length);
          break;
        case "space":
          if (this.atNewLine && source[0] === " ")
            this.indent += source.length;
          break;
        case "explicit-key-ind":
        case "map-value-ind":
        case "seq-item-ind":
          if (this.atNewLine)
            this.indent += source.length;
          break;
        case "doc-mode":
        case "flow-error-end":
          return;
        default:
          this.atNewLine = false;
      }
      this.offset += source.length;
    }
  }
  /** Call at end of input to push out any remaining constructions */
  *end() {
    while (this.stack.length > 0)
      yield* this.pop();
  }
  get sourceToken() {
    const st = {
      type: this.type,
      offset: this.offset,
      indent: this.indent,
      source: this.source
    };
    return st;
  }
  *step() {
    const top = this.peek(1);
    if (this.type === "doc-end" && (top == null ? void 0 : top.type) !== "doc-end") {
      while (this.stack.length > 0)
        yield* this.pop();
      this.stack.push({
        type: "doc-end",
        offset: this.offset,
        source: this.source
      });
      return;
    }
    if (!top)
      return yield* this.stream();
    switch (top.type) {
      case "document":
        return yield* this.document(top);
      case "alias":
      case "scalar":
      case "single-quoted-scalar":
      case "double-quoted-scalar":
        return yield* this.scalar(top);
      case "block-scalar":
        return yield* this.blockScalar(top);
      case "block-map":
        return yield* this.blockMap(top);
      case "block-seq":
        return yield* this.blockSequence(top);
      case "flow-collection":
        return yield* this.flowCollection(top);
      case "doc-end":
        return yield* this.documentEnd(top);
    }
    yield* this.pop();
  }
  peek(n) {
    return this.stack[this.stack.length - n];
  }
  *pop(error) {
    const token = error != null ? error : this.stack.pop();
    if (!token) {
      const message = "Tried to pop an empty stack";
      yield { type: "error", offset: this.offset, source: "", message };
    } else if (this.stack.length === 0) {
      yield token;
    } else {
      const top = this.peek(1);
      if (token.type === "block-scalar") {
        token.indent = "indent" in top ? top.indent : 0;
      } else if (token.type === "flow-collection" && top.type === "document") {
        token.indent = 0;
      }
      if (token.type === "flow-collection")
        fixFlowSeqItems(token);
      switch (top.type) {
        case "document":
          top.value = token;
          break;
        case "block-scalar":
          top.props.push(token);
          break;
        case "block-map": {
          const it = top.items[top.items.length - 1];
          if (it.value) {
            top.items.push({ start: [], key: token, sep: [] });
            this.onKeyLine = true;
            return;
          } else if (it.sep) {
            it.value = token;
          } else {
            Object.assign(it, { key: token, sep: [] });
            this.onKeyLine = !it.explicitKey;
            return;
          }
          break;
        }
        case "block-seq": {
          const it = top.items[top.items.length - 1];
          if (it.value)
            top.items.push({ start: [], value: token });
          else
            it.value = token;
          break;
        }
        case "flow-collection": {
          const it = top.items[top.items.length - 1];
          if (!it || it.value)
            top.items.push({ start: [], key: token, sep: [] });
          else if (it.sep)
            it.value = token;
          else
            Object.assign(it, { key: token, sep: [] });
          return;
        }
        /* istanbul ignore next should not happen */
        default:
          yield* this.pop();
          yield* this.pop(token);
      }
      if ((top.type === "document" || top.type === "block-map" || top.type === "block-seq") && (token.type === "block-map" || token.type === "block-seq")) {
        const last = token.items[token.items.length - 1];
        if (last && !last.sep && !last.value && last.start.length > 0 && findNonEmptyIndex(last.start) === -1 && (token.indent === 0 || last.start.every((st) => st.type !== "comment" || st.indent < token.indent))) {
          if (top.type === "document")
            top.end = last.start;
          else
            top.items.push({ start: last.start });
          token.items.splice(-1, 1);
        }
      }
    }
  }
  *stream() {
    switch (this.type) {
      case "directive-line":
        yield { type: "directive", offset: this.offset, source: this.source };
        return;
      case "byte-order-mark":
      case "space":
      case "comment":
      case "newline":
        yield this.sourceToken;
        return;
      case "doc-mode":
      case "doc-start": {
        const doc = {
          type: "document",
          offset: this.offset,
          start: []
        };
        if (this.type === "doc-start")
          doc.start.push(this.sourceToken);
        this.stack.push(doc);
        return;
      }
    }
    yield {
      type: "error",
      offset: this.offset,
      message: `Unexpected ${this.type} token in YAML stream`,
      source: this.source
    };
  }
  *document(doc) {
    if (doc.value)
      return yield* this.lineEnd(doc);
    switch (this.type) {
      case "doc-start": {
        if (findNonEmptyIndex(doc.start) !== -1) {
          yield* this.pop();
          yield* this.step();
        } else
          doc.start.push(this.sourceToken);
        return;
      }
      case "anchor":
      case "tag":
      case "space":
      case "comment":
      case "newline":
        doc.start.push(this.sourceToken);
        return;
    }
    const bv = this.startBlockValue(doc);
    if (bv)
      this.stack.push(bv);
    else {
      yield {
        type: "error",
        offset: this.offset,
        message: `Unexpected ${this.type} token in YAML document`,
        source: this.source
      };
    }
  }
  *scalar(scalar) {
    if (this.type === "map-value-ind") {
      const prev = getPrevProps(this.peek(2));
      const start = getFirstKeyStartProps(prev);
      let sep;
      if (scalar.end) {
        sep = scalar.end;
        sep.push(this.sourceToken);
        delete scalar.end;
      } else
        sep = [this.sourceToken];
      const map2 = {
        type: "block-map",
        offset: scalar.offset,
        indent: scalar.indent,
        items: [{ start, key: scalar, sep }]
      };
      this.onKeyLine = true;
      this.stack[this.stack.length - 1] = map2;
    } else
      yield* this.lineEnd(scalar);
  }
  *blockScalar(scalar) {
    switch (this.type) {
      case "space":
      case "comment":
      case "newline":
        scalar.props.push(this.sourceToken);
        return;
      case "scalar":
        scalar.source = this.source;
        this.atNewLine = true;
        this.indent = 0;
        if (this.onNewLine) {
          let nl = this.source.indexOf("\n") + 1;
          while (nl !== 0) {
            this.onNewLine(this.offset + nl);
            nl = this.source.indexOf("\n", nl) + 1;
          }
        }
        yield* this.pop();
        break;
      /* istanbul ignore next should not happen */
      default:
        yield* this.pop();
        yield* this.step();
    }
  }
  *blockMap(map2) {
    var _a;
    const it = map2.items[map2.items.length - 1];
    switch (this.type) {
      case "newline":
        this.onKeyLine = false;
        if (it.value) {
          const end = "end" in it.value ? it.value.end : void 0;
          const last = Array.isArray(end) ? end[end.length - 1] : void 0;
          if ((last == null ? void 0 : last.type) === "comment")
            end == null ? void 0 : end.push(this.sourceToken);
          else
            map2.items.push({ start: [this.sourceToken] });
        } else if (it.sep) {
          it.sep.push(this.sourceToken);
        } else {
          it.start.push(this.sourceToken);
        }
        return;
      case "space":
      case "comment":
        if (it.value) {
          map2.items.push({ start: [this.sourceToken] });
        } else if (it.sep) {
          it.sep.push(this.sourceToken);
        } else {
          if (this.atIndentedComment(it.start, map2.indent)) {
            const prev = map2.items[map2.items.length - 2];
            const end = (_a = prev == null ? void 0 : prev.value) == null ? void 0 : _a.end;
            if (Array.isArray(end)) {
              arrayPushArray(end, it.start);
              end.push(this.sourceToken);
              map2.items.pop();
              return;
            }
          }
          it.start.push(this.sourceToken);
        }
        return;
    }
    if (this.indent >= map2.indent) {
      const atMapIndent = !this.onKeyLine && this.indent === map2.indent;
      const atNextItem = atMapIndent && (it.sep || it.explicitKey) && this.type !== "seq-item-ind";
      let start = [];
      if (atNextItem && it.sep && !it.value) {
        const nl = [];
        for (let i = 0; i < it.sep.length; ++i) {
          const st = it.sep[i];
          switch (st.type) {
            case "newline":
              nl.push(i);
              break;
            case "space":
              break;
            case "comment":
              if (st.indent > map2.indent)
                nl.length = 0;
              break;
            default:
              nl.length = 0;
          }
        }
        if (nl.length >= 2)
          start = it.sep.splice(nl[1]);
      }
      switch (this.type) {
        case "anchor":
        case "tag":
          if (atNextItem || it.value) {
            start.push(this.sourceToken);
            map2.items.push({ start });
            this.onKeyLine = true;
          } else if (it.sep) {
            it.sep.push(this.sourceToken);
          } else {
            it.start.push(this.sourceToken);
          }
          return;
        case "explicit-key-ind":
          if (!it.sep && !it.explicitKey) {
            it.start.push(this.sourceToken);
            it.explicitKey = true;
          } else if (atNextItem || it.value) {
            start.push(this.sourceToken);
            map2.items.push({ start, explicitKey: true });
          } else {
            this.stack.push({
              type: "block-map",
              offset: this.offset,
              indent: this.indent,
              items: [{ start: [this.sourceToken], explicitKey: true }]
            });
          }
          this.onKeyLine = true;
          return;
        case "map-value-ind":
          if (it.explicitKey) {
            if (!it.sep) {
              if (includesToken(it.start, "newline")) {
                Object.assign(it, { key: null, sep: [this.sourceToken] });
              } else {
                const start2 = getFirstKeyStartProps(it.start);
                this.stack.push({
                  type: "block-map",
                  offset: this.offset,
                  indent: this.indent,
                  items: [{ start: start2, key: null, sep: [this.sourceToken] }]
                });
              }
            } else if (it.value) {
              map2.items.push({ start: [], key: null, sep: [this.sourceToken] });
            } else if (includesToken(it.sep, "map-value-ind")) {
              this.stack.push({
                type: "block-map",
                offset: this.offset,
                indent: this.indent,
                items: [{ start, key: null, sep: [this.sourceToken] }]
              });
            } else if (isFlowToken(it.key) && !includesToken(it.sep, "newline")) {
              const start2 = getFirstKeyStartProps(it.start);
              const key = it.key;
              const sep = it.sep;
              sep.push(this.sourceToken);
              delete it.key;
              delete it.sep;
              this.stack.push({
                type: "block-map",
                offset: this.offset,
                indent: this.indent,
                items: [{ start: start2, key, sep }]
              });
            } else if (start.length > 0) {
              it.sep = it.sep.concat(start, this.sourceToken);
            } else {
              it.sep.push(this.sourceToken);
            }
          } else {
            if (!it.sep) {
              Object.assign(it, { key: null, sep: [this.sourceToken] });
            } else if (it.value || atNextItem) {
              map2.items.push({ start, key: null, sep: [this.sourceToken] });
            } else if (includesToken(it.sep, "map-value-ind")) {
              this.stack.push({
                type: "block-map",
                offset: this.offset,
                indent: this.indent,
                items: [{ start: [], key: null, sep: [this.sourceToken] }]
              });
            } else {
              it.sep.push(this.sourceToken);
            }
          }
          this.onKeyLine = true;
          return;
        case "alias":
        case "scalar":
        case "single-quoted-scalar":
        case "double-quoted-scalar": {
          const fs = this.flowScalar(this.type);
          if (atNextItem || it.value) {
            map2.items.push({ start, key: fs, sep: [] });
            this.onKeyLine = true;
          } else if (it.sep) {
            this.stack.push(fs);
          } else {
            Object.assign(it, { key: fs, sep: [] });
            this.onKeyLine = true;
          }
          return;
        }
        default: {
          const bv = this.startBlockValue(map2);
          if (bv) {
            if (bv.type === "block-seq") {
              if (!it.explicitKey && it.sep && !includesToken(it.sep, "newline")) {
                yield* this.pop({
                  type: "error",
                  offset: this.offset,
                  message: "Unexpected block-seq-ind on same line with key",
                  source: this.source
                });
                return;
              }
            } else if (atMapIndent) {
              map2.items.push({ start });
            }
            this.stack.push(bv);
            return;
          }
        }
      }
    }
    yield* this.pop();
    yield* this.step();
  }
  *blockSequence(seq2) {
    var _a;
    const it = seq2.items[seq2.items.length - 1];
    switch (this.type) {
      case "newline":
        if (it.value) {
          const end = "end" in it.value ? it.value.end : void 0;
          const last = Array.isArray(end) ? end[end.length - 1] : void 0;
          if ((last == null ? void 0 : last.type) === "comment")
            end == null ? void 0 : end.push(this.sourceToken);
          else
            seq2.items.push({ start: [this.sourceToken] });
        } else
          it.start.push(this.sourceToken);
        return;
      case "space":
      case "comment":
        if (it.value)
          seq2.items.push({ start: [this.sourceToken] });
        else {
          if (this.atIndentedComment(it.start, seq2.indent)) {
            const prev = seq2.items[seq2.items.length - 2];
            const end = (_a = prev == null ? void 0 : prev.value) == null ? void 0 : _a.end;
            if (Array.isArray(end)) {
              arrayPushArray(end, it.start);
              end.push(this.sourceToken);
              seq2.items.pop();
              return;
            }
          }
          it.start.push(this.sourceToken);
        }
        return;
      case "anchor":
      case "tag":
        if (it.value || this.indent <= seq2.indent)
          break;
        it.start.push(this.sourceToken);
        return;
      case "seq-item-ind":
        if (this.indent !== seq2.indent)
          break;
        if (it.value || includesToken(it.start, "seq-item-ind"))
          seq2.items.push({ start: [this.sourceToken] });
        else
          it.start.push(this.sourceToken);
        return;
    }
    if (this.indent > seq2.indent) {
      const bv = this.startBlockValue(seq2);
      if (bv) {
        this.stack.push(bv);
        return;
      }
    }
    yield* this.pop();
    yield* this.step();
  }
  *flowCollection(fc) {
    const it = fc.items[fc.items.length - 1];
    if (this.type === "flow-error-end") {
      let top;
      do {
        yield* this.pop();
        top = this.peek(1);
      } while ((top == null ? void 0 : top.type) === "flow-collection");
    } else if (fc.end.length === 0) {
      switch (this.type) {
        case "comma":
        case "explicit-key-ind":
          if (!it || it.sep)
            fc.items.push({ start: [this.sourceToken] });
          else
            it.start.push(this.sourceToken);
          return;
        case "map-value-ind":
          if (!it || it.value)
            fc.items.push({ start: [], key: null, sep: [this.sourceToken] });
          else if (it.sep)
            it.sep.push(this.sourceToken);
          else
            Object.assign(it, { key: null, sep: [this.sourceToken] });
          return;
        case "space":
        case "comment":
        case "newline":
        case "anchor":
        case "tag":
          if (!it || it.value)
            fc.items.push({ start: [this.sourceToken] });
          else if (it.sep)
            it.sep.push(this.sourceToken);
          else
            it.start.push(this.sourceToken);
          return;
        case "alias":
        case "scalar":
        case "single-quoted-scalar":
        case "double-quoted-scalar": {
          const fs = this.flowScalar(this.type);
          if (!it || it.value)
            fc.items.push({ start: [], key: fs, sep: [] });
          else if (it.sep)
            this.stack.push(fs);
          else
            Object.assign(it, { key: fs, sep: [] });
          return;
        }
        case "flow-map-end":
        case "flow-seq-end":
          fc.end.push(this.sourceToken);
          return;
      }
      const bv = this.startBlockValue(fc);
      if (bv)
        this.stack.push(bv);
      else {
        yield* this.pop();
        yield* this.step();
      }
    } else {
      const parent = this.peek(2);
      if (parent.type === "block-map" && (this.type === "map-value-ind" && parent.indent === fc.indent || this.type === "newline" && !parent.items[parent.items.length - 1].sep)) {
        yield* this.pop();
        yield* this.step();
      } else if (this.type === "map-value-ind" && parent.type !== "flow-collection") {
        const prev = getPrevProps(parent);
        const start = getFirstKeyStartProps(prev);
        fixFlowSeqItems(fc);
        const sep = fc.end.splice(1, fc.end.length);
        sep.push(this.sourceToken);
        const map2 = {
          type: "block-map",
          offset: fc.offset,
          indent: fc.indent,
          items: [{ start, key: fc, sep }]
        };
        this.onKeyLine = true;
        this.stack[this.stack.length - 1] = map2;
      } else {
        yield* this.lineEnd(fc);
      }
    }
  }
  flowScalar(type) {
    if (this.onNewLine) {
      let nl = this.source.indexOf("\n") + 1;
      while (nl !== 0) {
        this.onNewLine(this.offset + nl);
        nl = this.source.indexOf("\n", nl) + 1;
      }
    }
    return {
      type,
      offset: this.offset,
      indent: this.indent,
      source: this.source
    };
  }
  startBlockValue(parent) {
    switch (this.type) {
      case "alias":
      case "scalar":
      case "single-quoted-scalar":
      case "double-quoted-scalar":
        return this.flowScalar(this.type);
      case "block-scalar-header":
        return {
          type: "block-scalar",
          offset: this.offset,
          indent: this.indent,
          props: [this.sourceToken],
          source: ""
        };
      case "flow-map-start":
      case "flow-seq-start":
        return {
          type: "flow-collection",
          offset: this.offset,
          indent: this.indent,
          start: this.sourceToken,
          items: [],
          end: []
        };
      case "seq-item-ind":
        return {
          type: "block-seq",
          offset: this.offset,
          indent: this.indent,
          items: [{ start: [this.sourceToken] }]
        };
      case "explicit-key-ind": {
        this.onKeyLine = true;
        const prev = getPrevProps(parent);
        const start = getFirstKeyStartProps(prev);
        start.push(this.sourceToken);
        return {
          type: "block-map",
          offset: this.offset,
          indent: this.indent,
          items: [{ start, explicitKey: true }]
        };
      }
      case "map-value-ind": {
        this.onKeyLine = true;
        const prev = getPrevProps(parent);
        const start = getFirstKeyStartProps(prev);
        return {
          type: "block-map",
          offset: this.offset,
          indent: this.indent,
          items: [{ start, key: null, sep: [this.sourceToken] }]
        };
      }
    }
    return null;
  }
  atIndentedComment(start, indent2) {
    if (this.type !== "comment")
      return false;
    if (this.indent <= indent2)
      return false;
    return start.every((st) => st.type === "newline" || st.type === "space");
  }
  *documentEnd(docEnd) {
    if (this.type !== "doc-mode") {
      if (docEnd.end)
        docEnd.end.push(this.sourceToken);
      else
        docEnd.end = [this.sourceToken];
      if (this.type === "newline")
        yield* this.pop();
    }
  }
  *lineEnd(token) {
    switch (this.type) {
      case "comma":
      case "doc-start":
      case "doc-end":
      case "flow-seq-end":
      case "flow-map-end":
      case "map-value-ind":
        yield* this.pop();
        yield* this.step();
        break;
      case "newline":
        this.onKeyLine = false;
      // fallthrough
      case "space":
      case "comment":
      default:
        if (token.end)
          token.end.push(this.sourceToken);
        else
          token.end = [this.sourceToken];
        if (this.type === "newline")
          yield* this.pop();
    }
  }
};

// node_modules/yaml/browser/dist/public-api.js
function parseOptions(options) {
  const prettyErrors = options.prettyErrors !== false;
  const lineCounter = options.lineCounter || prettyErrors && new LineCounter() || null;
  return { lineCounter, prettyErrors };
}
function parseAllDocuments(source, options = {}) {
  const { lineCounter, prettyErrors } = parseOptions(options);
  const parser = new Parser(lineCounter == null ? void 0 : lineCounter.addNewLine);
  const composer = new Composer(options);
  const docs = Array.from(composer.compose(parser.parse(source)));
  if (prettyErrors && lineCounter)
    for (const doc of docs) {
      doc.errors.forEach(prettifyError(source, lineCounter));
      doc.warnings.forEach(prettifyError(source, lineCounter));
    }
  if (docs.length > 0)
    return docs;
  return Object.assign([], { empty: true }, composer.streamInfo());
}
function parseDocument(source, options = {}) {
  const { lineCounter, prettyErrors } = parseOptions(options);
  const parser = new Parser(lineCounter == null ? void 0 : lineCounter.addNewLine);
  const composer = new Composer(options);
  let doc = null;
  for (const _doc of composer.compose(parser.parse(source), true, source.length)) {
    if (!doc)
      doc = _doc;
    else if (doc.options.logLevel !== "silent") {
      doc.errors.push(new YAMLParseError(_doc.range.slice(0, 2), "MULTIPLE_DOCS", "Source contains multiple documents; please use YAML.parseAllDocuments()"));
      break;
    }
  }
  if (prettyErrors && lineCounter) {
    doc.errors.forEach(prettifyError(source, lineCounter));
    doc.warnings.forEach(prettifyError(source, lineCounter));
  }
  return doc;
}
function parse(src, reviver, options) {
  let _reviver = void 0;
  if (typeof reviver === "function") {
    _reviver = reviver;
  } else if (options === void 0 && reviver && typeof reviver === "object") {
    options = reviver;
  }
  const doc = parseDocument(src, options);
  if (!doc)
    return null;
  doc.warnings.forEach((warning) => warn(doc.options.logLevel, warning));
  if (doc.errors.length > 0) {
    if (doc.options.logLevel !== "silent")
      throw doc.errors[0];
    else
      doc.errors = [];
  }
  return doc.toJS(Object.assign({ reviver: _reviver }, options));
}
function stringify3(value, replacer, options) {
  var _a;
  let _replacer = null;
  if (typeof replacer === "function" || Array.isArray(replacer)) {
    _replacer = replacer;
  } else if (options === void 0 && replacer) {
    options = replacer;
  }
  if (typeof options === "string")
    options = options.length;
  if (typeof options === "number") {
    const indent2 = Math.round(options);
    options = indent2 < 1 ? void 0 : indent2 > 8 ? { indent: 8 } : { indent: indent2 };
  }
  if (value === void 0) {
    const { keepUndefined } = (_a = options != null ? options : replacer) != null ? _a : {};
    if (!keepUndefined)
      return void 0;
  }
  if (isDocument(value) && !_replacer)
    return value.toString(options);
  return new Document(value, _replacer, options).toString(options);
}

// node_modules/yaml/browser/index.js
var browser_default = dist_exports;
