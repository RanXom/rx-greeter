# Test-only Wayland protocols

These XML files are used only by `tests/wl-input.c`, the virtual keyboard and
pointer injector for the nested headless test. They are not linked into
`rx-greeter`.

- `virtual-keyboard-unstable-v1.xml` — from the wlroots tree
  (`protocol/virtual-keyboard-unstable-v1.xml`)
- `wlr-virtual-pointer-unstable-v1.xml` — from wlroots
  (`protocol/wlr-virtual-pointer-unstable-v1.xml`)

Both carry their original MIT copyright notices. They are vendored so
`make integration` is reproducible without fetching wlroots sources.
