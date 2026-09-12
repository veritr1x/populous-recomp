#!/bin/sh
# A mutex over build/recomp, shared by every script that reads or writes it.
#
# WHO HAS TO TAKE IT
#
#   Any script that reads or writes build/recomp/gen, build/recomp/obj or
#   build/recomp/librecomp_gen.a must hold this lock for as long as it is
#   using them.  That is tools/recomp/build.sh, which regenerates all three;
#   tools/recomp/parity_build.sh, which compiles and links against them; and
#   tools/recomp/headless_build.sh.  A reader needs gen/ and the archive to
#   agree with EACH OTHER, not merely to be individually whole, so holding the
#   lock is the requirement - publishing each artifact atomically would not be
#   enough on its own, and is not relied on.
#
# HOW IT WORKS
#
#   `flock` on build/recomp/.lock, held by a python process for exactly as
#   long as the command it wraps.  The kernel owns the lock: it is released
#   when the holder exits however it exits, so there is no stale state to
#   detect and nothing to reclaim.
#
#   The wrapper passes the locked descriptor down to the command it runs, and
#   the command's own children inherit it in turn.  A flock lives on the open
#   file description, not on the process, so it stands until every descriptor
#   referring to it is closed.  That is what makes killing the wrapper safe:
#   without the hand-down, SIGKILL to the wrapper would free the lock while
#   the compiler it started was still writing into build/recomp, and the next
#   build would walk straight into those files.
#
#   Two earlier versions of this file tried to do it in the shell, with a
#   directory and then with a symlink carrying the owner's pid.  Both had the
#   same shape of bug, and the second one survived a race test: A and B both
#   see a dead owner, A renames the stale lock away and takes a fresh one, and
#   B - still acting on what it saw - renames A's *live* lock away and takes
#   it too.  An atomic rename does not check that what it is renaming is still
#   what was observed, and no sequence of shell primitives closes that without
#   a compare-and-swap.  An advisory lock has no such window because there is
#   nothing to observe: a process either holds it or waits.
#
# USAGE
#
#   From a POSIX sh script, wrapping the whole of it:
#
#       exec "$ROOT/tools/recomp/buildlock.sh" run "$ROOT" "what I am doing" \
#            "$0" --locked "$@"
#
#   or, more usually, source it and let it re-exec:
#
#       BUILDLOCK_SH="$ROOT/tools/recomp/buildlock.sh"
#       . "$BUILDLOCK_SH"
#       buildlock_acquire "$ROOT" "what I am doing" "$SELF" "$@"
#
#   Pass an ABSOLUTE path to the script, resolved before any cd, not "$0".
#   The re-exec runs it by name; a script invoked as ./x.sh that has since
#   changed directory re-execs nothing at all.
#
#   The first argument to buildlock_acquire is where the lock file lives, which
#   is the repository root in normal use but not in the race test, so the path
#   to this script is taken from BUILDLOCK_SH rather than derived from it.
#
#   Set BUILDLOCK_HELD=1 in the environment to declare the lock already held,
#   which is how the re-exec avoids taking it twice, and how a caller that
#   already holds it can invoke another script that wants it.

BUILDLOCK_WAIT=${BUILDLOCK_WAIT:-900}     # seconds to wait for the lock

buildlock_python() {
    if [ -n "$BUILDLOCK_PY" ]; then
        printf '%s' "$BUILDLOCK_PY"
        return
    fi
    for _p in "$1/.venv/bin/python" python3 /usr/bin/python3 python; do
        if [ -x "$_p" ] || command -v "$_p" >/dev/null 2>&1; then
            printf '%s' "$_p"
            return
        fi
    done
    printf '%s' python3
}

# buildlock.sh run ROOT DESCRIPTION COMMAND...
#   Runs COMMAND with build/recomp/.lock held, and exits with its status.
if [ "${1:-}" = "run" ]; then
    _root=$2
    _what=$3
    shift 3
    mkdir -p "$_root/build/recomp"
    BUILDLOCK_HELD=1
    export BUILDLOCK_HELD
    exec "$(buildlock_python "$_root")" - "$_root/build/recomp/.lock" \
         "$_what" "$BUILDLOCK_WAIT" "$@" <<'PY'
import fcntl, os, subprocess, sys, threading, time

path, what, wait, cmd = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4:]
fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o644)

# Say who we are waiting for, once, if we do not get it straight away.  The
# note is advisory only: correctness comes from flock, not from this file.
try:
    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
except OSError:
    try:
        os.lseek(fd, 0, os.SEEK_SET)
        holder = (os.read(fd, 256).decode("utf-8", "replace")
                  .replace("\0", "").strip() or "another build")
    except OSError:
        holder = "another build"
    print("buildlock: waiting for %s" % holder, file=sys.stderr, flush=True)
    done = threading.Event()

    def acquire():
        fcntl.flock(fd, fcntl.LOCK_EX)
        done.set()

    t = threading.Thread(target=acquire, daemon=True)
    t.start()
    if not done.wait(wait):
        print("buildlock: %s still held after %gs; giving up" % (path, wait),
              file=sys.stderr)
        sys.exit(1)

os.ftruncate(fd, 0)
# Truncating does not move the write offset, and a waiter that has been sitting
# in flock still has one from its own read; without this the note lands after a
# run of NULs and the "waiting for" line prints a hole instead of a name.
os.lseek(fd, 0, os.SEEK_SET)
os.write(fd, ("%s (pid %d)\n" % (what, os.getpid())).encode())
os.fsync(fd)

# Hand the locked descriptor to the command, so the lock outlives this
# wrapper for as long as anything it started is still running.  pass_fds
# keeps the number stable and marks it inheritable; everything else is still
# closed across the exec.
try:
    sys.exit(subprocess.call(cmd, pass_fds=(fd,)))
finally:
    # The kernel drops the lock when this process exits; clearing the note
    # first only keeps a stale name from being reported to the next waiter.
    try:
        os.ftruncate(fd, 0)
    except OSError:
        pass
PY
fi

# buildlock_acquire ROOT DESCRIPTION COMMAND...
#   Re-executes the calling script under the lock, unless it is already held.
#   BUILDLOCK_HELD is exported across the re-exec, so a script that runs
#   another locked script from inside the lock does not deadlock on itself.
buildlock_acquire() {
    [ "${BUILDLOCK_HELD:-}" = "1" ] && return 0
    _root=$1
    _what=$2
    shift 2
    exec "${BUILDLOCK_SH:-$_root/tools/recomp/buildlock.sh}" run \
         "$_root" "$_what" "$@"
}

# buildlock_release: nothing to do.  The kernel releases the lock when the
# holder exits; this exists so callers that used the old API still work.
buildlock_release() { :; }
