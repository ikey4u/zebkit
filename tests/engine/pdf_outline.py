"""Bookmark editing persists in real PDFs; invalid replacement is atomic."""
import copy
from pathlib import Path
import zebkit

sample = Path(__file__).resolve().parents[2] / 'examples/native-host/sample.pdf'
tree = [
    {'title': '第一章 – Résumé 📖', 'page': 0, 'x': 12.5, 'y': 72, 'zoom': 1.25, 'open': True,
     'children': [{'title': 'Child', 'page': 0, 'open': False}]},
    {'title': 'External', 'url': 'https://example.com/path?q=a&b=%22c%22', 'open': False},
    {'title': 'Closed group', 'open': False, 'children': [
        {'title': 'Nested group', 'open': True, 'children': [{'title': 'Leaf', 'page': 0, 'open': False}]}]},
]
with zebkit.Pdf(sample) as pdf:
    pdf.set_outline(tree)
    assert pdf.outline() == tree, pdf.outline()
    original = pdf.outline()
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
    for value in invalid:
        replacement = [{'title': 'must not replace the original', 'page': 0}, *value] if isinstance(value, list) else value
        try:
            pdf.set_outline(replacement)
            raise AssertionError('invalid tree accepted')
        except zebkit.Error:
            assert pdf.outline() == original
    with zebkit.Pdf(pdf.save()) as reopened:
        assert reopened.outline() == tree, reopened.outline()
    changed = copy.deepcopy(tree)
    changed.reverse()
    changed[0]['title'] = 'Moved and renamed'
    changed[1:2] = []
    pdf.set_outline(changed)
    with zebkit.Pdf(pdf.save(incremental=True)) as reopened:
        assert reopened.outline() == changed, reopened.outline()
    valid_deep = {'title': 'leaf', 'page': 0, 'open': False}
    for _ in range(64):
        valid_deep = {'title': 'branch', 'open': False, 'children': [valid_deep]}
    pdf.set_outline([valid_deep])
    with zebkit.Pdf(pdf.save()) as reopened:
        assert reopened.outline() == [valid_deep]
    pdf.set_outline([])
    assert pdf.outline() == []
    with zebkit.Pdf(pdf.save()) as reopened:
        assert reopened.outline() == []
print('ok PDF outline: Unicode, destinations, hierarchy, atomic validation, full/incremental save, reorder/rename/delete', flush=True)
