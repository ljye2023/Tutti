# tests/CMakeLists.txt -- Legacy smoke tests build configuration.
#
# Round 13 Session 2: legacy layer3_* tests removed (they referenced
# deleted trees via accel/ + tutti_backends_nvme targets).  layer1_smoke_test
# has a pre-existing link failure (links tutti_accel which is a standalone
# target not available in the root build); per S1 decision, it is commented
# out and recorded as a gap.  The root build's real tests live in
# tutti/tests/ (standalone) and tests/ (hardware contract tests).

# ---------------------------------------------------------------------------
# Legacy smoke tests — REMOVED in Round 13 S2
# ---------------------------------------------------------------------------
# layer1_smoke_test:  pre-existing link failure (tutti_accel not in root build)
# layer3_smoke_test:  references deleted accel/ paths
# layer3_integration_test: same
# layer3_integration_simple: same
# All four targets removed; their .cu source files remain on disk for
# reference but are not compiled.  S3 will decide their fate.
# ---------------------------------------------------------------------------

message(STATUS "tests/ (legacy): all targets removed in Round 13 S2")
