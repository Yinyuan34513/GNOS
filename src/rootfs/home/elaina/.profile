# ~/.profile — read by the login shell login(1) execs.
if [ -r "$HOME/.bashrc" ]; then
    . "$HOME/.bashrc"
fi
