#!/usr/bin/env python3
"""ENC-1331 regression test: `gma_server <ws> <conf> <feed>` must BIND <feed>.

argv[3] was accepted, logged, and then ignored by the thing that opens the
socket. `Config::loadFromFile()` ended with `synthesizeIngressFromLegacy()`,
which copied the FILE's `feedPort` into the `market.feedserver` ingress params;
main.cpp applied argv[3] to `cfg.feedPort` only afterwards, and nothing revisited
the params. The boot log printed the port the server INTENDED while the acceptor
used the file's — so a log assertion passed while the bind was wrong.

This test therefore asserts the *bind*: it connects to the overridden port and
proves the file's port is NOT listening. Run against the real binary, because
the defect lived in the composition root's ordering, not in Config alone.

Usage: feed_port_override_test.py <path-to-gma_server>
"""

import os
import socket
import subprocess
import sys
import tempfile
import time

BOOT_TIMEOUT_S = 25


def free_port():
    """Best-effort unused port: bind ephemeral, note it, release it."""
    with socket.socket() as s:
        s.bind(("0.0.0.0", 0))
        return s.getsockname()[1]


def can_connect(port, timeout=0.4):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout):
            return True
    except OSError:
        return False


def main():
    if len(sys.argv) != 2:
        print("usage: feed_port_override_test.py <gma_server>", file=sys.stderr)
        return 2
    server = sys.argv[1]

    conf_feed_port = free_port()   # what the config file asks for
    argv_feed_port = free_port()   # what argv[3] asks for — must win
    ws_port = free_port()
    if conf_feed_port == argv_feed_port:
        print("FAIL: could not pick two distinct free ports", file=sys.stderr)
        return 1

    problems = []
    with tempfile.TemporaryDirectory() as workdir:
        conf = os.path.join(workdir, "feed_port_override.conf")
        with open(conf, "w") as fh:
            fh.write("wsPort=%d\nfeedPort=%d\nthreadPoolSize=2\nmetricsEnabled=false\n"
                     % (ws_port, conf_feed_port))

        env = dict(os.environ)
        # A configured forum would replace the ingress list and change what binds.
        for key in ("FORUM_URL", "FORUM_TENANT_ID", "FORUM_AGENT_TOKEN"):
            env.pop(key, None)

        log = open(os.path.join(workdir, "server.log"), "w+b")
        proc = subprocess.Popen(
            [server, str(ws_port), conf, str(argv_feed_port)],
            stdout=log, stderr=subprocess.STDOUT, cwd=workdir, env=env,
        )
        try:
            deadline = time.time() + BOOT_TIMEOUT_S
            bound = False
            while time.time() < deadline:
                if proc.poll() is not None:
                    break
                if can_connect(argv_feed_port):
                    bound = True
                    break
                time.sleep(0.2)

            # Probe both before tearing down.
            conf_port_listening = can_connect(conf_feed_port)
            exited_early = proc.poll()
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=10)
            log.flush()
            log.seek(0)
            out = log.read().decode("utf-8", "replace")
            log.close()

    print("--- conf feedPort=%d, argv[3] feedPort=%d, wsPort=%d ---\n%s"
          % (conf_feed_port, argv_feed_port, ws_port, out))

    if exited_early is not None:
        problems.append("server exited early (rc=%s) instead of serving" % exited_early)
    if not bound:
        problems.append("nothing accepted on argv[3] feedPort %d — the override "
                        "did not reach the ingress that binds" % argv_feed_port)
    if conf_port_listening:
        problems.append("the config file's feedPort %d is listening; argv[3] (%d) "
                        "lost to the file" % (conf_feed_port, argv_feed_port))

    for problem in problems:
        print("FAIL: %s" % problem, file=sys.stderr)
    print("PASS" if not problems else "FAILED")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
