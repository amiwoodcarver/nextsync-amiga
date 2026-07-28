#!/usr/bin/env python3
"""
A small Nextcloud-shaped WebDAV server, for testing NextSync without an
account and for taking documentation screenshots that contain nobody's
real server.

    tools/mockdav.py [root-directory] [port]      default: emu/mockdav 8080

It serves the same layout Nextcloud does:

    /remote.php/dav/files/<user>/<path>

and speaks the subset NextSync uses: PROPFIND (Depth 0 and 1), GET, PUT,
MKCOL, DELETE, plus the ETag and X-OC-MTime handling the sync engine
depends on. Any user name and password are accepted.

Amiberry's bsdsocket emulation gives the guest the host's network stack,
so from inside the Amiga this is simply "localhost", port 8080. Use port
8080 rather than 443: NextSync treats 443 as TLS, and this speaks plain
HTTP.
"""

import http.server
import os
import posixpath
import socketserver
import sys
import time
import urllib.parse
from email.utils import formatdate

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "emu/mockdav")
PORT = int(sys.argv[2] if len(sys.argv) > 2 else 8080)
PREFIX = "/remote.php/dav/files/"


def etag_for(path):
    st = os.stat(path)
    return "%x-%x" % (int(st.st_mtime), st.st_size if os.path.isfile(path) else 0)


def xml_escape(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "mockdav/1.0"

    # ---------------------------------------------------------------- #

    def log_message(self, fmt, *args):
        sys.stderr.write("  %s %s\n" % (self.command, self.path))

    def local(self):
        """Map a request path to a file under ROOT, or None if it escapes."""
        path = urllib.parse.unquote(self.path.split("?", 1)[0])
        if not path.startswith(PREFIX):
            return None
        rest = path[len(PREFIX):]
        parts = [p for p in rest.split("/") if p]
        rel = posixpath.join(*parts[1:]) if len(parts) > 1 else ""   # drop user
        full = os.path.abspath(os.path.join(ROOT, rel))
        if full != ROOT and not full.startswith(ROOT + os.sep):
            return None
        return full

    def send_body(self, code, body, ctype="application/xml; charset=utf-8",
                  extra=None):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    def fail(self, code):
        self.send_body(code, b"", "text/plain")

    # ---------------------------------------------------------------- #

    def entry(self, full, href):
        is_dir = os.path.isdir(full)
        st = os.stat(full)
        rt = "<d:collection/>" if is_dir else ""
        size = "" if is_dir else \
            "<d:getcontentlength>%d</d:getcontentlength>" % st.st_size
        return (
            "<d:response>"
            "<d:href>%s</d:href>"
            "<d:propstat><d:prop>"
            "<d:resourcetype>%s</d:resourcetype>"
            "<d:getetag>&quot;%s&quot;</d:getetag>"
            "<d:getlastmodified>%s</d:getlastmodified>"
            "%s"
            "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>"
            "</d:response>"
        ) % (xml_escape(href), rt, etag_for(full),
             formatdate(st.st_mtime, usegmt=True), size)

    def do_PROPFIND(self):
        full = self.local()
        length = int(self.headers.get("Content-Length", 0))
        if length:
            self.rfile.read(length)
        if not full or not os.path.exists(full):
            return self.fail(404)

        depth = self.headers.get("Depth", "1")
        base = self.path.split("?", 1)[0].rstrip("/")
        out = ['<?xml version="1.0"?><d:multistatus xmlns:d="DAV:">']
        out.append(self.entry(full, base + ("/" if os.path.isdir(full) else "")))

        if depth == "1" and os.path.isdir(full):
            for name in sorted(os.listdir(full)):
                if name.startswith("."):
                    continue
                child = os.path.join(full, name)
                href = base + "/" + urllib.parse.quote(name)
                if os.path.isdir(child):
                    href += "/"
                out.append(self.entry(child, href))

        out.append("</d:multistatus>")
        self.send_body(207, "".join(out))

    def do_GET(self):
        full = self.local()
        if not full or not os.path.isfile(full):
            return self.fail(404)
        with open(full, "rb") as f:
            data = f.read()
        self.send_body(200, data, "application/octet-stream",
                       {"ETag": '"%s"' % etag_for(full)})

    def do_HEAD(self):
        self.do_GET()

    def do_PUT(self):
        full = self.local()
        if not full:
            return self.fail(403)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        length = int(self.headers.get("Content-Length", 0))
        remaining = length
        with open(full, "wb") as f:
            while remaining > 0:
                chunk = self.rfile.read(min(65536, remaining))
                if not chunk:
                    break
                f.write(chunk)
                remaining -= len(chunk)

        mtime = self.headers.get("X-OC-MTime")
        if mtime and mtime.isdigit():
            os.utime(full, (time.time(), int(mtime)))
        self.send_body(201, b"", "text/plain",
                       {"ETag": '"%s"' % etag_for(full)})

    def do_MKCOL(self):
        full = self.local()
        if not full:
            return self.fail(403)
        if os.path.exists(full):
            return self.fail(405)
        os.makedirs(full)
        self.send_body(201, b"", "text/plain")

    def do_DELETE(self):
        full = self.local()
        if not full or not os.path.exists(full):
            return self.fail(404)
        if os.path.isdir(full):
            import shutil
            shutil.rmtree(full)
        else:
            os.remove(full)
        self.send_body(204, b"", "text/plain")


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    os.makedirs(ROOT, exist_ok=True)
    print("mockdav: serving %s on port %d" % (ROOT, PORT))
    print("mockdav: configure NextSync with")
    print("           server localhost")
    print("           port   %d" % PORT)
    print("           user   anything")
    Server(("", PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
