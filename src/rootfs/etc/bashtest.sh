# /etc/bashtest.sh -- non-interactive assertions for GNU Bash on GNOS.
#
# Run from /etc/rc on every boot, so `make test` catches a regression in the
# kernel's process, signal or filesystem behaviour the moment it appears.
# There is nobody at the keyboard here: nothing may read from the terminal,
# and nothing may block without a bound.
#
# The tests are ordered by how much of the kernel they need, cheapest first.
# That ordering is the point: when bash dies partway through, the last line
# printed says which kernel facility it died on.  A test that needs fork(2)
# must not run before the one that only needs the parser.

echo "bashtest: bash ${BASH_VERSION} is alive"

fails=0

# Deliberately built out of builtins only.  An assertion helper that shells
# out would be untestable by the very thing it is trying to test.
check() {
    if [ "x$2" = "x$3" ]; then
        echo "bashtest: $1 ok"
    else
        echo "bashtest: $1 FAILED -- expected [$2], got [$3]"
        fails=$((fails + 1))
    fi
}

# ---- the parser and the expansions, no syscalls involved ----------------
check "arithmetic"       "42"      "$((6 * 7))"

v=world
check "variable"         "world"   "$v"
check "string length"    "5"       "${#v}"
check "substring"        "wor"     "${v:0:3}"
check "default expansion" "fallback" "${undefined_var:-fallback}"

# ${p##*/} is basename without a fork.  bash uses it internally too.
p=/usr/bin/busybox.elf
check "suffix strip"     "busybox.elf" "${p##*/}"
check "prefix strip"     "/usr/bin"    "${p%/*}"

case "$p" in
    /usr/*) check "case glob" "yes" "yes" ;;
    *)      check "case glob" "yes" "no"  ;;
esac

n=0
for i in a b c d; do n=$((n + 1)); done
check "for loop"         "4"       "$n"

n=0
while [ $n -lt 3 ]; do n=$((n + 1)); done
check "while loop"       "3"       "$n"

f() { echo "in function $1"; }
check "function call"    "in function x" "$(f x)"

arr=(one two three)
check "array element"    "two"     "${arr[1]}"
check "array length"     "3"       "${#arr[@]}"

# ---- exit status: needs fork(2), execve(2) and wait4(2) -----------------
true
check "true status"      "0"       "$?"
false
check "false status"     "1"       "$?"

# `!` and && / || are how every shell script does error handling.
if ! false; then check "negated status" "y" "y"; else check "negated status" "y" "n"; fi
true && check "and-list"  "y" "y"
false || check "or-list"  "y" "y"

# ---- fork + execve of a real binary -------------------------------------
out=$(/usr/bin/echo external command)
check "external command" "external command" "$out"

# $$ must be this shell's pid, and a child must see a different one.  If
# fork() ever handed the child the parent's pid this is what would catch it.
child_pid=$(/bin/bash -c 'echo $$')
if [ "x$child_pid" = "x$$" ]; then
    echo "bashtest: child pid differs FAILED -- child reported $child_pid too"
    fails=$((fails + 1))
else
    echo "bashtest: child pid differs ok"
fi

# ---- pipelines: fork twice, wire a pipe, collect the last status --------
check "pipeline"         "HELLO"   "$(/usr/bin/echo hello | /usr/bin/tr a-z A-Z)"
check "three-stage pipe" "3"       "$(/usr/bin/printf 'a\nb\nc\n' | /usr/bin/sort | /usr/bin/wc -l)"

# The status of a pipeline is the status of its *last* command.
/usr/bin/echo x | false
check "pipeline status"  "1"       "$?"

# ---- redirection: open(2), dup2(2) and close-on-exec --------------------
tmp=/tmp/bashtest.$$
echo "written by bash" > "$tmp"
check "write redirect"   "written by bash" "$(/usr/bin/cat "$tmp")"

echo "appended" >> "$tmp"
check "append redirect"  "2"       "$(/usr/bin/wc -l < "$tmp")"

# A here-document is a file (or a pipe) the shell fills in itself.
check "here-document"    "heredoc body" "$(/usr/bin/cat <<EOF
heredoc body
EOF
)"

# read(1) from a redirected stdin: the builtin side of the same plumbing.
read line < "$tmp"
check "read builtin"     "written by bash" "$line"

# 2>&1 is dup2 onto a descriptor that is already open.
check "stderr redirect"  "to stderr" "$(/bin/bash -c '/usr/bin/echo to stderr >&2' 2>&1)"

/usr/bin/rm -f "$tmp"

# ---- globbing: opendir(3)/readdir(3) over a real directory --------------
mkdir -p /tmp/bashglob
: > /tmp/bashglob/alpha
: > /tmp/bashglob/beta
: > /tmp/bashglob/gamma
matched=(/tmp/bashglob/*a)          # alpha, beta and gamma all end in "a"
check "glob match count" "3" "${#matched[@]}"
matched=(/tmp/bashglob/g*)
check "glob prefix match" "/tmp/bashglob/gamma" "${matched[0]}"
# A pattern that matches nothing is left alone, which is what stops
# `rm *.tmp` in an empty directory from being a silent no-op.
matched=(/tmp/bashglob/zz*)
check "glob no match" "/tmp/bashglob/zz*" "${matched[0]}"
/usr/bin/rm -rf /tmp/bashglob

# ---- cd / pwd: getcwd(2) and chdir(2) -----------------------------------
here=$PWD
cd /tmp
check "cd changes PWD"   "/tmp"    "$PWD"
check "pwd builtin"      "/tmp"    "$(pwd)"
cd "$here"
check "cd back"          "$here"   "$PWD"

# ---- job control and wait(2) --------------------------------------------
# `sleep 0 &` forks a child that exits at once; wait must reap it and report
# its status.  A background job also needs setpgid(2) to have worked.
/usr/bin/sleep 0 &
bg=$!
wait $bg
check "wait background"  "0"       "$?"

/bin/bash -c 'exit 7' &
wait $!
check "wait exit status" "7"       "$?"

# ---- signals: trap, kill and the exit status of a killed child ----------
trapped=no
trap 'trapped=yes' USR1
kill -USR1 $$
check "trap USR1"        "yes"     "$trapped"
trap - USR1

# A child killed by a signal reports 128+signum.  SIGTERM is 15.
/bin/bash -c 'kill -TERM $$'
check "killed child status" "143"  "$?"

# ---- subshells: fork without exec ---------------------------------------
outer=parent
( outer=child )
check "subshell isolation" "parent" "$outer"
check "subshell output"    "child"  "$( outer=child; echo $outer )"

# ---- the environment survives fork and exec -----------------------------
export BASHTEST_VAR=exported
check "exported var"     "exported" "$(/bin/bash -c 'echo $BASHTEST_VAR')"
check "unexported var"   ""         "$(v_local=nope /bin/bash -c 'echo $v_unexported')"

# ---- shebang: execve returning ENOEXEC, and the interpreter re-exec ------
cat > /tmp/bashshebang.sh <<'EOF'
#!/bin/bash
echo "shebang says $1"
EOF
chmod +x /tmp/bashshebang.sh
check "shebang exec"     "shebang says arg" "$(/tmp/bashshebang.sh arg)"
/usr/bin/rm -f /tmp/bashshebang.sh

# ---- times(2), which is syscall 100 and used to be gethostname ----------
# `time` prints to stderr and its format depends on TIMEFORMAT; all this
# asserts is that it runs at all and the command inside it still works.
check "time keyword"     "timed"   "$( { time /usr/bin/echo timed ; } 2>/dev/null )"

echo "bashtest: $fails failure(s)"
exit $fails
