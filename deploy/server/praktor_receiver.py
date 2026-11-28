"""Receive signed release instructions relayed by one authenticated Flowie instance."""

import argparse
import hashlib
import hmac
import json
import os
import re
import signal
import sqlite3
import ssl
import subprocess
import tarfile
import tempfile
import threading
from contextlib import closing, contextmanager
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path


MAX_YAML_BYTES = 65536
MAX_TAR_BYTES = 32 * 1024 * 1024
MAX_PENDING_BYTES = 64 * 1024 * 1024
MAX_TAR_ENTRIES = 256
TAR_BLOCK = 512
YAML_TYPE = "application/yaml"
TAR_TYPE = "application/x-tar"
MAX_PENDING = 32
MAX_RECORDS = 1024
DIGEST = re.compile(r"[0-9a-f]{64}")
KEY = re.compile(r"[A-Za-z0-9._-]{1,128}")
DOMAIN = re.compile(r"[A-Za-z0-9_-]{1,255}")
PATH_PART = re.compile(r"[a-z0-9._-]+")
DEVICE_STEM = re.compile(r"(aux|con|nul|prn|com[0-9]|lpt[0-9])")


def open_db(path):
    return closing(sqlite3.connect(path, timeout=2))


@contextmanager
def receiver_lock(path):
    """Keep one execution authority per spool, including recovery."""
    with open(str(Path(path).resolve()) + ".lock", "a+b") as lock:
        lock.seek(0)
        if os.name == "nt":
            import msvcrt

            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl

            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


class Receiver(HTTPServer):
    def get_request(self):
        connection, address = super().get_request()
        connection.settimeout(5)
        try:
            return self.tls.wrap_socket(connection, server_side=True), address
        except OSError:
            connection.close()
            raise


def bundle_path(name):
    parts = name.split("/")
    if len(name) > 240 or any(not PATH_PART.fullmatch(part) or part.endswith(".")
                             or DEVICE_STEM.fullmatch(part.split(".")[0]) for part in parts):
        raise ValueError("invalid archive path")
    return parts


def tar_files(body):
    if not 2 * TAR_BLOCK <= len(body) <= MAX_TAR_BYTES or len(body) % TAR_BLOCK:
        raise ValueError("invalid TAR size")
    view = memoryview(body)
    files = {}
    offset = 0
    while offset + TAR_BLOCK <= len(body):
        header = body[offset:offset + TAR_BLOCK]
        if header == bytes(TAR_BLOCK):
            if len(body) - offset < 2 * TAR_BLOCK or any(view[offset:]):
                raise ValueError("invalid TAR end")
            return files
        if len(files) == MAX_TAR_ENTRIES or header[257:265] != b"ustar\x0000":
            raise ValueError("invalid USTAR header")
        entry = tarfile.TarInfo.frombuf(header, "ascii", "strict")
        bundle_path(entry.name)
        if entry.type not in (tarfile.REGTYPE, tarfile.AREGTYPE) or entry.size < 0 or entry.name in files:
            raise ValueError("invalid TAR entry")
        start = offset + TAR_BLOCK
        end = start + entry.size
        offset = start + ((entry.size + TAR_BLOCK - 1) // TAR_BLOCK) * TAR_BLOCK
        if offset > len(body) or any(view[end:offset]):
            raise ValueError("invalid TAR payload")
        files[entry.name] = view[start:end]
    raise ValueError("missing TAR end")


def workflow_files(body, content_type):
    if content_type == YAML_TYPE:
        entrypoint, files = "instructions.yml", {"instructions.yml": body}
    else:
        files = tar_files(body)
        raw_manifest = files.pop("manifest.json")
        signature = files.pop("manifest.p7s")
        if not signature or not 0 < len(raw_manifest) <= MAX_YAML_BYTES:
            raise ValueError("invalid manifest entries")
        # The pinned Flowie service verifies the CMS signature and every payload digest.
        manifest = json.loads(bytes(raw_manifest))
        entrypoint = manifest["entrypoints"]["praktor"]
        declared = {item["path"]: item["size"] for item in manifest["files"]}
        if (len(declared) != len(manifest["files"]) or declared.keys() != files.keys()
                or any(len(files[name]) != size for name, size in declared.items())):
            raise ValueError("unexpected TAR file set")
        for name in files:
            parts = bundle_path(name)
            if any("/".join(parts[:i]) in files for i in range(1, len(parts))):
                raise ValueError("TAR entry conflicts with parent path")
    yaml = bytes(files[entrypoint])
    if not 0 < len(yaml) <= MAX_YAML_BYTES or b"\0" in yaml:
        raise ValueError("invalid YAML text length")
    yaml.decode("utf-8")
    return entrypoint, files


class Jobs(BaseHTTPRequestHandler):
    def do_POST(self):
        self.close_connection = True
        config = self.server.config
        peer = self.connection.getpeercert(binary_form=True)
        fingerprint = hashlib.sha256(peer).hexdigest()
        if not hmac.compare_digest(fingerprint, config.flowie_pin):
            self.reply(403)
            return
        if self.path != "/jobs":
            self.reply(404)
            return
        digest = self.headers.get("Flowie-Release-Digest", "")
        key = self.headers.get("Idempotency-Key", "")
        lengths = self.headers.get_all("Content-Length", [])
        if (self.headers.get("Flowie-Domain") != config.domain
                or not DIGEST.fullmatch(digest) or not KEY.fullmatch(key)
                or len(lengths) != 1 or not re.fullmatch(r"[0-9]{1,8}", lengths[0])
                or self.headers.get("Transfer-Encoding") is not None):
            self.reply(400)
            return
        content_type = self.headers.get("Content-Type")
        if (content_type not in (YAML_TYPE, TAR_TYPE)
                or self.headers.get("Content-Encoding", "identity") != "identity"):
            self.reply(415)
            return
        size = int(lengths[0])
        if not 0 < size <= (MAX_TAR_BYTES if content_type == TAR_TYPE else MAX_YAML_BYTES):
            self.reply(413)
            return
        try:
            body = self.rfile.read(size)
        except OSError:
            self.reply(400)
            return
        if len(body) != size:
            self.reply(400)
            return
        try:
            workflow_files(body, content_type)
        except (ValueError, TypeError, KeyError, RecursionError, tarfile.TarError):
            self.reply(422)
            return
        if not self.server.worker.is_alive():
            self.reply(503)
            return
        try:
            status = submit(config.database, digest, key, body, content_type)
        except sqlite3.OperationalError:
            status = 503
        if status == 202:
            self.server.changed.set()
        self.reply(status)

    def reply(self, status):
        self.send_response(status)
        self.send_header("Content-Length", "0")
        self.send_header("Connection", "close")
        self.end_headers()


def submit(path, digest, key, body, content_type):
    content_hash = hashlib.sha256(body).digest()
    with open_db(path) as db, db:
        db.execute("BEGIN IMMEDIATE")
        prior = db.execute("SELECT yaml_hash, content_type FROM jobs WHERE digest=? AND job_key=?",
                           (digest, key)).fetchone()
        if prior:
            return 202 if prior == (content_hash, content_type) else 409
        total, pending, pending_bytes = db.execute(
            "SELECT COUNT(*), COUNT(yaml), COALESCE(SUM(LENGTH(yaml)), 0) "
            "FROM jobs"
        ).fetchone()
        if (total >= MAX_RECORDS or pending >= MAX_PENDING
                or pending_bytes + len(body) > MAX_PENDING_BYTES):
            return 503
        db.execute("INSERT INTO jobs (digest, job_key, yaml, yaml_hash, state, content_type) "
                   "VALUES (?, ?, ?, ?, 'queued', ?)", (digest, key, body, content_hash, content_type))
    return 202


def execute(config, body, content_type):
    try:
        entrypoint, files = workflow_files(body, content_type)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, data in files.items():
                path = root.joinpath(*bundle_path(name))
                path.parent.mkdir(parents=True, exist_ok=True)
                with path.open("xb") as artifact:
                    artifact.write(data)
            source = root.joinpath(*bundle_path(entrypoint))
            result = subprocess.run(
                [config.executable, "--config", str(source), "--task", config.task,
                 "--timeout-ms", str(config.timeout_ms)],
                cwd=directory, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, timeout=config.timeout_ms / 1000 + 5, check=False,
            )
            return "succeeded" if result.returncode == 0 else "failed"
    except subprocess.TimeoutExpired:
        return "unknown"
    except (OSError, ValueError, TypeError, KeyError, RecursionError, tarfile.TarError):
        return "failed"


def run_jobs(server):
    config = server.config
    with open_db(config.database) as db:
        while not server.stopping.is_set():
            server.changed.clear()
            with db:
                db.execute("BEGIN IMMEDIATE")
                item = db.execute("SELECT id, yaml, content_type FROM jobs WHERE state='queued' "
                                  "ORDER BY id LIMIT 1").fetchone()
                if item:
                    db.execute("UPDATE jobs SET state='running' WHERE id=?", (item[0],))
            if not item:
                if not server.stopping.is_set():
                    server.changed.wait()
                continue
            state = execute(config, item[1], item[2])
            with db:
                db.execute("UPDATE jobs SET state=?, yaml=NULL WHERE id=?", (state, item[0]))


def serve(config):
    with open_db(config.database) as db, db:
        db.execute("BEGIN IMMEDIATE")
        db.execute("CREATE TABLE IF NOT EXISTS jobs (id INTEGER PRIMARY KEY, digest TEXT NOT NULL, "
                   "job_key TEXT NOT NULL, yaml BLOB, yaml_hash BLOB NOT NULL, state TEXT NOT NULL, "
                   "content_type TEXT NOT NULL DEFAULT 'application/yaml', UNIQUE (digest, job_key))")
        if "content_type" not in {row[1] for row in db.execute("PRAGMA table_info(jobs)")}:
            db.execute("ALTER TABLE jobs ADD COLUMN content_type TEXT NOT NULL DEFAULT 'application/yaml'")
        db.execute("UPDATE jobs SET state='unknown', yaml=NULL WHERE state='running'")
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.minimum_version = ssl.TLSVersion.TLSv1_2
    tls.load_cert_chain(config.cert, config.key)
    tls.load_verify_locations(config.client_ca)
    tls.verify_mode = ssl.CERT_REQUIRED
    with Receiver((config.host, config.port), Jobs) as server:
        server.config = config
        server.tls = tls
        server.changed = threading.Event()
        server.stopping = threading.Event()
        server.worker = threading.Thread(target=run_jobs, args=(server,))
        server.worker.start()
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            server.stopping.set()
            server.changed.set()
            server.worker.join()


def stop_on_signal(_signal, _frame):
    raise KeyboardInterrupt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("domain", "flowie-pin", "cert", "key", "client-ca", "database",
                 "executable", "task"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9444)
    parser.add_argument("--timeout-ms", type=int, default=30000)
    config = parser.parse_args()
    if (not DOMAIN.fullmatch(config.domain) or not DIGEST.fullmatch(config.flowie_pin)
            or not KEY.fullmatch(config.task) or config.task.startswith("--")
            or not Path(config.executable).is_absolute() or not Path(config.executable).is_file()
            or not 0 < config.timeout_ms <= 300000 or not 0 < config.port <= 65535):
        parser.error("invalid Domain, certificate pin, task, executable, port or timeout")
    signal.signal(signal.SIGTERM, stop_on_signal)
    with receiver_lock(config.database):
        serve(config)


if __name__ == "__main__":
    main()
