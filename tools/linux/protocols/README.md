# Wayland protocol XML (vendored)

The Linux present lab (Linux PR 1, [docs/plans/linux.md](../../../docs/plans/linux.md)) speaks
these protocols. `cmake/linux.cmake` runs `wayland-scanner` over them at build time
(`client-header` and `private-code` into the build tree); nothing generated is committed.

They are copied unchanged from wayland-protocols 1.45, so a build does not depend on the
distribution's copy: Ubuntu 22.04 ships 1.25, which has no fractional-scale-v1, and a newer
protocol file must never change what a pinned build means ("reproducible builds beat fresh
deps", [09](../../../docs/design/09-build-and-test.md)). Each file keeps its own copyright
notice; all five are under the MIT licence printed in it.

| File | Upstream path | Bound at |
|---|---|---|
| `xdg-shell.xml` | `stable/xdg-shell/` | `xdg_wm_base` up to v6 (`suspended`) |
| `presentation-time.xml` | `stable/presentation-time/` | `wp_presentation` **v1** (a variable-rate output reports `refresh` 0) |
| `viewporter.xml` | `stable/viewporter/` | v1 |
| `fractional-scale-v1.xml` | `staging/fractional-scale/` | v1 |
| `xdg-decoration-unstable-v1.xml` | `unstable/xdg-decoration/` | v1, when offered (Mutter offers none) |

Move them deliberately, in their own commit, and re-run the Linux smoke afterwards.
