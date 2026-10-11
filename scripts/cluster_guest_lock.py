#!/usr/bin/env python3
"""Hold the cluster controller's flock in Docker's Linux kernel.

Keep the host-side CLI flock too: it serializes host invocations. This helper
adds the Linux lock needed to exclude the container watchdog on Docker Desktop.
The shared lock file must already exist and must never be replaced or unlinked.
"""

import argparse
import os
from pathlib import Path
import secrets
import selectors
import stat
import subprocess
import sys
import tempfile
import time


GUEST_CODE = r'''
import fcntl, os, signal, stat, sys, time

def stop(signum, frame):
    raise SystemExit(0)

signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)
descriptor = None
try:
    descriptor = os.open(sys.argv[1], os.O_RDWR | os.O_NOFOLLOW)
    if not stat.S_ISREG(os.fstat(descriptor).st_mode):
        raise OSError()
    deadline = time.monotonic() + float(sys.argv[2])
    while True:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            break
        except BlockingIOError:
            if time.monotonic() >= deadline:
                print("LOCK_TIMEOUT", flush=True)
                raise SystemExit(3)
            time.sleep(0.1)
    print("READY", flush=True)
    sys.stdin.buffer.read()
except OSError:
    print("LOCK_UNAVAILABLE", flush=True)
    raise SystemExit(2)
finally:
    if descriptor is not None:
        os.close(descriptor)
'''


class GuestLockError(Exception):
    pass


class GuestClusterLock:
    """Context manager with subprocess injection for tests without Docker.

    docker_command is the existing trusted Docker CLI prefix, including an
    optional --context. The helper never receives a Docker socket, credentials,
    application mounts, a network, or host ports. Missing images fail closed;
    callers may skip this helper only before automatic recovery is installed.
    """

    def __init__(self, lock_path, *, docker_command=("docker",),
                 image="punto/cluster-recovery:local", acquire_timeout=180,
                 popen=subprocess.Popen, now=time.monotonic):
        self.lock_path = Path(lock_path)
        self.docker_command = list(docker_command)
        self.image = image
        self.acquire_timeout = float(acquire_timeout)
        self.popen, self.now = popen, now
        self.process = None
        self.name = "punto-cluster-lock-" + str(os.getpid()) + "-" + secrets.token_hex(6)
        if not self.docker_command or not image or not 0 < self.acquire_timeout <= 600:
            raise GuestLockError("guest_lock_invalid_configuration")

    def command(self):
        try:
            if self.lock_path.is_symlink() or not stat.S_ISREG(self.lock_path.stat().st_mode):
                raise OSError()
            source = str(self.lock_path.resolve(strict=True))
        except OSError:
            raise GuestLockError("guest_lock_file_unavailable") from None
        if any(character in source for character in (",", "\n", "\r")):
            raise GuestLockError("guest_lock_invalid_path")
        return self.docker_command + [
            "run", "--rm", "--interactive", "--pull=never", "--name", self.name,
            "--label", "io.punto.cluster-lock-helper=true", "--network=none", "--read-only",
            "--user=10001:10001", "--cap-drop=ALL", "--security-opt=no-new-privileges:true",
            "--mount", "type=bind,src=" + source + ",dst=/run/punto-cluster.lock",
            "--entrypoint=python3", self.image, "-u", "-c", GUEST_CODE,
            "/run/punto-cluster.lock", str(self.acquire_timeout),
        ]

    def acquire(self):
        if self.process is not None:
            raise GuestLockError("guest_lock_already_acquired")
        try:
            self.process = self.popen(self.command(), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      stderr=subprocess.DEVNULL, bufsize=0)
            deadline = self.now() + self.acquire_timeout + 10
            output = b""
            with selectors.DefaultSelector() as selector:
                selector.register(self.process.stdout, selectors.EVENT_READ)
                while self.now() < deadline:
                    if not selector.select(min(0.2, max(0, deadline - self.now()))):
                        if self.process.poll() is not None:
                            break
                        continue
                    chunk = os.read(self.process.stdout.fileno(), 1024)
                    if not chunk:
                        break
                    output += chunk
                    if len(output) > 1024:
                        break
                    if b"\n" in output:
                        if output == b"READY\n" and self.process.poll() is None:
                            return self
                        break
            raise GuestLockError("guest_lock_not_acquired")
        except (OSError, GuestLockError) as error:
            try:
                self.release()
            except GuestLockError:
                pass
            if isinstance(error, GuestLockError):
                raise
            raise GuestLockError("guest_lock_helper_unavailable") from None

    def release(self):
        process, self.process = self.process, None
        if process is None:
            return
        try:
            if process.stdin:
                process.stdin.close()  # EOF releases flock in the Linux helper.
            try:
                code = process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.terminate()  # Docker forwards SIGTERM; guest PID1 handles it.
                try:
                    code = process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
                    raise GuestLockError("guest_lock_helper_shutdown_failed") from None
            if code != 0:
                raise GuestLockError("guest_lock_helper_failed")
        except OSError:
            raise GuestLockError("guest_lock_helper_shutdown_failed") from None
        finally:
            if process.stdout:
                process.stdout.close()

    def assert_held(self):
        if self.process is None or self.process.poll() is not None:
            raise GuestLockError("guest_lock_lost")

    def __enter__(self):
        return self.acquire()

    def __exit__(self, error_type, error, traceback):
        try:
            self.release()
        except GuestLockError:
            if error_type is None:
                raise
        return False


def cross_container_proof(image, docker_command=("docker",)):
    """Create only two/three networkless helpers and a disposable empty file."""
    with tempfile.TemporaryDirectory(prefix="punto-guest-lock-") as directory:
        path = Path(directory) / "controller.lock"
        path.touch(mode=0o666)
        path.chmod(0o666)
        with GuestClusterLock(path, image=image, docker_command=docker_command):
            try:
                with GuestClusterLock(path, image=image, docker_command=docker_command, acquire_timeout=1):
                    raise GuestLockError("guest_lock_cross_container_exclusion_failed")
            except GuestLockError as error:
                if str(error) != "guest_lock_not_acquired":
                    raise
        with GuestClusterLock(path, image=image, docker_command=docker_command):
            pass
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cross-container-proof", action="store_true")
    parser.add_argument("--image", default="punto/cluster-recovery:local")
    parser.add_argument("--docker", default="docker")
    parser.add_argument("--context")
    args = parser.parse_args()
    if not args.cross_container_proof:
        print("Plan: networkless guest helper holds only the existing shared lock file; no SQL or production lifecycle.")
        return 0
    command = [args.docker] + (["--context", args.context] if args.context else [])
    try:
        cross_container_proof(args.image, command)
        print("guest_lock cross_container_exclusion=PASS release_reacquire=PASS")
        return 0
    except GuestLockError as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
