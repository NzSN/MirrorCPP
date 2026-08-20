# Vendored fixtures

Test/spec fixtures copied verbatim from the upstream ModelMirrors and MirrorECMA
repositories (single shallow clone, then removed — nothing else from those repos
lives here).

| File(s) | Origin | Used by |
|---|---|---|
| `HourClock.tla` | ModelMirrors `test/specs/HourClock.tla` (identical in MirrorECMA) | stepping-flow integration tests, hourclock example |
| `Counter.tla` | ModelMirrors `specs/Counter.tla` | register/validate integration tests |
| `ExtMain.tla`, `ExtDep.tla` | MirrorECMA `specs/` (also present in ModelMirrors `test/specs/`) | `spec_from_files` inline multi-module spec tests |
| `traces/all_steps_done.itf.json`, `step_mismatch.itf.json`, `register_error.itf.json`, `explore_session.itf.json`, `explore_cmd.itf.json`, `fault_close.itf.json` | ModelMirrors `specs/traces/` | `run_client_with_traces`, MBT conformance replay (design §8) |
| `traces/violation*.itf.json` (38 files) | MirrorECMA `specs/traces/` | smoke/validation trace fixtures |

The 44 trace files cover the protocol-state-machine cases named in design §8
(`all_steps_done`, `step_mismatch`, `register_error`, `explore_session`,
`explore_cmd`, `fault_close`) plus MirrorECMA's violation traces.

## Real-mirror build provenance (t13)

The MIRROR_BIN-gated integration tests (design §8) run against a real
ModelMirrors binary built from upstream HEAD:

- **Upstream commit**: `8f5749fad8289940d3ad621db7fd09649be31ada`
  (github.com/NzSN/ModelMirrors, upstream HEAD at build time; the local
  `third_party/modelmirrors/` checkout is a squashed snapshot of it).
- **Source**: `.mirror-src/` (root, gitignored). **Binary**: 
  `.mirror-bin/ModelMirrors` (root, gitignored, built by `cabal build`;
  the whole `third_party/` tree is gitignored build tooling, not vendored).
- **Test certs** (mTLS scenarios): `.mirror-bin/certs/` — CA + server (SAN
  IP:127.0.0.1, CN=127.0.0.1) + client (CN=modelmirrors-client), private keys
  mode 0600. Regenerate with `.mirror-src/scripts/gen-certs.sh
  .mirror-bin/certs 127.0.0.1 <days>`.
- Set `MIRROR_BIN=<repo>/.mirror-bin/ModelMirrors` to run the real-mirror
  tests. Do **not** use `~/.local/bin/ModelMirrors` — it predates
  `register_validate` and is stale.
