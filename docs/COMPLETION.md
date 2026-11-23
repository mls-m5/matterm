# Tab completion for the options

`matterm --completion bash|zsh|fish` prints a completion script for `matterm`, `matterm-server` and
`matterm-daemon`; it is generated from the same option tables the programs are checked against
(`src/frontend/clioptions.h`), so it follows them. Install it where your shell looks by itself, no
edit of `.bashrc`, `.zshrc` or `config.fish`:

```sh
# bash (needs the bash-completion package; each command name loads its own file)
mkdir -p ~/.local/share/bash-completion/completions
for c in matterm matterm-server matterm-daemon; do matterm --completion bash > ~/.local/share/bash-completion/completions/$c; done

# zsh: a file named _matterm in a folder of $fpath (print -l $fpath shows them; one in your home,
# ~/.zfunc or ~/.local/share/zsh/site-functions, only has to be listed there once)
matterm --completion zsh > ~/.local/share/zsh/site-functions/_matterm

# fish
mkdir -p ~/.config/fish/completions
for c in matterm matterm-server matterm-daemon; do matterm --completion fish > ~/.config/fish/completions/$c.fish; done
```

It completes the options, the values of `--term` and `--trusted-proxy`, files and folders where an
option wants one, `*.ttf` for the font argument, and for `--daemon-host` the `Host` names of
`~/.ssh/config` (patterns left out; `known_hosts` is never read). Open a new shell afterwards.


