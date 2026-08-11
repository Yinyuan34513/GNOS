# ~/.bashrc — GNOS root shell setup.  Sourced by interactive, non-login bash.
#
# GNOS boots straight into an interactive bash (init execs /bin/bash with
# stdin/stdout/stderr on /dev/tty, and the tty answers isatty()), so this file
# is what turns the bare prompt into something usable.

# Pull in the system-wide settings first.
if [ -r /etc/bash.bashrc ]; then
    . /etc/bash.bashrc
fi

# A colourful prompt.  The escapes are bash's own PS1 expansions, not readline
# control codes, so they survive our minimal terminal just fine.
PS1='\[\e[1;32m\]\u@\h\[\e[0m\]:\[\e[1;34m\]\w\[\e[0m\]\$ '

# A couple of conveniences -- harmless even if the underlying commands are the
# BusyBox applets rather than the GNU ones.
alias ll='ls -l'
alias la='ls -a'
alias l='ls -CF'

# Let coreutils' ls emit colours when it can; BusyBox ls ignores --color.
export LS_COLORS=''

# The toy shells can't do this, but bash can: keep a sane history.
export HISTSIZE=500
export HISTFILE=/root/.bash_history
