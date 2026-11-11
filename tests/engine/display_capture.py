"""getDisplayMedia() (docs/SYSTEM_INTEGRATION.md, 10): refused without a function
of the application's; with one, a request of type "display" with the sources the
engine can capture, answered with one: the view itself, whose frames the page
then receives; refused when the application says no.

    python3 tests/engine/display_capture.py [engine switches...]

Exit 0 when every check passes.
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import Server, activated, check, ev, finish, wait, zebkit  # noqa: E402


# The stream's first frame's size, or the error's name.
CAPTURE = """navigator.mediaDevices.getDisplayMedia({video: true, audio: %s}).then(async stream => {
  const track = stream.getVideoTracks()[0];
  const video = document.createElement('video');
  video.muted = true; video.srcObject = stream; document.body.append(video);
  await video.play();
  const end = Date.now() + 8000;
  while (!video.videoWidth && Date.now() < end) await new Promise(r => setTimeout(r, 50));
  const result = {surface: track.getSettings().displaySurface, width: video.videoWidth,
                  audio: stream.getAudioTracks().length};
  stream.getTracks().forEach(t => t.stop());
  return JSON.stringify(result);
}, e => e.name)"""


PAGE = (b"<title>Captured</title><body style='background:#36c'><h1>a page to capture</h1>"
        b"<script>let n = 0; setInterval(() => document.querySelector('h1').textContent = ++n, 100)</script>")


def main():
    # A secure context's (http://127.0.0.1): getDisplayMedia() is not elsewhere.
    server = Server({"/": PAGE})
    url = server.base + "/"
    if zebkit.init(["display-capture"] + sys.argv[1:]) != 0:
        print("FAIL zebkit.init()")
        return 1
    try:
        view = zebkit.View(url, 400, 300)
        wait(lambda: ev(view, "location.href + document.readyState") == url + "complete")
        # getDisplayMedia() needs the page's user activation, and a view with focus.
        view.focus()
        alone = ev(view, activated(CAPTURE % "false"))
        check(alone == "NotAllowedError", "without a function getDisplayMedia() is refused", alone)
        asked = []

        def choose(request):
            asked.append(request.metadata)
            request.respond(True, "view" if request.metadata.get("audio") else None, "view")
        view.on_media_access(choose)
        got = ev(view, activated(CAPTURE % "false"), 20)
        meta = asked[0] if asked else {}
        check(meta.get("type") == "display" and any(s["id"] == "view" for s in meta.get("sources", [])),
              "the application is asked, with the sources", meta)
        result = json.loads(got) if isinstance(got, str) and got.startswith("{") else got
        check(isinstance(result, dict) and result.get("surface") == "browser" and result.get("width", 0) > 0,
              "the view chosen, the page receives its frames", result)
        with_audio = ev(view, activated(CAPTURE % "true"), 20)
        result = json.loads(with_audio) if isinstance(with_audio, str) and with_audio.startswith("{") else with_audio
        check(isinstance(result, dict) and result.get("audio") == 1, "with the view's sound when asked and given",
              result)
        view.on_media_access(lambda request: request.respond(False))
        refused = ev(view, activated(CAPTURE % "false"))
        check(refused == "NotAllowedError", "refused by the application", refused)
        view.close()
    except Exception as error:  # noqa: BLE001
        check(False, "the test ran to the end", repr(error))
    finally:
        zebkit.shutdown()
        server.close()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
