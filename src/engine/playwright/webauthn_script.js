// Playwright's WebAuthn script (injected/src/webAuthn.ts), bundled by scripts/playwright-sources.mjs
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


// packages/injected/src/webAuthn.ts
var webAuthn_exports = {};
__export(webAuthn_exports, {
  inject: () => inject
});
module.exports = __toCommonJS(webAuthn_exports);
function inject(globalThis) {
  if (globalThis.__pwWebAuthnInstalled)
    return;
  globalThis.__pwWebAuthnInstalled = true;
  const binding = globalThis.__pwWebAuthnBinding;
  if (!binding || !globalThis.navigator)
    return;
  if (!globalThis.navigator.credentials) {
    Object.defineProperty(globalThis.navigator, "credentials", {
      value: { create: async () => null, get: async () => null },
      writable: true,
      configurable: true
    });
  }
  function toBase64Url(buf) {
    const bytes = buf instanceof ArrayBuffer ? new Uint8Array(buf) : new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
    let s = "";
    for (let i = 0; i < bytes.length; i++)
      s += String.fromCharCode(bytes[i]);
    return globalThis.btoa(s).replaceAll("+", "-").replaceAll("/", "_").replaceAll("=", "");
  }
  function fromBase64Url(s) {
    let str = s.replaceAll("-", "+").replaceAll("_", "/");
    while (str.length % 4)
      str += "=";
    const bin = globalThis.atob(str);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++)
      out[i] = bin.charCodeAt(i);
    return out.buffer;
  }
  const PublicKeyCredentialCtor = globalThis.PublicKeyCredential;
  const AuthAttestationResponseCtor = globalThis.AuthenticatorAttestationResponse;
  const AuthAssertionResponseCtor = globalThis.AuthenticatorAssertionResponse;
  function defineReadonly(target, props) {
    for (const k of Object.keys(props))
      Object.defineProperty(target, k, { value: props[k], enumerable: true, configurable: true });
  }
  function makeAttestationResponse(clientDataJSON, attestationObject, authenticatorData, publicKey) {
    const proto = (AuthAttestationResponseCtor == null ? void 0 : AuthAttestationResponseCtor.prototype) || Object.prototype;
    const r = Object.create(proto);
    defineReadonly(r, { clientDataJSON, attestationObject });
    r.getTransports = () => ["internal"];
    r.getAuthenticatorData = () => authenticatorData;
    r.getPublicKey = () => publicKey;
    r.getPublicKeyAlgorithm = () => -7;
    return r;
  }
  function makeAssertionResponse(clientDataJSON, authenticatorData, signature, userHandle) {
    const proto = (AuthAssertionResponseCtor == null ? void 0 : AuthAssertionResponseCtor.prototype) || Object.prototype;
    const r = Object.create(proto);
    defineReadonly(r, { clientDataJSON, authenticatorData, signature, userHandle });
    return r;
  }
  function makePublicKeyCredential(id, response, responseToJSON) {
    const proto = (PublicKeyCredentialCtor == null ? void 0 : PublicKeyCredentialCtor.prototype) || Object.prototype;
    const cred = Object.create(proto);
    defineReadonly(cred, {
      id,
      rawId: fromBase64Url(id),
      type: "public-key",
      authenticatorAttachment: "platform",
      response
    });
    cred.getClientExtensionResults = () => ({});
    cred.toJSON = () => ({
      id,
      rawId: id,
      response: responseToJSON(),
      authenticatorAttachment: "platform",
      clientExtensionResults: {},
      type: "public-key"
    });
    return cred;
  }
  function toBuf(x) {
    if (!x)
      return new ArrayBuffer(0);
    if (x instanceof ArrayBuffer)
      return x;
    const v = x;
    const out = new Uint8Array(v.byteLength);
    out.set(new Uint8Array(v.buffer, v.byteOffset, v.byteLength));
    return out.buffer;
  }
  function failure(name, message) {
    const Ctor = globalThis.DOMException || Error;
    throw new Ctor(message, name);
  }
  const origCreate = globalThis.navigator.credentials.create.bind(globalThis.navigator.credentials);
  const origGet = globalThis.navigator.credentials.get.bind(globalThis.navigator.credentials);
  globalThis.navigator.credentials.create = async function(options) {
    var _a, _b, _c, _d, _e, _f, _g;
    if (!options || !options.publicKey)
      return origCreate(options);
    const pk = options.publicKey;
    const req = {
      type: "create",
      origin: globalThis.location.origin,
      challenge: toBase64Url(toBuf(pk.challenge)),
      rp: { id: (_a = pk.rp) == null ? void 0 : _a.id, name: ((_b = pk.rp) == null ? void 0 : _b.name) || "" },
      user: {
        id: toBase64Url(toBuf((_c = pk.user) == null ? void 0 : _c.id)),
        name: ((_d = pk.user) == null ? void 0 : _d.name) || "",
        displayName: ((_e = pk.user) == null ? void 0 : _e.displayName) || ""
      },
      pubKeyCredParams: (pk.pubKeyCredParams || []).map((p) => ({ type: p.type, alg: p.alg })),
      excludeCredentials: (pk.excludeCredentials || []).map((c) => ({ type: c.type, id: toBase64Url(toBuf(c.id)) })),
      userVerification: (_f = pk.authenticatorSelection) == null ? void 0 : _f.userVerification,
      residentKey: (_g = pk.authenticatorSelection) == null ? void 0 : _g.residentKey
    };
    const result = await binding(req);
    if (!result.ok)
      failure(result.name, result.message);
    const resp = makeAttestationResponse(
      fromBase64Url(result.clientDataJSON),
      fromBase64Url(result.attestationObject),
      fromBase64Url(result.authenticatorData),
      fromBase64Url(result.publicKey)
    );
    return makePublicKeyCredential(result.id, resp, () => ({
      clientDataJSON: result.clientDataJSON,
      authenticatorData: result.authenticatorData,
      transports: resp.getTransports(),
      publicKey: result.publicKey,
      publicKeyAlgorithm: resp.getPublicKeyAlgorithm(),
      attestationObject: result.attestationObject
    }));
  };
  globalThis.navigator.credentials.get = async function(options) {
    if (!options || !options.publicKey)
      return origGet(options);
    const pk = options.publicKey;
    const req = {
      type: "get",
      origin: globalThis.location.origin,
      challenge: toBase64Url(toBuf(pk.challenge)),
      rpId: pk.rpId || new URL(globalThis.location.origin).hostname,
      allowCredentials: (pk.allowCredentials || []).map((c) => ({ type: c.type, id: toBase64Url(toBuf(c.id)) })),
      userVerification: pk.userVerification
    };
    const result = await binding(req);
    if (!result.ok)
      failure(result.name, result.message);
    const resp = makeAssertionResponse(
      fromBase64Url(result.clientDataJSON),
      fromBase64Url(result.authenticatorData),
      fromBase64Url(result.signature),
      result.userHandle ? fromBase64Url(result.userHandle) : null
    );
    return makePublicKeyCredential(result.id, resp, () => ({
      clientDataJSON: result.clientDataJSON,
      authenticatorData: result.authenticatorData,
      signature: result.signature,
      ...result.userHandle ? { userHandle: result.userHandle } : {}
    }));
  };
  if (PublicKeyCredentialCtor) {
    PublicKeyCredentialCtor.isUserVerifyingPlatformAuthenticatorAvailable = async () => true;
    PublicKeyCredentialCtor.isConditionalMediationAvailable = async () => true;
  }
}
