# Verze firmwaru z gitu (git describe) jako makro FW_VERSION,
# např. "3cc3749" nebo "v1.0-2-g3cc3749-dirty".
import subprocess

Import("env")

try:
    version = subprocess.check_output(
        ["git", "describe", "--tags", "--always", "--dirty"],
        cwd=env.subst("$PROJECT_DIR"),
        stderr=subprocess.DEVNULL,
        text=True,
    ).strip()
except Exception:
    version = "unknown"

print("Firmware version:", version)
env.Append(CPPDEFINES=[("FW_VERSION", env.StringifyMacro(version))])
