# pio_version.py - PlatformIO extra script (post): Firmware-Version aus Git.
#
# Setzt FERRARIS_FW_VERSION auf `git describe --tags --dirty`, ohne fuehrendes "V"/"v":
#   auf einem Tag         -> "2.2.0"
#   3 Commits danach      -> "2.2.0-3-g1a2b3c4"
#   mit lokalen Aenderungen -> "...-dirty"
# So zeigen Dashboard, MQTT und Konsole immer genau den gebauten Stand.
# Ohne Git (z. B. ZIP-Download) gilt der Fallback in src/main.cpp.
#
# Nur fuer den Projektcode (projenv), nicht global: sonst wuerde bei jedem
# neuen Commit das ganze Framework neu kompiliert.
import subprocess

Import("projenv")


def git_version():
    try:
        out = subprocess.check_output(
            ["git", "describe", "--tags", "--dirty", "--always"],
            cwd=projenv.subst("$PROJECT_DIR"), stderr=subprocess.DEVNULL, timeout=10)
    except Exception:
        return None
    ver = out.decode().strip()
    return ver[1:] if ver[:1] in ("v", "V") else ver


version = git_version()
if version:
    projenv.Append(CPPDEFINES=[("FERRARIS_FW_VERSION", projenv.StringifyMacro(version))])
    print("Firmware-Version (git): " + version)
else:
    print("Firmware-Version: kein Git - Fallback aus src/main.cpp")
