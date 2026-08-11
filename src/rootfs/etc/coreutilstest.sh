# /etc/coreutilstest.sh -- non-interactive assertions for GNU coreutils on GNOS.
#
# Run from /etc/rc on every boot, next to bashtest.sh and for the same reason:
# coreutils is the largest body of third-party code in the image and it leans
# on far more of the kernel than BusyBox ever did.  gnulib wraps almost every
# syscall in a probe-and-fall-back, so a missing feature here does not crash --
# it silently produces the wrong answer.  Only assertions catch that.
#
# Nothing may read from the terminal and nothing may block unbounded: there is
# nobody at the keyboard during `make test`.
#
# Ordered cheapest-first, like bashtest.sh, so that when a program dies the
# last line printed names the kernel facility it died on.

CU=/usr/bin
T=/tmp/cutest

fails=0

# Built from bash builtins only -- an assertion helper that shelled out to
# coreutils could not report a broken coreutils.
check() {
    if [ "x$2" = "x$3" ]; then
        echo "coreutils: $1 ok"
    else
        echo "coreutils: $1 FAILED -- expected [$2], got [$3]"
        fails=$((fails + 1))
    fi
}

# ---- identity ------------------------------------------------------------
# The single most important assertion in the file.  /usr/bin/ls is a name both
# BusyBox and coreutils want; if the initrd staging order ever flips, every
# test below would quietly pass against BusyBox instead and prove nothing.
ver=$($CU/ls --version 2>/dev/null | head -1)
case "$ver" in
    *"GNU coreutils"*) check "GNU coreutils in /usr/bin" "yes" "yes" ;;
    *)                 check "GNU coreutils in /usr/bin" "yes" "no ($ver)" ;;
esac
echo "coreutils: $ver"

# ---- no syscalls beyond write(2) ----------------------------------------
check "echo"        "hello"       "$($CU/echo hello)"
check "printf"      "a-b"         "$($CU/printf '%s-%s' a b)"
check "seq"         "1 2 3 4 5"   "$($CU/seq -s' ' 5)"
check "factor"      "12: 2 2 3"   "$($CU/factor 12)"
check "basename"    "rc"          "$($CU/basename /etc/rc)"
check "dirname"     "/etc"        "$($CU/dirname /etc/rc)"
check "true/false"  "0 1"         "$($CU/true; a=$?; $CU/false; echo "$a $?")"
check "expr"        "7"           "$($CU/expr 3 + 4)"
check "test -d"     "0"           "$($CU/test -d /etc; echo $?)"

# ---- read(2) on the ext2 root -------------------------------------------
check "cat"         "1"           "$($CU/cat /etc/hostname | $CU/wc -l)"
check "head -1"     "1"           "$($CU/head -1 /etc/rc | $CU/wc -l)"
check "wc -c"       "$($CU/stat -c%s /etc/hostname)" "$($CU/cat /etc/hostname | $CU/wc -c)"
check "md5sum agrees with itself" \
      "$($CU/md5sum < /etc/rc | $CU/cut -d' ' -f1)" \
      "$($CU/md5sum /etc/rc | $CU/cut -d' ' -f1)"

# base64 round-trips through two pipes and a fork each way.  Its output is
# also the only place in this file where a >76-column line gets wrapped, which
# is a decent smoke test for the buffered-write path.
check "base64 round trip" "GNOS" "$($CU/echo -n GNOS | $CU/base64 | $CU/base64 -d)"

# sha256 of the empty string: a fixed constant, so this checks the hash
# implementation rather than just its self-consistency.
check "sha256sum" "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" \
      "$($CU/printf '' | $CU/sha256sum | $CU/cut -d' ' -f1)"

# ---- the write side of the filesystem ------------------------------------
# /tmp is a tmpfs mounted by `mount -a` earlier in /etc/rc, so this exercises
# the tmpfs driver; the ext2 root is read from above.
$CU/rm -rf "$T"
$CU/mkdir -p "$T/a/b/c"
check "mkdir -p"    "0"           "$($CU/test -d $T/a/b/c; echo $?)"

$CU/printf 'one\ntwo\nthree\n' > "$T/f"
check "shell redirect + wc -l" "3" "$($CU/wc -l < $T/f)"
check "tail -1"     "three"       "$($CU/tail -1 $T/f)"
# paste -s joins the lines back into one, so the expected value stays readable
# on a single line here instead of being an embedded newline soup.
check "sort"        "one three two" "$($CU/sort $T/f | $CU/paste -s -d' ')"
check "uniq"        "3"           "$($CU/sort $T/f | $CU/uniq | $CU/wc -l)"
check "cut -c1-2"   "on tw th"    "$($CU/cut -c1-2 $T/f | $CU/paste -s -d' ')"
check "tr"          "ONE TWO THREE" "$($CU/tr a-z A-Z < $T/f | $CU/paste -s -d' ')"

$CU/cp "$T/f" "$T/g"
check "cp"          "$($CU/md5sum < $T/f)" "$($CU/md5sum < $T/g)"
$CU/mv "$T/g" "$T/h"
check "mv"          "1 0"         "$($CU/test -e $T/g; a=$?; $CU/test -e $T/h; echo "$a $?")"

# truncate(2) both ways -- grow into a hole, then shrink.
$CU/truncate -s 4096 "$T/h"
check "truncate grow"   "4096"    "$($CU/stat -c%s $T/h)"
$CU/truncate -s 7 "$T/h"
check "truncate shrink" "7"       "$($CU/stat -c%s $T/h)"

# touch(1) is utimensat(2); coreutils checks the result, so a no-op kernel
# stub that returns 0 without setting anything is caught here.
$CU/rm -f "$T/new"
$CU/touch "$T/new"
check "touch creates"   "0"       "$($CU/test -f $T/new; echo $?)"

# symlink(2)/readlink(2) -- realpath resolves the whole chain, which is more
# than fstest.elf's single-hop check.
$CU/ln -s f "$T/link"
check "ln -s + readlink" "f"      "$($CU/readlink $T/link)"
check "realpath"        "$T/f"    "$($CU/realpath $T/link)"

$CU/rm -rf "$T/a"
check "rm -rf"      "1"           "$($CU/test -d $T/a; echo $?)"

# ---- the kernel proper ---------------------------------------------------
# nproc reads the CPU count out of sched_getaffinity(2)/sysconf; GNOS is
# uniprocessor, so anything other than 1 means the syscall lied.
check "nproc"       "1"           "$($CU/nproc)"

# id/whoami parse /etc/passwd through musl's NSS.  Running as root here.
check "whoami"      "root"        "$($CU/whoami)"
check "id -u"       "0"           "$($CU/id -u)"

# uname(2)
check "uname -s"    "GNOS"        "$($CU/uname -s)"

# SIGPIPE: `yes` writes forever and must die when head closes the pipe.  If
# the kernel never raises SIGPIPE this hangs the boot until the test timeout,
# so it is deliberately placed after everything that still needs to run.
check "yes | head (SIGPIPE)" "3"  "$($CU/yes gnos | $CU/head -3 | $CU/wc -l)"

# timeout(1) is alarm/setitimer + SIGTERM + waitpid; 124 is its documented
# exit status for "the command was still running".
$CU/timeout 1 $CU/sleep 5
check "timeout kills"   "124"     "$?"

# sleep(1) itself must *not* be killed early.
$CU/timeout 5 $CU/sleep 1
check "timeout spares"  "0"       "$?"

$CU/rm -rf "$T"

echo "coreutils: $fails failure(s)"
exit $fails
