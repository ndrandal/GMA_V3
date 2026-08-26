#!/usr/bin/env python3
"""ENC-1006 regression test: gma_server must fail *cleanly* on an occupied port.

Before the fix, an EADDRINUSE on either listening socket escaped main() as an
uncaught boost::system::system_error, so the process died with
"terminate called after throwing an instance of ..." and SIGABRT (exit 134 /
a core dump) instead of a diagnosable error exit.

Two cases are covered, because the server binds two independent sockets:
  * wsPort   -> gma::WebSocketServer's acceptor (opened in its constructor)
  * feedPort -> the market.feedserver ingress (opened in FeedServer::run)

For each: occupy the port from this process, launch gma_server, and assert it
exits non-zero, without "terminate called", and names the port + the OS error.

Usage: port_conflict_test.py <path-to-gma_server>
"""

import os
import socket
import subprocess
import sys
import tempfile

TIMEOUT_S = 30


def bind_held(sock_list):
    """Bind an ephemeral port and keep it listening for the test's duration."""
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 0))
    s.listen(5)
    sock_list.append(s)
    return s.getsockname()[1]


def free_port():
    """Best-effort unused port: bind ephemeral, note it, release it."""
    with socket.socket() as s:
        s.bind(("0.0.0.0", 0))
        return s.getsockname()[1]


def run_server(server, ws_port, feed_port, workdir):
    # Ports must go through the INI: Config::loadFromFile() synthesizes the
    # ingress[] entry (and therefore the feed acceptor's port) while parsing.
    conf = os.path.join(workdir, "port_conflict.conf")
    with open(conf, "w") as fh:
        fh.write("wsPort=%d\nfeedPort=%d\nthreadPoolSize=2\nmetricsEnabled=false\n"
                 % (ws_port, feed_port))

    env = dict(os.environ)
    # A configured forum would replace the ingress list and change what binds.
    for key in ("FORUM_URL", "FORUM_TENANT_ID", "FORUM_AGENT_TOKEN"):
        env.pop(key, None)

    proc = subprocess.run(
        [server, str(ws_port), conf, str(feed_port)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        cwd=workdir, env=env, timeout=TIMEOUT_S,
    )
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def check(name, rc, out):
    print("--- %s: exit=%d ---\n%s" % (name, rc, out))
    problems = []
    if "terminate called" in out:
        problems.append("std::terminate reached (unhandled exception escaped main)")
    if rc == 0:
        problems.append("exit code was 0; a failed bind must exit non-zero")
    if rc < 0 or rc > 128:
        problems.append("died from a signal (exit=%d); expected a normal error exit" % rc)
    if "Address already in use" not in out:
        problems.append("message does not report the OS error")
    for problem in problems:
        print("FAIL [%s]: %s" % (name, problem), file=sys.stderr)
    return not problems


def main():
    if len(sys.argv) != 2:
        print("usage: port_conflict_test.py <gma_server>", file=sys.stderr)
        return 2
    server = sys.argv[1]

    held = []
    ok = True
    try:
        with tempfile.TemporaryDirectory() as workdir:
            # Case 1: wsPort occupied.
            ws_taken = bind_held(held)
            rc, out = run_server(server, ws_taken, free_port(), workdir)
            ok &= check("wsPort occupied (%d)" % ws_taken, rc, out)
            if str(ws_taken) not in out:
                print("FAIL [wsPort]: message does not name port %d" % ws_taken, file=sys.stderr)
                ok = False

            # Case 2: feedPort occupied, wsPort free.
            feed_taken = bind_held(held)
            rc, out = run_server(server, free_port(), feed_taken, workdir)
            ok &= check("feedPort occupied (%d)" % feed_taken, rc, out)
            if str(feed_taken) not in out:
                print("FAIL [feedPort]: message does not name port %d" % feed_taken, file=sys.stderr)
                ok = False
    finally:
        for s in held:
            s.close()

    print("PASS" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
