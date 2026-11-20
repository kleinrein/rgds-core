#!/usr/bin/env python3
"""RetroAchievements for RG DS Play: the account, and the player's way to the internet.

  achievements.py login --data DIR      {"user": ..., "password": ...} on stdin; keeps only the token
  achievements.py logout --data DIR
  achievements.py serve --badges DIR    rgds-play's requests on stdin, the answers on stdout

The password is sent once to retroachievements.org and never kept: what stays (in the private folder, mode 0600 where
the card allows it) is the account name and the token the site gives back, which is all rgds-play needs to sign in.
The launcher shows the name from achievements/account.ini and the outcome from achievements/status.ini.

serve: rgds-play links no TLS library, so rcheevos's server calls come here, one per line, tab separated:
    ID  URL  POST-DATA  CONTENT-TYPE  SAVE-PATH        (empty fields allowed, no tabs or newlines inside them)
and each answer goes back as a line "ID STATUS LENGTH" followed by LENGTH bytes of body. STATUS is the HTTP status, or
0 when the site could not be reached. A SAVE-PATH (a badge picture) gets the body written there, inside --badges,
instead of sent back. Several requests run at once; answers come in the order they finish.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request

sys.dont_write_bytecode = True
from fields import write_fields  # noqa: E402

SITE = "https://retroachievements.org/dorequest.php"
HOSTS = ("retroachievements.org", "media.retroachievements.org", "i.retroachievements.org")
ACCOUNT = "retroachievements.json"
TOKEN = "retroachievements.token"
USER_AGENT = "RGDS-Play/%s"


class AchievementsError(Exception):
    pass


def version():
    try:
        return (Path(__file__).resolve().parent.parent / "VERSION").read_text().strip() or "0"
    except OSError:
        return "0"


def private_root(data):
    # The same folder as the ScreenScraper account (scraper.private_root): ext4 /userdata on the stock card.
    if sys.platform == "linux" and Path("/userdata").is_dir():
        return Path("/userdata/rgds/private")
    return Path(data) / "private"


def save_private(path, value):
    import tempfile
    path.parent.mkdir(parents=True, mode=0o700, exist_ok=True)
    try:
        path.parent.chmod(0o700)
    except OSError:
        pass  # exFAT
    fd, pending = tempfile.mkstemp(prefix=".credentials-", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump(value, stream)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(pending, path)
        try:
            path.chmod(0o600)
        except OSError:
            pass
    finally:
        Path(pending).unlink(missing_ok=True)


def write_private_text(path, text):
    import tempfile
    fd, pending = tempfile.mkstemp(prefix=".credentials-", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as stream:
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(pending, path)
        try:
            path.chmod(0o600)
        except OSError:
            pass
    finally:
        Path(pending).unlink(missing_ok=True)


def read_account(data, folder=None):
    """{"user": ..., "token": ...} when signed in, else {}."""
    try:
        value = json.loads(((folder or private_root(data)) / ACCOUNT).read_text())
    except (OSError, ValueError):
        return {}
    if not isinstance(value, dict) or not all(isinstance(value.get(k), str) and value[k] for k in ("user", "token")):
        return {}
    return {"user": value["user"], "token": value["token"]}


class SiteOnly(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        target = urllib.parse.urlsplit(newurl)
        if target.scheme != "https" or target.hostname not in HOSTS:
            raise AchievementsError("RetroAchievements returned an unexpected redirect.")
        return super().redirect_request(request, fp, code, message, headers, newurl)


def allowed(url):
    parts = urllib.parse.urlsplit(url)
    return parts.scheme == "https" and parts.hostname in HOSTS


def fetch(url, post=None, content_type=None, agent=None, opener=None, timeout=30):
    """(status, body); status 0 when the site could not be reached."""
    if not allowed(url):
        return 0, b""
    headers = {"User-Agent": agent or USER_AGENT % version()}
    data = None
    if post is not None:
        data = post.encode("utf-8")
        headers["Content-Type"] = content_type or "application/x-www-form-urlencoded"
    request = urllib.request.Request(url, data=data, headers=headers)
    open_url = opener or urllib.request.build_opener(SiteOnly()).open
    try:
        with open_url(request, timeout=timeout) as response:
            return response.status, response.read(16 << 20)
    except urllib.error.HTTPError as error:
        try:
            return error.code, error.read(1 << 20)
        except OSError:
            return error.code, b""
    except (OSError, ValueError, AchievementsError):
        return 0, b""


def login(data, user, password, folder=None, opener=None):
    if not user or not password:
        raise AchievementsError("Enter your RetroAchievements username and password.")
    post = urllib.parse.urlencode({"r": "login2", "u": user, "p": password})
    status, body = fetch(SITE, post, opener=opener)
    if status == 0:
        raise AchievementsError("Could not reach RetroAchievements. Check the connection.")
    try:
        answer = json.loads(body.decode("utf-8", "replace"))
    except ValueError:
        answer = {}
    if not isinstance(answer, dict) or not answer.get("Success") or not answer.get("Token"):
        message = answer.get("Error") if isinstance(answer, dict) else None
        if status in (401, 403) or (isinstance(answer, dict) and answer.get("Code") == "invalid_credentials"):
            message = "Wrong username or password."
        raise AchievementsError(str(message or "RetroAchievements did not accept the sign-in."))
    name = str(answer.get("User") or user)
    save_private((folder or private_root(data)) / ACCOUNT, {"user": name, "token": str(answer["Token"])})
    # DSCore's --cheevos-token format: the name, then the token (dscore/src/cheevos read_token_file).
    write_private_text((folder or private_root(data)) / TOKEN, name + "\n" + str(answer["Token"]) + "\n")
    write_fields(Path(data) / "achievements/account.ini", {"user": name, "score": str(answer.get("Score", ""))})
    return name


def logout(data, folder=None):
    ((folder or private_root(data)) / ACCOUNT).unlink(missing_ok=True)
    ((folder or private_root(data)) / TOKEN).unlink(missing_ok=True)
    (Path(data) / "achievements/account.ini").unlink(missing_ok=True)


def inside(folder, path):
    try:
        return Path(path).resolve().is_relative_to(Path(folder).resolve())
    except (OSError, ValueError):
        return False


def serve(badges, stdin=None, stdout=None, opener=None, workers=4):
    stdin = stdin or sys.stdin.buffer
    stdout = stdout or sys.stdout.buffer
    lock = threading.Lock()

    def answer(ident, status, body):
        with lock:
            stdout.write(b"%s %d %d\n" % (ident.encode(), status, len(body)) + body)
            stdout.flush()

    def handle(fields):
        ident, url, post, content_type, save = (fields + [""] * 5)[:5]
        agent = os.environ.get("RGDS_RA_USER_AGENT") or None
        if save and not inside(badges, save):
            answer(ident, 0, b"")
            return
        status, body = fetch(url, post if post else None, content_type or None, agent, opener)
        if save:
            if status == 200 and body:
                target = Path(save)
                target.parent.mkdir(parents=True, exist_ok=True)
                pending = target.with_name("." + target.name + ".part")
                pending.write_bytes(body)
                os.replace(pending, target)
            answer(ident, status, b"")
        else:
            answer(ident, status, body)

    with ThreadPoolExecutor(max_workers=workers) as pool:
        for line in iter(stdin.readline, b""):
            fields = line.decode("utf-8", "replace").rstrip("\r\n").split("\t")
            if fields and fields[0]:
                pool.submit(handle, fields)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=("login", "logout", "serve"))
    parser.add_argument("--data", type=Path)
    parser.add_argument("--private-dir", type=Path)
    parser.add_argument("--badges", type=Path)
    args = parser.parse_args()
    if args.action == "serve":
        if not args.badges:
            parser.error("serve needs --badges")
        args.badges.mkdir(parents=True, exist_ok=True)
        serve(args.badges)
        return 0
    if not args.data:
        parser.error(args.action + " needs --data")
    status = args.data / "achievements/status.ini"
    try:
        if args.action == "login":
            # Never on the command line, where ps would show it.
            try:
                given = json.loads(sys.stdin.read(4096) or "{}")
            except ValueError:
                given = {}
            given = given if isinstance(given, dict) else {}
            name = login(args.data, str(given.get("user") or ""), str(given.get("password") or ""), args.private_dir)
            write_fields(status, {"state": "ready", "message": "Signed in to RetroAchievements as %s." % name})
        else:
            logout(args.data, args.private_dir)
            write_fields(status, {"state": "ready", "message": "Signed out of RetroAchievements."})
        return 0
    except AchievementsError as error:
        write_fields(status, {"state": "error", "message": str(error)})
        return 1


if __name__ == "__main__":
    sys.exit(main())
