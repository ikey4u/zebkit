"""The Python binding's source, read without an engine: every C function it
declares is used, and __all__ names every public name of the module.

    python3 tests/engine/python_declarations.py

scripts/check-bindings.py counts a function as covered when its name is in
the source; this holds that a declared function is also called.
"""

import ast
import pathlib
import sys

PACKAGE = pathlib.Path(__file__).resolve().parent.parent.parent / "bindings" / "python" / "zebkit"


def declared_and_unused(tree):
    """Module-level names bound to _declare(...) that nothing else uses."""
    declared = {}
    for node in tree.body:
        if isinstance(node, ast.Assign) and isinstance(node.value, ast.Call) and \
                getattr(node.value.func, "id", None) == "_declare":
            for target in node.targets:
                if isinstance(target, ast.Name):
                    declared[target.id] = node.lineno
    used = {node.id for node in ast.walk(tree) if isinstance(node, ast.Name) and isinstance(node.ctx, ast.Load)}
    return sorted((line, name) for name, line in declared.items() if name not in used)


def public_names(tree):
    names = set()
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)) and not node.name.startswith("_"):
            names.add(node.name)
        elif isinstance(node, ast.Assign):
            for target in node.targets:
                for name in ast.walk(target):
                    if isinstance(name, ast.Name) and not name.id.startswith("_") and name.id != "__all__":
                        names.add(name.id)
    return names


def listed(tree):
    names = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Assign, ast.AugAssign)):
            targets = node.targets if isinstance(node, ast.Assign) else [node.target]
            if any(isinstance(t, ast.Name) and t.id == "__all__" for t in targets) and \
                    isinstance(node.value, (ast.List, ast.Tuple)):
                names |= {element.value for element in node.value.elts if isinstance(element, ast.Constant)}
    return names


def main():
    failures = 0
    for path in sorted(PACKAGE.glob("*.py")):
        tree = ast.parse(path.read_text(), str(path))
        for line, name in declared_and_unused(tree):
            print(f"FAIL {path.name}:{line}: {name} is declared and never used")
            failures += 1
        names = listed(tree)
        if names:
            missing = sorted(public_names(tree) - names)
            for name in missing:
                print(f"FAIL {path.name}: {name} is public and not in __all__")
                failures += 1
            for name in sorted(names - public_names(tree)):
                print(f"FAIL {path.name}: __all__ names {name}, which it does not define")
                failures += 1
    print("ok   the Python binding's declarations" if not failures else f"{failures} problems")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
