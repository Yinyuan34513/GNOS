#!/usr/bin/env bash
# tools/keys.sh — drive the GNOS console through the QEMU monitor.
#
# Typing at a headless VM is otherwise impossible: this translates ASCII into
# the "sendkey" commands the monitor understands, so a boot test can exercise
# the shell exactly the way a person would.
#
#   usage: keys.sh <monitor-socket> "text to type"
#          keys.sh <monitor-socket> --key ctrl-c
set -u

sock="$1"; shift

emit() { printf 'sendkey %s\n' "$1"; sleep 0.06; }

to_keys() {
    local s="$1" i c
    for (( i=0; i<${#s}; i++ )); do
        c="${s:i:1}"
        case "$c" in
            [a-z0-9]) emit "$c" ;;
            [A-Z])    emit "shift-$(printf '%s' "$c" | tr A-Z a-z)" ;;
            ' ')      emit spc ;;
            '/')      emit slash ;;
            '.')      emit dot ;;
            '-')      emit minus ;;
            ',')      emit comma ;;
            '&')      emit shift-7 ;;
            '%')      emit shift-5 ;;
            '_')      emit shift-minus ;;
            '|')      emit shift-backslash ;;
            '>')      emit shift-dot ;;
            '<')      emit shift-comma ;;
            $'\n')    emit ret ;;
            *)        ;;
        esac
    done
}

{
    while (( $# )); do
        case "$1" in
            --key)   emit "$2"; shift 2 ;;
            --sleep) sleep "$2"; shift 2 ;;
            *)       to_keys "$1"; shift ;;
        esac
    done
    sleep 0.3
} | socat - "unix-connect:$sock" >/dev/null 2>&1
