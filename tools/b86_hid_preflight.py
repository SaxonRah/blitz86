"""microDOS v29 USB HID bridge preflight; no shell quoting required."""
import sys
try:
    import hid
except Exception as exc:
    print('[hid] Could not import hidapi:', exc, flush=True)
    raise SystemExit(13)
try:
    matches = [dev for dev in hid.enumerate(0xCAFE, 0x4028)
               if 'microDOS HID UART bridge v29' in (dev.get('product_string') or '')]
except Exception as exc:
    print('[hid] USB enumeration failed:', exc, flush=True)
    raise SystemExit(14)
print('[hid] microDOS v29 bridge devices:', len(matches), flush=True)
for dev in matches:
    print('[hid] found:', dev.get('product_string'), flush=True)
raise SystemExit(0 if matches else 12)
