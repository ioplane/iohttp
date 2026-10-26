[![GitHub](https://img.shields.io/badge/GitHub-iohttp-181717)](https://github.com/ioplane/iohttp)

# iohttp Agent Development Plan

Date: 2026-10-26. Audited default branch: `main` at
`1d1cc9de367ed75c527eb4537ed01bf8ee52a75f`. Planning audit: `ngtun-i8s.42.2`.
Durable execution work order: `ngtun-4zy.5` under independent portfolio `ngtun-4zy`.

Historical plans and local uncommitted August plans remain preserved. This dated
plan supplies the next validation/implementation contracts; it does not rewrite past
measurements, sprint completion records or current publish decisions.

## Verified baseline

HTTP/1, HTTP/2, HTTP/3 and io_uring modules exist in the default tree; their presence is
not release qualification. The HTTP/2 public callback is explicitly invoked only for a
complete request at END_STREAM (`src/http/ioh_http2.h:37-44`). The data callback grows
and accumulates a body (`src/http/ioh_http2.c:422-439`) before dispatch at lines 470-489.
The historical Sprint 17 streaming proposal is not evidence of a usable gRPC transport.

## Next functional slice

Define and implement bounded incremental HTTP/2 input/output for one stream while keeping
the existing completed-request API working. Specify ownership of headers, body chunks,
response data and trailers; explicit pause/resume, window consumption, cancellation,
RST_STREAM and GOAWAY behavior; and per-stream/connection byte limits. Do not create a
second reactor or duplicate nghttp2 semantics in a consumer.

| Order | Owned paths | Exit evidence |
| --- | --- | --- |
| 1 | `src/http/ioh_http2.*`, existing H2 tests | Reviewed additive event/ownership contract and failing chunk/backpressure controls |
| 2 | H2 implementation and core integration actually used by that API | Incremental bounded processing, explicit end/trailer events, no post-cancel callback/use-after-free |
| 3 | Existing tests/examples and English transport contract | A small external C consumer receives fragmented data and trailers; slow consumer, stream reset and connection drain pass |

## Dependencies and overlap

PR #1 owns the shared image, CI/release automation, contracts, documentation indexes and
quality migration. Extend/review that work in place; do not recreate those facilities in
a parallel branch. Freeze the actually merged transport interface before iogrpc consumes
it. Keep the current HTTP/1 parser until a separate iohttpparser consumer qualification
justifies migration. liboas remains an adapter; logging remains a separately admitted
consumer boundary. ngtun is not an HTTP transport dependency.

## Publication contract

Use the existing `main` branch/PR contribution policy and container-only quality gate.
Keep callback/body lifetime regressions, two compiler lanes, required analyzers and
sanitizers with the implementation. Record exact image/tool/source identity. Reconcile
CHANGELOG and docs entries with PR #1's existing files before publication; do not replace
its versions with a second bootstrap. Full H2/gRPC support is advertised only for the
accepted transport and consumer profile; this plan establishes no throughput result.

## Execution and evidence rules

Read the target repository contract and applicable local skills before each leaf.
This independent portfolio does not gate ngtun release or require ngtun adoption.
Use an actually qualified project OCI; shared-base publication gates only its adoption.
Preserve dirty worktrees, unpublished plans, existing PRs and their path ownership.
Refresh exact default/PR identities before execution; keep compatibility explicit.
Record actual nonempty test/analyzer coverage and exact source/tool/image evidence.
Follow the repository task, changelog, review and delivery contract for each leaf.
This planning update claims no runtime success and authorizes no live mutation.
Repository quality gates remain pending until run; no host build or engine was used.

## Source baseline

- [Default README](https://github.com/ioplane/iohttp/blob/1d1cc9de367ed75c527eb4537ed01bf8ee52a75f/README.md).
- [Default repository contract](https://github.com/ioplane/iohttp/blob/1d1cc9de367ed75c527eb4537ed01bf8ee52a75f/CLAUDE.md); applicable dirty local contracts were read without importing their changes.
- [Existing PR #1](https://github.com/ioplane/iohttp/pull/1), audited head `c612021550dabecf2be3f9eafec7aa9f6c7f7cc0`; recheck the patchset before execution.
- [src/http/ioh_http2.h:37](https://github.com/ioplane/iohttp/blob/1d1cc9de367ed75c527eb4537ed01bf8ee52a75f/src/http/ioh_http2.h#L37).
- [src/http/ioh_http2.c:422](https://github.com/ioplane/iohttp/blob/1d1cc9de367ed75c527eb4537ed01bf8ee52a75f/src/http/ioh_http2.c#L422).
- [docs/plans/2026-03-10-sprints-15-19-roadmap.md:466](https://github.com/ioplane/iohttp/blob/1d1cc9de367ed75c527eb4537ed01bf8ee52a75f/docs/plans/2026-03-10-sprints-15-19-roadmap.md#L466).
