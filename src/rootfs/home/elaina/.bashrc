# ~/.bashrc — the ordinary user's shell setup.
#
# Deliberately almost identical to /root/.bashrc, with one difference that is
# the whole point of having two accounts: the prompt ends in '$' rather than
# '#', and PATH does not include the sbin directories, because nothing in
# them will work for a user who is not root.

if [ -r /etc/bash.bashrc ]; then
    . /etc/bash.bashrc
fi

PS1='\[\e[1;36m\]\u@\h\[\e[0m\]:\[\e[1;34m\]\w\[\e[0m\]\$ '

alias ll='ls -l'
alias la='ls -a'
alias l='ls -CF'

# A reminder of what this account can and cannot do -- printed once, on the
# first interactive shell of the session.
if [ -z "$GNOS_GREETED" ]; then
    export GNOS_GREETED=1
    echo "You are $(id -un) (uid $(id -u)).  /root and /etc/shadow are not yours."
    echo "Switch terminals with Ctrl+Alt+F1 .. F6; log in as root there for admin."
fi
