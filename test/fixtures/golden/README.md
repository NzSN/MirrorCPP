# Golden wire corpus (vendored)

Frozen JSON wire corpus vendored from the Mirrors repo (`Mirrors/test/fixtures/`),
frozen from the **Haskell** ModelMirrors implementation at commit
`349625134441a9263c10dee92a99d1be3d29483b` (see `metadata.json`). This is the
byte-level wire contract MirrorCPP's codecs are replayed against by
`test/unit/golden_corpus_test.cpp` (guide §10 item 1).

**Do not hand-edit.** Refresh by re-copying from the Mirrors repo after any
upstream regeneration (`tools/fixtures/run.sh` there), and update this note if
the upstream commit moves.

| File | Consumed as |
| ---- | ----------- |
| `client_messages.jsonl` (+ `manifest.json`) | encode-side: build → `encode_client_message` → semantic compare (absent ≡ null for optional keys) |
| `mirror_messages.jsonl` (+ `manifest.json`) | decode-side: `decode_mirror_message` must succeed and match the manifest constructor |
| `decode_only.jsonl` | JS-shape decode→re-encode vectors, incl. bare integral number → `#bigint` acceptance (C11 decode parity) |
| `consul_payloads.jsonl` | `health` lines replayed through `detail::parse_registry_entries`; `register` lines are server-side (skipped). NB: MirrorCPP's discovery is deliberately stricter than the reference on malformed `cert-sha256` pins (skips the entry; design 5.4) — the corpus's `fp1` entry pins this divergence |
| `diff_cases.jsonl` | 500 decode/equality vectors: `(expected == actual) ⇔ haskell.tag == "match"` |

`explorer_transcripts.jsonl` is intentionally not vendored: it pins the
server↔apalache JSON-RPC exchange, which a client never produces or consumes.
