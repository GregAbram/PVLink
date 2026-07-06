"""
test_select.py — run on TACC with both system python AND pvpython

Tests whether socket timeout mechanisms work in this Python environment.

Usage:
    python test_select.py
    pvpython test_select.py

Each test should complete in ~2 seconds.  If a test hangs, that mechanism
is broken in this environment — Ctrl-C to move on.
"""

import socket
import select
import threading
import time
import sys

print(f"Python: {sys.version}", flush=True)
print(f"Executable: {sys.executable}", flush=True)

# ── Test 1: socket.settimeout ────────────────────────────────────────────────
print("\n--- Test 1: socket.settimeout(2.0) ---", flush=True)
s1, s2 = socket.socketpair()
s2.settimeout(2.0)
t0 = time.monotonic()
try:
    data = s2.recv(4)
    print(f"UNEXPECTED: recv returned {len(data)} bytes after {time.monotonic()-t0:.2f}s", flush=True)
except socket.timeout:
    print(f"OK: socket.timeout after {time.monotonic()-t0:.2f}s (expected ~2.0)", flush=True)
except OSError as e:
    print(f"OK (OSError): {e} after {time.monotonic()-t0:.2f}s", flush=True)
except Exception as e:
    print(f"UNEXPECTED exception: {type(e).__name__}: {e} after {time.monotonic()-t0:.2f}s", flush=True)
finally:
    try: s1.close()
    except: pass
    try: s2.close()
    except: pass

# ── Test 2: select.select with timeout ───────────────────────────────────────
print("\n--- Test 2: select.select(timeout=2.0) ---", flush=True)
s1, s2 = socket.socketpair()
t0 = time.monotonic()
ready, _, _ = select.select([s2], [], [], 2.0)
elapsed = time.monotonic() - t0
if ready:
    print(f"UNEXPECTED: select returned ready after {elapsed:.2f}s", flush=True)
else:
    print(f"OK: select timed out after {elapsed:.2f}s (expected ~2.0)", flush=True)
s1.close()
s2.close()

# ── Test 3: threading.Timer + shutdown(SHUT_RDWR) ────────────────────────────
print("\n--- Test 3: threading.Timer(2.0) + shutdown(SHUT_RDWR) ---", flush=True)
s1, s2 = socket.socketpair()

def _shutdown():
    print(f"  Timer fired — shutting down socket", flush=True)
    try: s2.shutdown(socket.SHUT_RDWR)
    except Exception as e: print(f"  shutdown error: {e}", flush=True)

timer = threading.Timer(2.0, _shutdown)
timer.daemon = True
timer.start()
t0 = time.monotonic()
try:
    data = s2.recv(4)
    elapsed = time.monotonic() - t0
    if len(data) == 0:
        print(f"OK: recv returned EOF after {elapsed:.2f}s (expected ~2.0)", flush=True)
    else:
        print(f"UNEXPECTED: recv returned {len(data)} bytes after {elapsed:.2f}s", flush=True)
except Exception as e:
    elapsed = time.monotonic() - t0
    print(f"OK: {type(e).__name__}: {e} after {elapsed:.2f}s (expected ~2.0)", flush=True)
finally:
    timer.cancel()
    try: s1.close()
    except: pass
    try: s2.close()
    except: pass

# ── Test 4: threading.Timer + close() on the OTHER end ───────────────────────
print("\n--- Test 4: threading.Timer(2.0) closes the PEER socket ---", flush=True)
s1, s2 = socket.socketpair()

def _close_peer():
    print(f"  Timer fired — closing peer (s1)", flush=True)
    try: s1.close()
    except Exception as e: print(f"  close error: {e}", flush=True)

timer = threading.Timer(2.0, _close_peer)
timer.daemon = True
timer.start()
t0 = time.monotonic()
try:
    data = s2.recv(4)
    elapsed = time.monotonic() - t0
    if len(data) == 0:
        print(f"OK: recv returned EOF after {elapsed:.2f}s (expected ~2.0)", flush=True)
    else:
        print(f"UNEXPECTED: recv returned {len(data)} bytes after {elapsed:.2f}s", flush=True)
except Exception as e:
    elapsed = time.monotonic() - t0
    print(f"OK: {type(e).__name__}: {e} after {elapsed:.2f}s (expected ~2.0)", flush=True)
finally:
    timer.cancel()
    try: s1.close()
    except: pass
    try: s2.close()
    except: pass

print("\nAll tests done.", flush=True)
