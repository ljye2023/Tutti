# Layer 2 NVMe real-hardware test

Exercises the **real** NVMe device-manager path end-to-end against a physical
controller (default `0000:b1:00.0`), as opposed to the vendor-neutral mock
suite in `tutti/tests/device_manager/`.

```
daemon (B3 owner: chrdev_create + kernel_ioq_cap + SNVM_DEVICE_BIND + probe)
  -> gRPC Connect  -> libnvm nvm_ctrl_attach_client
  -> nvm_create_group -> nvm_add_user_queue -> GPU-memory Write/Read/verify
  -> nvm_destroy_group -> nvm_ctrl_free_client -> Disconnect
```

`nvme_real_hw_test` is a GoogleTest that **shells out** to the prebuilt
`nvmeservice_client` example binary (the only in-tree driver of the full
libnvm + GPU IO path). The test links only GoogleTest — no libnvm/CUDA/gRPC.

`daemon_driver_test` drives the low-level `DaemonNvmeDeviceDriver` directly
(unit tier in mock-grant mode; `DaemonDriverRealHw.*` against a live daemon).

`device_manager_real_hw_test` exercises the **upper-layer** path a real
consumer (e.g. `tutti/backends/nvme`) uses, through the `IDeviceManager`
facade rather than the driver:

```
create_device_manager({DaemonNvmeDeviceDriver}) -> IDeviceManager
  dm->Open()
  IVirtualDevice* v = dm->open_vdevice(phys_id, quota, &err)
  assert v->type() == LOCAL_NVME; auto* nvme = static_cast<NvmeVirtualDevice*>(v)
  read nvme->{d_qps, queue_quota, namespace_id, blk_size, blk_size_log, max_data_size}
  dm->close_vdevice(v)  ->  dm->Close()
```

`DeviceManagerFacadeUnit.*` runs anywhere (mock-grant mode, no daemon);
`DeviceManagerFacadeRealHw.*` gates on `TUTTI_NVME_REAL_HW=1` + a live daemon
and asserts a downcast `NvmeVirtualDevice` carries live GPU queues (`d_qps`
non-null) and populated namespace metadata.

The old monolithic driver script has been split into single-responsibility
scripts (build, module reload, daemon start, gtest run, daemon stop). See
"Scripts" and "Typical flow" below for how they chain together.

## Scripts

| Script | Job | Root? |
|--------|-----|:---:|
| `tutti/build.sh` | Configure + build the whole project (business + test binaries) into `tutti/build` | no |
| `build_snvme_module.sh` | Build the snvme kernel module from repo source into `tutti/build_snvme_module` | no |
| `reload_snvme_module.sh` | Fresh-reload the snvme module (`rmmod`+`insmod`), with safety gates | yes |
| `start_nvmeservice_daemon.sh` | Privileged bring-up: preflight, clear `driver_override`, start daemon, wait for ready | yes |
| `run_real_hw_test.sh` | Run the `nvme_real_hw_test` gtest against the already-running daemon | no |
| `stop_nvmeservice_daemon.sh` | Stop the daemon (via pidfile) and restore stock `nvme` | yes |

## Typical flow

```bash
cd <repo>/tutti && ./build.sh                                    # 1. build all binaries -> tutti/build
tests/device_manager/nvme/build_snvme_module.sh                  # 2. build snvme module  -> tutti/build_snvme_module
tests/device_manager/nvme/reload_snvme_module.sh                 # 3. fresh-load the module
tests/device_manager/nvme/start_nvmeservice_daemon.sh            # 4. bring the daemon up (owns the device)
tests/device_manager/nvme/run_real_hw_test.sh                    # 5. run the gtest against the running daemon
tests/device_manager/nvme/stop_nvmeservice_daemon.sh             # 6. stop daemon + restore stock nvme
```

(Paths above are relative to `<repo>/tutti`. Every script is standalone and
locates the repo root itself, so you can also run each with its full path from
anywhere.)

## Tiers

| Test case | What it does | Destructive? | GPU? |
|-----------|--------------|:---:|:---:|
| `ListDevicesReportsTargetPci`      | daemon lists the owned controller; asserts `pci=<target>` | no | no |
| `ConnectAttachSkipIoLifecycle`     | gRPC Connect + libnvm attach/create/destroy (`--skip-io`) | no | no |
| `GpuIoWriteReadVerifyDestructive`  | GPU-memory write/read/verify, 4 blocks at LBA 2621440 | **yes** | yes |

## Opt-in gates (safe by default)

Every case `GTEST_SKIP()`s unless the operator opts in, so the target is safe
to configure/build/`ctest` on hardware-less nodes.

| Env var | Effect |
|---------|--------|
| `TUTTI_NVME_REAL_HW=1`          | required for **any** tier to run |
| `TUTTI_NVME_ALLOW_DESTRUCTIVE=1`| required for the GPU-IO (write) tier |
| `TUTTI_NVME_CLIENT_BIN`         | override client path (default: build-time) |
| `TUTTI_NVME_ENDPOINT`           | gRPC endpoint (default `127.0.0.1:50051`) |
| `TUTTI_NVME_DEVICE_ID`          | daemon device_id (default `0`) |
| `TUTTI_NVME_CUDA`               | CUDA device id (default `0`) |
| `TUTTI_NVME_PCI`                | expected PCI addr (default `0000:b1:00.0`) |

`run_real_hw_test.sh` sets `TUTTI_NVME_REAL_HW`, `TUTTI_NVME_ENDPOINT`,
`TUTTI_NVME_PCI` and `TUTTI_NVME_CLIENT_BIN`, and — unless `--no-destructive`
is passed — `TUTTI_NVME_ALLOW_DESTRUCTIVE=1`.

## Prerequisites (this host)

- **GPU arch**: the host GPU is an **L40S (sm_89)**. Binaries must be built
  with CUDA arch 89 (the `build.sh` default) or the GPU-IO tier fails with
  `no kernel image is available` (the test diagnoses this explicitly).
- **snvme kernel module** loaded (`/dev/snvm_control` present).
- The daemon needs **root** (PCI ownership + mount) — handled by
  `start_nvmeservice_daemon.sh`.

## Build

### Userspace (business + test binaries, unified)

```bash
cd <repo>/tutti
./build.sh                    # everything (daemon, client, gtest, ...) -> tutti/build
./build.sh --reconfigure       # force a fresh configure (needed if an old cache lacks the vcpkg toolchain)
```

Options: `--reconfigure`, `--build-type <t>` (default `RelWithDebInfo`),
`--cuda-arch <n>` (default `89`, matching the L40S / sm_89 host GPU), `-j <N>`.

### snvme kernel module (from repo source)

`build_snvme_module.sh` builds the module **from the repo baseline**
(`tutti/device_manager/nvme/kernel_modules/snvme-5.15.0-public`) into
`<repo>/tutti/build_snvme_module/` by default.

```bash
cd <repo>/tutti/tests/device_manager/nvme
./build_snvme_module.sh            # -> <repo>/tutti/build_snvme_module/{snvme-core,snvme}.ko
```

`reload_snvme_module.sh` loads these repo-built `.ko`s by default
(`--ko-dir <repo>/tutti/build_snvme_module`).

## Running the pieces

### 1. Reload the module

```bash
./reload_snvme_module.sh
./reload_snvme_module.sh --ko-dir /path/to/build_snvme_module --io-queue-depth 32
```

Options: `--ko-dir` (default `<repo>/tutti/build_snvme_module`), `--passwd-file`
(default `~/.passwd/1`; empty string forces interactive sudo),
`--io-queue-depth` (default `64`).

Refuses to reload if a device is still bound to `snvme`, has fd holders on
`/dev/snvm*`, or a `snvme` block device is mounted — a botched reload can
wedge the host.

### 2. Start the daemon

```bash
./start_nvmeservice_daemon.sh
./start_nvmeservice_daemon.sh --reload-module   # also reload the module first
```

Options: `--build-dir` (default `<repo>/tutti/build`), `--config` (default
`<build-dir>/bin/sys_config.b1.yaml`; this is the **daemon's** config — the
client takes no config, it connects over gRPC), `--passwd-file`, `--pci`
(default `0000:b1:00.0`), `--reload-module` (delegates to
`reload_snvme_module.sh`).

Does preflight, clears `driver_override` on the target PCI device, ensures
it starts bound to stock `nvme`, starts the daemon, and waits for the
"NVMeService daemon listening" marker in its log before leaving it running.

### 3. Run the gtest

```bash
./run_real_hw_test.sh
./run_real_hw_test.sh --no-destructive
./run_real_hw_test.sh --pci 0000:b1:00.0 --endpoint 127.0.0.1:50051
```

Options (only these — `run_real_hw_test.sh` no longer manages the module or
daemon): `--build-dir` (default `<repo>/tutti/build`), `--pci` (default
`0000:b1:00.0`), `--endpoint` (default `127.0.0.1:50051`),
`--no-destructive` (skip Tier 2).

This script does **one job**: run the `nvme_real_hw_test` binary against an
already-running daemon. It does not reload the module, start/stop the
daemon, or touch `driver_override`.

### 4. Stop the daemon

```bash
./stop_nvmeservice_daemon.sh
./stop_nvmeservice_daemon.sh --pci 0000:b1:00.0 --passwd-file ~/.passwd/1
```

Options: `--passwd-file`, `--pci`. Needs no build dir — it stops the daemon
via its fixed pidfile (`/tmp/nvmeservice_daemon_b1.pid`) with `SIGINT`, then
unbinds from `snvme` and rebinds stock `nvme`.

## Kernel module bring-up gotchas (both cost real debugging time)

Two host-state conditions make `SNVM_DEVICE_BIND` fail with `errno=19`
(`nvm_controller_init_b3 failed`) even though the module is loaded and the
device is present:

1. **`driver_override` pin.** If `/sys/bus/pci/devices/<bdf>/driver_override`
   is set to `nvme`, the PCI core rejects *every* other driver (including
   `snvme`) in `driver_match_device` — `nvme_probe` is never even called, so
   there is no probe log, just an immediate `-ENODEV`.
   `start_nvmeservice_daemon.sh` clears it automatically
   (`echo > /sys/bus/pci/devices/<bdf>/driver_override`).
2. **Stale module registration.** `register_driver()` runs the real
   `pci_register_driver()` (whose auto-probe binds the controller) only once
   per module load, guarded by `snvm_registered`. After the module has served
   any bind — success or failure — every later bind falls through to
   `device_driver_attach()` and returns `-ENODEV`. Fix: reload the module fresh
   (`rmmod snvme snvme_core; insmod ...`) before each daemon bring-up.
   `reload_snvme_module.sh` does this (and refuses if snvme still owns a
   device, has fd holders, or has a mounted block dev — a botched reload can
   wedge the host).

## Run via ctest (daemon must already be up)

```bash
# bring the daemon up first (reload_snvme_module.sh + start_nvmeservice_daemon.sh):
TUTTI_NVME_REAL_HW=1 TUTTI_NVME_ALLOW_DESTRUCTIVE=1 \
  ctest --test-dir <repo>/tutti/build -L real_hw --output-on-failure
```

## Config

`sys_config.b1.yaml` targets `0000:b1:00.0`, `kernel_ioq_cap: 16`,
`allowed_gpus: [0]`, mounts `/mnt/nvme0`, GPU view `/mnt/gpu0`. It is staged
next to the binaries at build time (`tutti/build/bin/sys_config.b1.yaml`).
Point it at a different device by editing `pci_addr` (or copy + `--config`
to `start_nvmeservice_daemon.sh`).

## Safety notes

- The daemon takes **exclusive ownership** of the target device (unbinds it
  from stock `nvme`); `/dev/nvme1n1` disappears while the daemon owns it.
- The destructive tier **writes** to the namespace at LBA 2621440 (10 GiB
  offset). Only run against a scratch device that is safe to wipe.
- `stop_nvmeservice_daemon.sh` restores the device to the stock `nvme`
  driver.
