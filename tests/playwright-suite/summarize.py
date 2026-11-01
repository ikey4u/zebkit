#!/usr/bin/env python3
"""Counts from a run of Playwright's suite (results.jsonl of reporter.mjs).

  summarize.py <results.jsonl> <expected-failures.txt> <summary.txt>
      [--reference <Chrome's results.jsonl>] [--exits <engine-exits.log>]

An entry of the expected list is "<file glob> | <title regex, error:<regex> for
the error's first line, or *> | REASON"; the first that matches a failure
explains it, but never of a test that timed out or crashed: those are counted apart,
and unexplained unless they fail on Chrome as well. Exits 1 when a failure is
explained by neither the list nor failing on Chrome as well.
"""

import argparse
import collections
import fnmatch
import json
import os
import re
import sys

parser = argparse.ArgumentParser()
parser.add_argument("results")
parser.add_argument("expected")
parser.add_argument("summary")
parser.add_argument("--reference")
parser.add_argument("--exits")
args = parser.parse_args()


def load(path):
    out = []
    with open(path) as lines:
        for line in lines:
            try:
                out.append(json.loads(line))
            except ValueError:
                pass  # A run cut short can leave half a line.
    return out


# What a client says when the engine went away under it.
CRASH = re.compile(r"Target crashed|Browser closed|browser has been closed|browserContext\.\w+: Target page, context or browser has been closed|Target closed|WebSocket error|Worker (teardown|process exited)")

entries = []
with open(args.expected) as lines:
    for number, line in enumerate(lines, 1):
        if not line.strip() or line.startswith("#"):
            continue
        parts = [part.strip() for part in line.split(" | ", 2)]
        if len(parts) != 3 or not parts[2]:
            sys.exit(f"{args.expected}:{number}: needs <file> | <title> | REASON")
        entries.append(
            {
                "file": parts[0],
                "title": None if parts[1] == "*" or parts[1].startswith("error:") else re.compile(parts[1]),
                "error": re.compile(parts[1][6:]) if parts[1].startswith("error:") else None,
                "reason": parts[2],
                "hits": 0,
            }
        )


def explain(test):
    # A hang or a crash is not what a listed cause says: it is counted apart, never
    # absorbed by an entry of its file (a test whose run reached its limit, or whose
    # engine went). An entry for it says "error:Test timeout" itself if it is known.
    if abnormal(test):
        return None
    name = f"{test['project']}/{test['file']}"
    for entry in entries:
        first_line = (test.get("error") or "").split("\n")[0]
        if (
            fnmatch.fnmatch(name, entry["file"])
            and (not entry["title"] or entry["title"].search(test["title"]))
            and (not entry["error"] or entry["error"].search(first_line))
        ):
            entry["hits"] += 1
            return entry["reason"]
    return None


def abnormal(test):
    """"timeout" or "crash", when the test ended so."""
    error = (test.get("error") or "").split("\n")[0]
    if test["status"] in ("timedOut", "interrupted"):
        return "timeout"
    if CRASH.search(error):
        return "crash"
    return None


def key(test):
    return (test["project"], test["file"], test["title"])


results = load(args.results)
reference = {key(t): t for t in load(args.reference)} if args.reference else None
FAILED = ("unexpected", "flaky")
files = collections.defaultdict(collections.Counter)
totals = collections.Counter()
unexplained, chrome_too, by_reason = [], [], collections.defaultdict(list)
ended = collections.defaultdict(list)
for test in results:
    name = f"{test['project']}/{test['file']}"
    outcome = test["outcome"]
    kind = "passed" if outcome == "expected" else "skipped" if outcome == "skipped" else "failed"
    files[name][kind] += 1
    totals[kind] += 1
    if kind != "failed":
        continue
    other = reference.get(key(test)) if reference else None
    reason = explain(test)
    kind_of_end = abnormal(test)
    if kind_of_end:
        totals[f"failed, {kind_of_end}"] += 1
        ended[kind_of_end].append(test)
    if other and other["outcome"] in FAILED:
        totals["failed, also on Chrome"] += 1
        chrome_too.append(test)
    if reason:
        totals["failed, listed"] += 1
        by_reason[reason].append(test)
    elif not (other and other["outcome"] in FAILED):
        totals["failed, unexplained"] += 1
        unexplained.append(test)

lines = [f"Counts: {sum(files[f].total() for f in files)} tests"]
for kind in ("passed", "failed", "skipped", "failed, listed", "failed, also on Chrome", "failed, timeout", "failed, crash",
             "failed, unexplained"):
    lines.append(f"  {kind}: {totals[kind]}")
if reference:
    passed_there = sum(1 for t in results if t["outcome"] != "skipped" and reference.get(key(t), {}).get("outcome") == "expected")
    ours = sum(1 for t in results if t["outcome"] == "expected" and reference.get(key(t), {}).get("outcome") == "expected")
    lines.append(f"  of the {passed_there} that pass on Chrome, here passed: {ours}")
if args.exits and os.path.exists(args.exits):
    with open(args.exits) as log:
        lines.append(f"  engine exits during the run: {sum(1 for _ in log)}")
lines += ["", "By file: passed failed skipped"]
for name in sorted(files):
    c = files[name]
    lines.append(f"  {name}: {c['passed']} {c['failed']} {c['skipped']}")
lines += ["", "Failures by listed reason:"]
for reason, tests in sorted(by_reason.items(), key=lambda item: -len(item[1])):
    lines.append(f"  {len(tests)}: {reason}")
stale = [e for e in entries if not e["hits"]]
if stale:
    lines += ["", "Listed, matching no failure in this run:"]
    lines += [f"  {e['file']} | {e['title'].pattern if e['title'] else e['error'].pattern if e['error'] else '*'}" for e in stale]
for kind_of_end in sorted(ended):
    lines += ["", f"Ended by a {kind_of_end} (a list's entry does not explain these): {len(ended[kind_of_end])}"]
    for test in ended[kind_of_end]:
        where = "also on Chrome" if reference and reference.get(key(test), {}).get("outcome") in FAILED else "here only"
        lines.append(f"  {test['project']}/{test['file']}:{test['line']} {test['title']} ({where})")
lines += ["", f"Unexplained failures: {len(unexplained)}"]
for test in unexplained:
    first = (test.get("error") or test["status"]).split("\n")[0][:200]
    lines.append(f"  {test['project']}/{test['file']}:{test['line']} {test['title']}\n      {first}")
text = "\n".join(lines) + "\n"
with open(args.summary, "w") as out:
    out.write(text)
print(text[: text.index("By file:")] + f"(the rest in {args.summary})")
sys.exit(1 if unexplained else 0)
