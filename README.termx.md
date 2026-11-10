# termx-bash

This repository is the termx-maintained GNU bash source tree. It adds the
structured shell lifecycle event channel used by the termx terminal runtime.

## Repositories and branches

- Upstream: https://git.savannah.gnu.org/git/bash.git
- termx fork: https://github.com/xiaoqiong0v0/termx-bash
- termx branch: `termx/events`

## termx changes

- `bash_event.c` / `bash_event.h` add a Unix-domain-socket event channel.
- `eval.c`, `shell.c`, and `builtins/exit.def` emit `A/B/C/D/X` lifecycle
  events, including the shell's authoritative exit status.
- Events are sent over the host-provided socket; nothing is written to stdout
  and no OSC marker is injected.
- Event-channel failures permanently degrade to a no-op; the shell never
  blocks or fabricates transcript state.

## Build and test

The tree builds like upstream bash:

```sh
./configure
make -j"$(nproc)"
```

The termx integration is built by the parent project with the same source tree;
the parent repository's tests cover the event protocol and PTY ordering.

## Syncing with upstream

Keep `upstream` pointed at the Savannah bash repository, rebase `termx/events`
onto the desired bash release, and re-run the termx event build after resolving
changes in `eval.c` or `shell.c`.

## Parent project

The parent `opencode-termx-cli` repository includes this repository as the
`terminals/termx-bash` submodule.
