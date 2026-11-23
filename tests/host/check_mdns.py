"""Feeds mDNS queries (made with dnslib) to test_mdns_min and checks the answers."""
import subprocess, sys
from dnslib import DNSRecord, DNSQuestion, QTYPE

def q(name, qtype="A", id=0):
    r = DNSRecord(q=DNSQuestion(name, getattr(QTYPE, qtype)))
    r.header.id = id
    return r.pack().hex()

cases = [
    ("A for the box", 0, q("teddybox-3a7f.local"), "192.168.10.42"),
    ("upper case name", 0, q("TeddyBox-3A7F.local"), "192.168.10.42"),
    ("ANY", 0, q("teddybox-3a7f.local", "ANY"), "192.168.10.42"),
    ("other host", 0, q("printer.local"), None),
    ("AAAA only", 0, q("teddybox-3a7f.local", "AAAA"), None),
    ("legacy resolver keeps the id and the question", 1, q("teddybox-3a7f.local", id=4711), "192.168.10.42"),
    ("garbage", 0, "0000", None),
    ("response is ignored", 0, DNSRecord.question("teddybox-3a7f.local").reply().pack().hex(), None),
]
inp = "".join(f"{l} {h}\n" for _, l, h, _ in cases)
out = subprocess.run(["./test_mdns_min"], input=inp, capture_output=True, text=True).stdout.split("\n")
fails = 0
for (name, legacy, h, want), ans in zip(cases, out):
    ok = False
    if want is None:
        ok = ans == ""
    elif ans:
        r = DNSRecord.parse(bytes.fromhex(ans))
        a = [rr for rr in r.rr if rr.rtype == QTYPE.A]
        ok = len(a) == 1 and str(a[0].rdata) == want and str(a[0].rname).lower() == "teddybox-3a7f.local." and r.header.qr == 1 and r.header.aa == 1
        if legacy:
            ok = ok and r.header.id == 4711 and len(r.questions) == 1 and a[0].rclass == 1
        else:
            ok = ok and r.header.id == 0 and a[0].rclass == 0x8001
    print(("ok   " if ok else "FAIL ") + name)
    fails += not ok
ann = DNSRecord.parse(bytes.fromhex(out[len(cases)]))
ok = str(ann.rr[0].rdata) == "192.168.10.42"
print(("ok   " if ok else "FAIL ") + "announcement")
fails += not ok
sys.exit(1 if fails else 0)
