#!/usr/bin/env python3
import json
import sys
import urllib.request

url = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:2344/health"
with urllib.request.urlopen(url, timeout=5) as res:
    body = json.loads(res.read().decode())
print(body)
if not body.get("ok"):
    sys.exit(1)
