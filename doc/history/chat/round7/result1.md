# T-023 Worker Result

## 1. Environment Readiness Self-check

```bash
ps -eo pid,etime,cmd | grep -E '[c]make|[c]test|[t]utti_daemon' | head
```
Result: one daemon process running (pid 3386944, `./build/bin/tutti_daemon --config sys_config.yaml`, 43 min uptime). This is the environment owner's daemon, not another session's build/test. No concurrent cmake/ctest.

```bash
findmnt /mnt/nvme1
```
```
TARGET     SOURCE        FSTYPE OPTIONS
/mnt/nvme1 /dev/snvme0n1 ext4   rw,relatime
```

```bash
test -w /mnt/nvme1/GPU0/resolver_test && echo 'writable: OK'
```
```
writable: OK
```

Environment ready. Proceeded.

## 2. Fix Method: How Multi-extent File Is Created

The old test 14 created a 4 MiB file which on fresh ext4 always has exactly 1 extent. With `exts_per_call=2`, the first FIEMAP call returns that 1 extent with `FIEMAP_EXTENT_LAST`, so the multi-round loop body executes only once. The multi-round concatenation path was never exercised.

**Fix**: Use the allocate-extend trick to force 2 physical extents:

1. `fallocate` file A to 4 MiB
2. `fallocate` file B to 4 MiB (ext4 places B adjacent to A)
3. `fallocate` A from 4 MiB to 8 MiB (extend at offset 4 MiB) — ext4 cannot extend A in place because B occupies the adjacent blocks, so A's second half is allocated elsewhere → 2 physical extents
4. `pwrite` full 8 MiB to A and 4 MiB to B, `fsync` both
5. Resolve A with `exts_per_call=1` (each extent needs a separate ioctl round → ≥2 rounds guaranteed)
6. Resolve A with default `exts_per_call=256`
7. Assert extent sets identical and count ≥2

**Why `exts_per_call=1` instead of `=2`**: With 2 extents and `exts_per_call=1`, each round gets exactly 1 extent, forcing exactly 2 rounds. With `exts_per_call=2`, both extents fit in one round, which defeats the purpose. `=1` is the strongest guarantee.

## 3. Precise Changes to test 14

**What changed**: Only the `test_multi_round` function body (test 14). The rest of the file is unchanged.

**Key changes**:
- Old: single 4 MiB file, `exts_per_call=2` vs default
- New: two files (A=8 MiB with 2 extents, B=4 MiB placeholder), `exts_per_call=1` vs default
- Added prerequisite assertion: if A has <2 extents, test fails explicitly with a message explaining multi-round was not triggered
- Added `filefrag -v` cross-validation output
- Added per-extent triple (logical_offset, device_offset, length) printing for both resolvers
- Cleanup both A and B

**Why this triggers real multi-round**: With 2 extents and `exts_per_call=1`, the resolver's FIEMAP loop must execute ≥2 iterations: first returns extent 0 (without `FIEMAP_EXTENT_LAST`), cursor advances to `logical_offset + length`, second returns extent 1 (with `FIEMAP_EXTENT_LAST`). The multi-round concatenation path is genuinely exercised.

## 4. Test 14 Complete Output

```
  default-buf extents: 2
  default [0] logical=0 device=146800640 length=4194304
  default [1] logical=4194304 device=155189248 length=4194304
  small-buf extents: 2
  small  [0] logical=0 device=146800640 length=4194304
  small  [1] logical=4194304 device=155189248 length=4194304
  --- filefrag -v /mnt/nvme1/GPU0/resolver_test/test_multiround_a.bin ---
Filesystem type is: ef53
File size of /mnt/nvme1/GPU0/resolver_test/test_multiround_a.bin is 8388608 (2048 blocks of 4096 bytes)
 ext:     logical_offset:        physical_offset: length:   expected: flags:
   0:        0..    1023:      35840..     36863:   1024:
   1:     1024..    2047:      37888..     38911:   1024:      36864: last,eof
/mnt/nvme1/GPU0/resolver_test/test_multiround_a.bin: 2 extents found
  --- end ---
[PASS] multi-round FIEMAP (exts_per_call=1 vs default)
```

**Multi-round evidence**:
- Both resolvers report **2 extents** (≥2, prerequisite met)
- Logical offsets are **continuous**: 0 → 4194304 (= 0 + 4194304), no hole
- Both extent sets are **identical** (same logical_offset, device_offset, length)
- `filefrag -v` confirms 2 extents with a gap in physical offsets (35840..36863 vs 37888..38911, with 36864..37887 occupied by B)

## 5. All 14 Tests Pass — Complete Output

```
Test directory: /mnt/nvme1/GPU0/resolver_test
Block size: 4096
PCI: 0000:08:00.0, NSID: 1

  extents: 1
  [0] logical=0 device=142606336 length=4194304
[PASS] normal_path (fallocate+write+fsync)
  fallocate-only extents: 1, file_size=2097152
[PASS] fallocate_only (UNWRITTEN accepted)
[PASS] view_payload round-trip
  map test: extents=1, first_byte_ok=1
[PASS] map_to_device_offset
  --- filefrag -v output ---
  ...
[PASS] filefrag cross-validation
  resolve failed as expected: code=10, msg=first extent does not start at offset 0
[PASS] sparse_file (hole -> failure expected)
[PASS] scheme mismatch -> UNSUPPORTED
[PASS] file not found -> NOT_FOUND
[PASS] malformed uri -> INVALID_ARGUMENT
[PASS] block_size == 0 -> INVALID_ARGUMENT
  code=1 msg=fs block size 4096 not multiple of block_size 1048576
[PASS] alignment check (1 MiB block_size)
  payload accessible during RT lifetime: OK
  second resolve after first RT destroyed: OK
[PASS] fd lease lifetime
[PASS] lease move safety (no double close)
  default-buf extents: 2
  ...
[PASS] multi-round FIEMAP (exts_per_call=1 vs default)

14/14 tests passed.
```

ctest result: `1/1 Test #1: tutti_resolver_contract_test .....   Passed    0.04 sec`

## 6. Other 13 Tests Unaffected

Only `test_multi_round` was modified. The other 13 test functions (`test_normal_path` through `test_lease_move`) were not touched. Their outputs are identical to before the change:
- test 1 (normal_path): 1 extent, PASS
- test 2 (fallocate_only): 1 extent, PASS
- test 3 (view_payload): PASS
- test 4 (map_to_device): 1 extent, PASS
- test 5 (filefrag): 1 extent, PASS
- test 6 (sparse_file): fails as expected, PASS
- test 7-10: PASS (URI/scheme/block_size validation)
- test 11 (alignment): fails as expected, PASS
- test 12 (fd lease): PASS
- test 13 (lease move): PASS

## 7. Hygiene Checks

```bash
git diff --check -- tests/resolver_contract/resolver_contract_test.cpp
```
Exit code: 0 (no whitespace errors).

```bash
git diff --stat -- tests/resolver_contract/resolver_contract_test.cpp
```
Shows only `tests/resolver_contract/resolver_contract_test.cpp` modified.

Trailing whitespace: 0 lines.
EOF newline: OK.

git status confirms only `tests/resolver_contract/resolver_contract_test.cpp` was modified by this task (other modified files are from previous sessions).

## 8. Environment Unchanged Verification

```bash
findmnt /mnt/nvme1 | tail -1
```
```
/mnt/nvme1 /dev/snvme0n1 ext4   rw,relatime
```

```bash
pgrep -af tutti_daemon | head -1
```
```
3386944 ./build/bin/tutti_daemon --config sys_config.yaml
```
(Same daemon as baseline, still running.)

```bash
grep -E '^(snvme|snvme_core|phoenixfs) ' /proc/modules
```
```
snvme 73728 7 - Live 0xffffffffa08c2000 (O)
snvme_core 77824 2 snvme, Live 0xffffffffa0792000 (O)
phoenixfs 81920 2 - Live 0xffffffffa07ad000 (O)
```
(Module state unchanged — same sizes, addresses. Refcount on snvme is 7, same as baseline.)

```bash
findmnt /mnt/nvme4 | tail -1
```
```
/mnt/nvme4 /dev/md0 xfs   rw,noatime,attr2,inode64,logbufs=8,logbsize=32k,sunit=1024,swidth=4096,noquota
```
(Production RAID intact.)

No sudo, insmod, rmmod, mount, umount, bind, unbind, or block IO operations were performed.

## 9. Final Status

**PASS**

All 7 success criteria met:
1. test 14 uses multi-extent file (2 extents) + `exts_per_call=1` — genuinely multi-round
2. Prerequisite assertion: if file has <2 extents, test fails explicitly
3. Hard evidence in output: 2 extents, continuous logical offsets, real triples, filefrag cross-validation
4. Other 13 tests unchanged, all 14 pass
5. Only `resolver_contract_test.cpp` modified (test 14 only)
6. No prohibited operations, environment unchanged
7. Whitespace and EOF newline checks pass

## 总指挥验收

验收结论：`PASS`。上一轮判定的「空洞验证」已真实闭合。

独立核验结果：

- **多轮真正发生了。** 输出显示 default 与 small（`exts_per_call=1`）两侧均为 **2 个 extent**，且 `filefrag -v` 独立确认 2 个 extent、物理偏移被占位文件 B 隔开（35840..36863 vs 37888..38911，中间 36864..37887 是 B）。`exts_per_call=1` 时每个 extent 需一轮 ioctl，第二次调用与游标推进后的拼接路径**确实被执行**——这正是上一轮缺失的验证。
- **前提断言生效。** worker 明确：若 A 文件 < 2 extent，测试显式失败而非静默通过。这把「ext4 顺序分配是启发式而非契约」这一不确定性变成了显式失败，而非隐藏假阳性。
- **logical 连续无空洞**：`0 → 4194304 → (0+4194304)`，能通过 binding 的 `validate()`，与「完整覆盖 `[0, file_size)`」一致。
- **我独立重跑 CTest：`1/1 Passed`（14/14）。** 其余 13 个测试输出与改动前一致（我只改了 test 14 一处，这一点 worker 声明与我抽核一致）。
- **环境未被改动**：`/mnt/nvme1` 仍挂载、daemon 仍运行（同一 pid 3386944）、模块状态与基线一致、`/mnt/nvme4` 生产 RAID 完好。
- 交付文件尾随空白与 EOF newline 均 OK。
- worker 对「为什么用 `exts_per_call=1` 而非 `=2`」的解释正确：2 个 extent 在 `=2` 时会装进同一轮，达不到强制多轮的目的；`=1` 才是最强保证。

未发现需返工或记录的非阻塞项。

后续决定：T-023 完成，Round 6 Session 3 的 `REQUIRED FOLLOW-UP` 至此清偿。
