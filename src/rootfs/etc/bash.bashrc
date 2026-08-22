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

# ---- the desktop (labwc) ------------------------------------------------
# A bare `labwc` at the prompt must come up the same way the boot-time rc run
# does.  GNOS has no seatd/logind, so real libseat needs its noop backend
# (without this the seatd socket connect fails and noop is never tried);
# xkb, the runtime socket dir and the WLR pins all mirror /etc/rc.
export LIBSEAT_BACKEND=noop
export XKB_CONFIG_ROOT=/usr/share/X11/xkb
export XDG_RUNTIME_DIR=/tmp/run
export FONTCONFIG_FILE=/etc/fonts/fonts.conf
export WLR_BACKENDS=drm,libinput
export WLR_RENDERER=pixman
export WLR_DRM_NO_ATOMIC=1
export WLR_LIBINPUT_NO_DEVICES=1
