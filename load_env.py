# ==============================================================================
# PlatformIO Pre-Build Script: Load Secrets & Configuration from the bridge .env
# ==============================================================================
# Reads ONLY neram-print-bridge/.env (or the file named by the ENV_FILE
# environment variable, e.g. ENV_FILE=.env.prod). The backend's root .env is
# never read.
# Printer IPs are managed in the POS UI; the *_PRINTER_HOST keys are optional boot hints. The build fails if a required key is missing or the URL is not
# HTTPS, so a bad config can never be flashed silently.
import os
import re
import sys
from urllib.parse import urlparse

Import("env")

project_dir = env.get("PROJECT_DIR", os.getcwd())
env_name = os.environ.get("ENV_FILE", ".env")
env_path = env_name if os.path.isabs(env_name) else os.path.join(project_dir, env_name)


def fail(msg):
    sys.stderr.write(f"\n[load_env.py] ERROR: {msg}\n")
    env.Exit(1)


if not os.path.isfile(env_path):
    fail(f"{env_path} not found. Create it (see README) before building.")

env_vars = {}
with open(env_path, "r", encoding="utf-8") as f:
    for line in f:
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        env_vars[k.strip()] = v.strip().strip("'\"")

REQUIRED = [
    "BACKEND_BASE_URL",
    "PRINTER_BRIDGE_TOKEN",
    "TENANT_CODE",
    "WIFI_SSID",
]
missing = [k for k in REQUIRED if not env_vars.get(k)]
if "WIFI_PASSWORD" not in env_vars:
    missing.append("WIFI_PASSWORD (may be empty, but the key must exist)")
if missing:
    fail(f"{env_path} is missing: {', '.join(missing)}")

# Backend URL: HTTPS only (set ALLOW_INSECURE_HTTP=1 for a local lab backend)
backend_url = env_vars["BACKEND_BASE_URL"].rstrip("/")
if backend_url.endswith("/api"):
    backend_url = backend_url[:-4].rstrip("/")
parsed = urlparse(backend_url)
if parsed.scheme != "https" and os.environ.get("ALLOW_INSECURE_HTTP") != "1":
    fail(f"BACKEND_BASE_URL must be https:// (got '{backend_url}'). "
         "Set ALLOW_INSECURE_HTTP=1 only for a local lab backend.")
if not parsed.hostname:
    fail(f"BACKEND_BASE_URL is not a valid URL: '{backend_url}'")

bridge_token = env_vars["PRINTER_BRIDGE_TOKEN"]
tenant_code = env_vars["TENANT_CODE"].lower()
if not re.fullmatch(r"[a-z0-9][a-z0-9-]{1,28}[a-z0-9]", tenant_code):
    fail("TENANT_CODE must be the cafe code from the SaaS console (3-30 chars: a-z, 0-9, '-')")
wifi_ssid = env_vars["WIFI_SSID"]
wifi_pass = env_vars["WIFI_PASSWORD"]
bill_host = env_vars.get("BILL_PRINTER_HOST", "")
kot_host = env_vars.get("KOT_PRINTER_HOST", "")
juice_host = env_vars.get("JUICE_KOT_PRINTER_HOST", "")
juice_port = env_vars.get("JUICE_KOT_PRINTER_PORT", "9100")
bill_port = env_vars.get("BILL_PRINTER_PORT", "9100")
kot_port = env_vars.get("KOT_PRINTER_PORT", "9100")
bridge_id = env_vars.get("BRIDGE_ID", "")

for name, port in (("BILL_PRINTER_PORT", bill_port), ("KOT_PRINTER_PORT", kot_port), ("JUICE_KOT_PRINTER_PORT", juice_port)):
    if not port.isdigit() or not (1 <= int(port) <= 65535):
        fail(f"{name} must be a TCP port number (got '{port}')")
if bridge_id and not re.fullmatch(r"[A-Za-z0-9_-]{1,40}", bridge_id):
    fail("BRIDGE_ID may only contain letters, digits, '-' and '_' (max 40 chars)")


def c_str(value):
    """Quote a value as a C string literal (escape backslash and quote)."""
    return '\\"' + value.replace("\\", "\\\\").replace('"', '\\\\\\"') + '\\"'


print(f"\n[load_env.py] Using {env_path}")
print(f"[load_env.py] -> BACKEND_BASE_URL:   {backend_url}")
print(f"[load_env.py] -> TENANT_CODE:        {tenant_code}")
print(f"[load_env.py] -> BRIDGE TOKEN:       {'*' * 6} ({len(bridge_token)} chars)")
print(f"[load_env.py] -> WIFI_SSID:          {wifi_ssid}")
print(f"[load_env.py] -> WIFI_PASSWORD:      {'*' * len(wifi_pass)}")
print(f"[load_env.py] -> BILL_PRINTER_HOST:  {bill_host}")
print(f"[load_env.py] -> KOT_PRINTER_HOST:   {kot_host}")
print(f"[load_env.py] -> JUICE_KOT_PRINTER:  {juice_host or "(from POS UI)"}:{juice_port}")
print(f"[load_env.py] -> BRIDGE_ID:          {bridge_id or '(auto: NERAM-ESP32-<mac>)'}\n")

env.Append(CPPDEFINES=[
    ("BACKEND_BASE_URL", c_str(backend_url)),
    ("BACKEND_AUTH_TOKEN", c_str(bridge_token)),
    ("BACKEND_TENANT_CODE", c_str(tenant_code)),
    ("DEFAULT_WIFI_SSID", c_str(wifi_ssid)),
    ("DEFAULT_WIFI_PASS", c_str(wifi_pass)),
    ("DEFAULT_BILL_PRINTER_HOST", c_str(bill_host)),
    ("DEFAULT_KOT_PRINTER_HOST", c_str(kot_host)),
    ("DEFAULT_JUICE_KOT_PRINTER_HOST", c_str(juice_host)),
    ("DEFAULT_JUICE_KOT_PRINTER_PORT", juice_port),
    ("DEFAULT_BILL_PRINTER_PORT", bill_port),
    ("DEFAULT_KOT_PRINTER_PORT", kot_port),
    ("BRIDGE_ID_OVERRIDE", c_str(bridge_id)),
])
