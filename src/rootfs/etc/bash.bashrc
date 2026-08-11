# /etc/bash.bashrc — system-wide interactive bash settings for GNOS.
#
# Sourced from /root/.bashrc (and, on a multi-user setup, from every user's
# own ~/.bashrc).  Kept deliberately small: this is a single-user OS and the
# interesting per-user stuff lives in /root/.bashrc.

# A friendly banner the first time an interactive shell starts.
if [ -z "$GNOS_BASH_WELCOMED" ]; then
    export GNOS_BASH_WELCOMED=1
    echo "GNOS — interactive bash ready ($(uname -s) $(uname -r) on $(uname -m))"
fi

# Make Ctrl-D not log anyone out instantly, and give the line editor a sensible
# width hint in case the winsize ioctl ever lies.
export INPUTRC=
