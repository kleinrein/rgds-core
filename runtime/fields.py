"""Small atomic field records shared by hardware controls and library metadata."""
import os
from pathlib import Path
import tempfile


def read_fields(path):
    try:
        return dict(line.split("=", 1) for line in path.read_text().splitlines() if "=" in line)
    except (OSError, UnicodeError):
        return {}


def write_fields(path, fields, durable=True):
    """Atomically publish fields; skip identical data and flush persistent records.

    Session status/HUD callers may set durable=False: these are transient IPC,
    regenerated after a restart, rather than a save or preference record.
    """
    path = Path(path)
    content = "".join(
        key + "=" + str(value).replace("\n", " ").replace("\r", " ") + "\n" for key, value in fields.items()
    )
    try:
        if path.read_text() == content:
            return False
    except (OSError, UnicodeError):
        pass
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, pending = tempfile.mkstemp(prefix=".pending-", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as stream:
            stream.write(content)
            stream.flush()
            if durable:
                os.fsync(stream.fileno())
        os.replace(pending, path)
    finally:
        Path(pending).unlink(missing_ok=True)
    return True
