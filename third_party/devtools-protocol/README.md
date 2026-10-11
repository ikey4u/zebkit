# devtools-protocol

The Chrome DevTools Protocol as Chrome defines it: every domain, command,
event and type, with parameter names, types and optionality. These are the
files Chrome's own protocol code is generated from, copied unchanged from
<https://github.com/ChromeDevTools/devtools-protocol> at the commit named in
`REVISION` (BSD licence, see `LICENSE`).

They are the reference for zebkit's endpoint and are used in two places:

- `scripts/gen-cdp-protocol.py` generates `src/cdp/CDPProtocol.inc` from
  them, the table the endpoint checks incoming parameters against.
- `tests/cdp/schema.py` checks every result and event the endpoint sends
  against them.

To update: replace the two JSON files and `REVISION` with a newer commit,
run `scripts/gen-cdp-protocol.py`, rebuild, and run `scripts/cdp.sh`.
