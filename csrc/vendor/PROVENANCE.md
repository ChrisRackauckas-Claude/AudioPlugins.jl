# Vendored third-party headers

## clap/ — the CLAP plugin ABI

| | |
|---|---|
| Upstream | https://github.com/free-audio/clap |
| Version | 1.2.10 (`CLAP_VERSION_MAJOR.MINOR.REVISION` in `clap/version.h`) |
| Commit | `a47f6badb49d948fd009998f28309cdab78979c9` |
| Vendored | 2026-08-24, `include/clap` copied verbatim, 68 headers, 392 KB |
| Licence | MIT — see `CLAP-LICENSE` (© 2021 Alexandre BIQUE) |

**Vendored rather than fetched, deliberately.** CLAP is header-only: there is no
library to link, a host `dlopen`s the plugin and talks to it through these
declarations. That makes the whole dependency 392 KB of MIT-licensed text with
no build step, so vendoring buys an offline, deterministic, network-free build
and costs nothing a pinned download would have saved. The headers are also
densely interdependent (`clap/clap.h` pulls in essentially all of them), so
vendoring a useful subset is not an option — it is the tree or a fetch.

Verbatim copy, no local edits. To update: replace the directory from a tagged
upstream checkout, update the table above, and re-run the test suite — the ABI
is versioned and `clap_host.c` checks `clap_version_is_compatible` at load.
