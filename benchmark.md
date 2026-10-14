# Benchmarks

## Build

```bash
scripts/build_bench.sh LeanStore_TPCC LeanStore_YCSB          # RelWithDebInfo, Ensure() on   -> build-bench/benchmark/
scripts/build_bench.sh --prod LeanStore_TPCC LeanStore_YCSB   # Release -O3, Ensure() off     -> build-bench-prod/benchmark/
```

- Never benchmark binaries from `build/`: it is configured with `ENABLE_TESTING=ON`, which forces a Debug build (`-O0 -DDEBUG`).
- `--prod` sets `-DDISABLE_ENSURE=ON`: `Ensure(e)` becomes `(void)(e)`, the check is gone but `e` is still evaluated.
- Each mode has its own build directory; never run two builds of the same directory at once.

## `scripts/bench.py`

Runs LeanStore and [tpcc-runner](https://github.com/rotaki/tpcc-runner) (cloned and built into `third_party/`). Experiments, systems and optional named perf / sanitizer profiles: `scripts/bench.yaml`. The machine state is always checked.

```bash
scripts/bench.py run --experiments tpcc --systems inpage,mvcc,tr-nowait --threads 8,16   # series -> results/<time>/: results.csv, summary.csv, *.png
scripts/bench.py run --replot results/<time>                                              # redraw plots
scripts/bench.py perf --experiments tpcc --systems inpage --threads 16 --mode record      # -> results/<time>-perf-inpage/
scripts/bench.py perf cpu-cc-8                                                            # named profile from bench.yaml
scripts/bench.py sanitize --sanitize address --experiments tpcc --systems inpage --threads 2,8 --set seconds=5 --build
scripts/bench.py sanitize asan-cc                                                         # named profile
```

Common options: `--set key=value` (any config key), `--build` (rebuild LeanStore; otherwise the existing binaries are used, a missing tpcc-runner binary is built automatically), `--machine-setup` (sudo), `--dry-run`.

- `perf --mode record`: `*.perf.txt` top functions, `*.perf.data` for `perf report -i` / hotspot; `stat`: hardware counters; `c2c`: cache-line contention. record/stat cover only the measured phase.
- `sanitize`: LeanStore only, separate build per kind (`build-bench-address/`, ...); prints `clean` or the number of reports per run.

## Storage

LeanStore opens `--db_path` with `O_DIRECT`. Default is the raw device `/dev/s0`; a plain file on an ext4 NVMe works:

```bash
truncate -s 32G /path/on/nvme/leanstore.db
```

Recreate the file before every run: the benchmarks always load from scratch.

## Run

```bash
build-bench-prod/benchmark/LeanStore_TPCC \
  --db_path=/path/on/nvme/leanstore.db --bm_virtual_gb=16 --bm_physical_gb=4 \
  --worker_count=4 --wal_enable=false \
  --txn_default_isolation_level=ser --txn_lock_inpage=true \
  --tpcc_warehouse_count=4 --tpcc_exec_seconds=30
```

| Flag | Meaning |
|---|---|
| `--txn_default_isolation_level=ser` | **Required.** The default `ru` takes no locks at all, every CC mode behaves the same |
| CC mode | MVCC: `--txn_mvcc=true`; SVCC global lock table: both flags `false` (default); SVCC per-page lock tables: `--txn_lock_inpage=true` |
| `--worker_count` | worker threads, one transaction each at a time |
| `--bm_physical_gb` / `--bm_virtual_gb` | buffer pool in RAM / address space; physical < data size forces eviction |
| `--wal_enable` | `false` to measure CC only; `true` adds logging + group commit (`--wal_fsync`, default on, fsyncs every group commit) |
| `--txn_rate` | 0 = closed loop; >0 = Poisson arrivals (txn/s), for latency |

**TPC-C:** `--tpcc_warehouse_count`, `--tpcc_warehouse_affinity` (worker ↔ warehouse, fewer conflicts), `--tpcc_exec_seconds`.

**YCSB:** one operation per transaction, read or update.

| Workload | Flags |
|---|---|
| A | `--ycsb_read_ratio=50 --ycsb_zipf_theta=0.99` |
| B | `--ycsb_read_ratio=95 --ycsb_zipf_theta=0.99` |
| C | `--ycsb_read_ratio=100 --ycsb_zipf_theta=0.99` |

Plus `--ycsb_record_count`, `--ycsb_payload_size`, `--ycsb_exec_seconds`. Defaults (`read_ratio=100`, `theta=0`) are uniform YCSB-C: no conflicts.

## Output

One CSV line per second on stdout, then the totals:

```
ts,committed_tx,aborted_tx,precommited_tx,commit_rounds,bm_rmb,bm_wmb,bm_evict,log_sz_mb,logio_mb,log_flush_cnt,...
```

- `precommited_tx` = committed + aborted that finished execution; `committed_tx`, `aborted_tx` are exact with and without WAL.
- Drop the first seconds (warm-up) when averaging.

## Known issues

- **YCSB does not catch `AbortTransaction`**: under `ser` it crashes on the first conflict. Use TPC-C until fixed.
- **SVCC with the global lock table crashes on TPC-C under `ser`** (heap corruption, already on HEAD).
- TPC-C runs with `manually_handle_isolation_anomalies=true`: it silently skips anomalies meant for `ru`, so it is no correctness check.
- No correctness check after a run (TPC-C consistency conditions are not implemented).

## Machine setup (this laptop: i7-1360P)

Topology: CPUs 0–7 = 4 P-cores × 2 hyper-threads (pairs 0/1, 2/3, 4/5, 6/7), CPUs 8–15 = E-cores. Check with `lscpu -e=CPU,CORE,MAXMHZ`.

Before a series of runs:

```bash
# AC power, close browsers/IDE indexers/Seafile sync
sudo cpupower frequency-set -g performance                                   # governor (default: powersave)
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo                # no turbo: stable clocks, no thermal drift
echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid                       # perf counters ("Error opening counter cycle")
echo madvise | sudo tee /sys/kernel/mm/transparent_hugepage/enabled           # LeanStore madvise()s huge pages itself
```

Undo afterwards: `cpupower frequency-set -g powersave`, `no_turbo` back to `0`, `perf_event_paranoid` back to `4`.

Pinning:

- `--worker_pin_thread=true` pins worker *t* to CPU *t*. On this CPU workers 0 and 1 share P-core 0, workers 2 and 3 share P-core 1, …; workers ≥ 8 land on E-cores.
- So: up to 8 workers stay on P-cores but share cores pairwise.
- For one worker per physical P-core: run without `--worker_pin_thread` under `taskset -c 0,2,4,6` (≤ 4 workers). Disabling SMT (`/sys/devices/system/cpu/smt/control`) does not renumber CPUs, it takes 1, 3, 5, 7 offline, so `--worker_pin_thread` would then pin to offline CPUs.
- Background threads (page providers, group commit, stats) are not pinned.

Per run: fresh DB file, ≥ 3 repetitions, report median; keep data on the internal NVMe, not on a synced folder.
