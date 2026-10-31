#!/usr/bin/env python3
"""Capture Pico/Pi serial console to stdout; PowerShell logs everything."""
import argparse, sys, time
try:
    import serial
except ImportError:
    sys.exit('Install pyserial first: py -m pip install pyserial')

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--port', default='COM5')
    p.add_argument('--baud', type=int, default=115200)
    p.add_argument('--seconds', type=float, default=60)
    a=p.parse_args()
    print(f'capturing {a.port} at {a.baud} for {a.seconds}s', flush=True)
    with serial.Serial(a.port, a.baud, timeout=0.2) as s:
        deadline=time.monotonic()+a.seconds
        while time.monotonic()<deadline:
            try:
                data=s.read(4096)
            except serial.SerialException as exc:
                print(f'port disconnected: {exc}', flush=True)
                raise
            if data:
                sys.stdout.write(data.decode('utf-8', errors='replace'))
                sys.stdout.flush()
    print('\nserial capture complete',flush=True)
if __name__=='__main__': main()
