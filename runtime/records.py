"""Atomic process-ownership records. Unknown records never imply an idle game."""
import json
import os
from pathlib import Path
import tempfile
import time


def process_identity(pid):
    try:
        content = Path("/proc") / str(pid) / "stat"
        # comm can contain spaces and parentheses; fields after its last ')' are fixed.
        return content.read_text().rsplit(")", 1)[1].split()[19]
    except (OSError, IndexError):
        return None


def write_record(path, record):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, pending = tempfile.mkstemp(prefix=".record-", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump(record, stream)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(pending, path)
        try:
            descriptor = os.open(str(path.parent), os.O_RDONLY)
            try:
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
        except OSError:
            pass
    finally:
        Path(pending).unlink(missing_ok=True)


def read_record(path, attempts=5):
    path = Path(path)
    for attempt in range(attempts):
        try:
            record = json.loads(path.read_text())
            if not isinstance(record, dict) or type(
                record.get("pid")
            ) is not int or record["pid"] <= 1 or not isinstance(record.get("start"),
                                                                 str) or not record["start"].isdigit():
                raise ValueError()
            return record
        except FileNotFoundError:
            return None
        except (OSError, ValueError, TypeError):
            if attempt + 1 < attempts: time.sleep(0.05)
    raise ValueError("Unreadable activity record: " + str(path))
