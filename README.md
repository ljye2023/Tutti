# Tutti
`Tutti` (Italian for "all instruments together") is a `CPU/GPU companion
unified storage runtime`: applications face a single stable set of memory,
target, and asynchronous IO handles, while file resolution, GPU vendor
capabilities, local NVMe, GDS, RDMA, and other data-movement implementations
live behind replaceable internal boundaries.

## Documentation

- [`TUTTI_TARGET_ARCHITECTURE.md`](doc/TUTTI_TARGET_ARCHITECTURE.md) — normative target architecture and interface contracts.
- [`TUTTI_REFACTOR_TAKEOVER.md`](doc/TUTTI_REFACTOR_TAKEOVER.md) — current implementation, known problems, and migration order.
- [`Roadmap.md`](Roadmap.md) — active version snapshot, current status, and implementation phases.
- [`doc/build_and_test.md`](doc/build_and_test.md) — build instructions and SNVMe driver testing.

Historical roadmap snapshots and superseded designs are archived under [`doc/history/`](doc/history/).
