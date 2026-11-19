# checks the two state JSONs printed by "test_mqtt_ha_payload state"
import sys, json
a, b = [json.loads(l) for l in sys.stdin if l.strip()]
assert a["progress"] == 37 and a["chapter_progress"] == 80 and a["download"] is None, a
assert a["position"] == 12.3 and a["duration"] == 33.3, (a["position"], a["duration"])  # 738.5 s, 1998 s
assert a["led"] == 2
assert b["progress"] is None and b["position"] is None and b["duration"] is None and b["download"] == 55, b
print("state JSON ok")
