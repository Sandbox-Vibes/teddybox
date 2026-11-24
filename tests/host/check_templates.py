"""Discovery templates must not use value_json.<name> for keys that are also
dict methods (update, items, keys, ...): Home Assistant's sandboxed Jinja
returns the method there, the sensor stays "unknown". Use value_json['name']."""
import json, re, sys
bad = []
for line in sys.stdin:
    topic, payload = line.rstrip("\n").split("\t", 1)
    for key, val in json.loads(payload).items():
        if key.endswith("template") and isinstance(val, str):
            for name in re.findall(r"value_json\.(\w+)", val):
                if hasattr(dict, name):
                    bad.append(f"{topic}: {key} uses value_json.{name}")
if bad:
    print("\n".join(bad)); sys.exit(1)
print("templates ok (no dict method names)")
