"""Check CDP messages against the standard protocol definition.

The definition is the one Chrome publishes, vendored in
deps/devtools-protocol. Everything the endpoint sends (results and
events) and everything the tests send (parameters) is compared with it:
unknown methods and events, missing mandatory fields, wrong types, enum
values the definition does not list, and fields it does not declare.
"""

import json
import pathlib

DEFINITION = pathlib.Path(__file__).resolve().parents[2] / "deps" / "devtools-protocol"


class Schema:
    def __init__(self, directory=DEFINITION):
        self.types = {}
        self.commands = {}
        self.events = {}
        for name in ("browser_protocol.json", "js_protocol.json"):
            for domain in json.loads((directory / name).read_text())["domains"]:
                prefix = domain["domain"]
                for item in domain.get("types", []):
                    self.types[f"{prefix}.{item['id']}"] = (prefix, item)
                for item in domain.get("commands", []):
                    self.commands[f"{prefix}.{item['name']}"] = (prefix, item)
                for item in domain.get("events", []):
                    self.events[f"{prefix}.{item['name']}"] = (prefix, item)

    # Each check returns a list of problems, empty when the message conforms.

    def command(self, method, params):
        if method not in self.commands:
            return [f"{method}: not a standard command"]
        domain, item = self.commands[method]
        return self._fields(domain, item.get("parameters", []), params, f"{method} params")

    def result(self, method, result):
        if method not in self.commands:
            return []
        domain, item = self.commands[method]
        return self._fields(domain, item.get("returns", []), result, f"{method} result")

    def event(self, method, params):
        if method not in self.events:
            return [f"{method}: not a standard event"]
        domain, item = self.events[method]
        return self._fields(domain, item.get("parameters", []), params, method)

    def _fields(self, domain, declared, value, where):
        if not isinstance(value, dict):
            return [f"{where}: expected an object, got {type(value).__name__}"]
        problems = []
        names = set()
        for field in declared:
            names.add(field["name"])
            if field["name"] not in value:
                if not field.get("optional"):
                    problems.append(f"{where}.{field['name']}: mandatory field is missing")
                continue
            problems += self._value(domain, field, value[field["name"]], f"{where}.{field['name']}")
        for name in value:
            if name not in names:
                problems.append(f"{where}.{name}: not in the definition")
        return problems

    def _value(self, domain, spec, value, where):
        if "$ref" in spec:
            name = spec["$ref"] if "." in spec["$ref"] else f"{domain}.{spec['$ref']}"
            domain, spec = self.types[name]
        kind = spec.get("type")
        if kind == "any":
            return []
        if kind in ("string", "binary"):
            if not isinstance(value, str):
                return [f"{where}: expected a string, got {value!r}"]
            if "enum" in spec and value not in spec["enum"]:
                return [f"{where}: {value!r} is not one of {spec['enum']}"]
            return []
        if kind == "boolean":
            return [] if isinstance(value, bool) else [f"{where}: expected a boolean, got {value!r}"]
        if kind == "number":
            if isinstance(value, bool) or not isinstance(value, (int, float)):
                return [f"{where}: expected a number, got {value!r}"]
            return []
        if kind == "integer":
            whole = isinstance(value, int) or (isinstance(value, float) and value.is_integer())
            if isinstance(value, bool) or not whole:
                return [f"{where}: expected an integer, got {value!r}"]
            return []
        if kind == "array":
            if not isinstance(value, list):
                return [f"{where}: expected an array, got {value!r}"]
            problems = []
            for index, item in enumerate(value):
                problems += self._value(domain, spec["items"], item, f"{where}[{index}]")
            return problems
        if kind == "object":
            if not isinstance(value, dict):
                return [f"{where}: expected an object, got {value!r}"]
            if "properties" not in spec:
                return []
            return self._fields(domain, spec["properties"], value, where)
        return [f"{where}: the definition has no type"]
