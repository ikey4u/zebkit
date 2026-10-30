"""Media outside the light DOM, stable IDs, and no accidental GC roots."""
import ctypes
import json
import time

import zebkit

assert zebkit.init() == 0
view = zebkit.View()
state = {"evaluated": False, "error": None, "list": None}
list_media = zebkit._declare("zebkit_view_media_list", None, zebkit._V, zebkit._RESULT, zebkit._P)

def wait(key):
    deadline = time.monotonic() + 10
    while not state[key] and time.monotonic() < deadline:
        assert zebkit.pump()
        time.sleep(0.001)
    assert state[key], state
    assert state["error"] is None, state

def evaluated(value, error):
    state["evaluated"] = True
    state["error"] = error

view.evaluate("""(() => {
  const keep = window.mediaInventory = {};
  const connected = document.createElement('audio');
  connected.id = 'connected'; document.body.append(connected);
  keep.detached = new Audio(); keep.detached.id = 'detached';
  for (const mode of ['open', 'closed']) {
    const host = document.createElement('div'); document.body.append(host);
    const root = host.attachShadow({mode});
    const video = document.createElement('video'); video.id = mode; root.append(video);
    if (mode === 'closed') {
      const nested = document.createElement('div'); root.append(nested);
      const audio = document.createElement('audio'); audio.id = 'nested';
      nested.attachShadow({mode: 'closed'}).append(audio);
    }
  }
  return true;
})()""", evaluated)
wait("evaluated")

@zebkit._RESULT
def listed(view_handle, result, error, data):
    state["list"] = json.loads(result) if result else []
    state["error"] = error

def inventory():
    state["list"] = None
    list_media(view._check(), listed, None)
    wait("list")
    return {entry["path"]: entry["id"] for entry in state["list"]}

first = inventory()
assert set(first) == {"audio#connected", "audio#detached", "video#open", "video#closed", "audio#nested"}, first
assert inventory() == first, "IDs must remain stable across lists"
print("ok media inventory includes detached Audio, open/closed and nested shadow roots with stable IDs", flush=True)

state["evaluated"] = False
view.evaluate("mediaInventory.detached = null; gc(); gc(); true", evaluated)
wait("evaluated")
after_gc = inventory()
assert "audio#detached" not in after_gc, "listing media must not retain otherwise unreachable Audio"
assert {key: value for key, value in first.items() if key != "audio#detached"} == after_gc
assert zebkit.shutdown()
print("ok media inventory does not keep unreachable detached elements alive", flush=True)
