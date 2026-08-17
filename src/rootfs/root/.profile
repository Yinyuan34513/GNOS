# ~/.profile — read by a *login* shell, which is what login(1) execs.
#
# bash reads ~/.bashrc for interactive non-login shells and this file for
# login shells, and never both.  Since every shell on this system now arrives
# through getty -> login -> `-bash`, without this line the prompt, aliases and
# PATH in ~/.bashrc would simply never be applied.
if [ -r "$HOME/.bashrc" ]; then
    . "$HOME/.bashrc"
fi
