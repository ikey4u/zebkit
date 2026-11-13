"""Bookmark editing persists in real PDFs; invalid replacement is atomic.

    python3 tests/engine/pdf_outline.py

Exit 0 when every check passes.
"""
import copy
import os
from pathlib import Path
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, finish, zebkit  # noqa: E402

sample = Path(__file__).resolve().parents[2] / 'examples/native-host/sample.pdf'
tree = [
    {'title': '第一章 – Résumé 📖', 'page': 0, 'x': 12.5, 'y': 72, 'zoom': 1.25, 'open': True,
     'children': [{'title': 'Child', 'page': 0, 'open': False}]},
    {'title': 'External', 'url': 'https://example.com/path?q=a&b=%22c%22', 'open': False},
    {'title': 'Closed group', 'open': False, 'children': [
        {'title': 'Nested group', 'open': True, 'children': [{'title': 'Leaf', 'page': 0, 'open': False}]}]},
]


def main():
    with zebkit.Pdf(sample) as pdf:
        pdf.set_outline(tree)
        original = pdf.outline()
        check(original == tree, 'set_outline: Unicode titles, destinations, links and hierarchy read back',
              None if original == tree else original)
        invalid = [None, {}, [None], [{'page': 0}], [{'title': 'bad', 'page': -1}],
                   [{'title': 'bad', 'page': 1}], [{'title': 'bad', 'page': 0, 'url': 'https://example.com'}],
                   [{'title': 'bad', 'x': 1}], [{'title': 'bad', 'page': 0, 'zoom': -1}],
                   [{'title': 'bad', 'open': 'yes'}], [{'title': 'bad', 'children': {}}],
                   [{'title': 'bad', 'unknown': True}], [{'title': 'bad', 'url': 'a\0b'}],
                   [{'title': 'x' * (1024 * 1024 + 1)}], [{'title': 'too many'}] * 100001]
        deep = {'title': 'leaf'}
        for _ in range(65):
            deep = {'title': 'branch', 'children': [deep]}
        invalid.append([deep])
        wrong = []
        for index, value in enumerate(invalid):
            replacement = [{'title': 'must not replace the original', 'page': 0}, *value] if isinstance(value, list) else value
            try:
                pdf.set_outline(replacement)
                wrong.append((index, 'accepted'))
            except zebkit.Error:
                if pdf.outline() != original:
                    wrong.append((index, 'the original changed'))
        check(not wrong, 'each of %d invalid trees refused, the outline unchanged' % len(invalid), wrong or None)
        with zebkit.Pdf(pdf.save()) as reopened:
            got = reopened.outline()
            check(got == tree, 'the outline after a full save', None if got == tree else got)
        changed = copy.deepcopy(tree)
        changed.reverse()
        changed[0]['title'] = 'Moved and renamed'
        changed[1:2] = []
        pdf.set_outline(changed)
        with zebkit.Pdf(pdf.save(incremental=True)) as reopened:
            got = reopened.outline()
            check(got == changed, 'reordered, renamed and deleted, after an incremental save',
                  None if got == changed else got)
        valid_deep = {'title': 'leaf', 'page': 0, 'open': False}
        for _ in range(64):
            valid_deep = {'title': 'branch', 'open': False, 'children': [valid_deep]}
        pdf.set_outline([valid_deep])
        with zebkit.Pdf(pdf.save()) as reopened:
            check(reopened.outline() == [valid_deep], 'a tree 65 deep, saved')
        pdf.set_outline([])
        empty = pdf.outline()
        with zebkit.Pdf(pdf.save()) as reopened:
            check(empty == [] and reopened.outline() == [], 'an empty outline, saved')
    return finish()


if __name__ == '__main__':
    sys.exit(main())
