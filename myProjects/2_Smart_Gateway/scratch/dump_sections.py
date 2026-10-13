import re
import sys
sys.stdout.reconfigure(encoding='utf-8')

with open(r'Docs/Onloi_BLE_Gateway_Protocol_v1_2.html', 'r', encoding='utf-8', errors='ignore') as f:
    text_v12 = f.read()

m = re.search(r'GET_SSIDS.*?(?=<div class="step"|<h2|<section|$)', text_v12, re.I | re.S)
if m:
    clean = re.sub(r'<[^>]+>', ' ', m.group(0))
    print(' '.join(clean.split()[:400]))
