import sys
b = open(__import__("os").environ.get("DREAD_ROMFS", "romfs") + "/system/localization/us_english.txt", "rb").read()
assert b[:4] == b"BTXT"
p = 8; d = {}
while p < len(b):
    e = b.index(b"\0", p); k = b[p:e].decode("ascii", "replace"); p = e + 1
    q = p
    while not (b[q] == 0 and b[q+1] == 0 and (q - p) % 2 == 0): q += 2
    v = b[p:q].decode("utf-16-le"); p = q + 2
    d[k] = v
import json
json.dump(d, open("us_english.json", "w"), indent=0, ensure_ascii=False)
print(len(d))
