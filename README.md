# `prun` — High-Performance Parallel Task Dispatcher

`prun` is a lightweight, bounded-concurrency process dispatcher and task execution engine written in C. It enables multi-core batch execution of independent processes with dynamic workload balancing, monotonic latency profiling, and POSIX exit status tracking—similar to the core execution engines of `make -j` and `xargs -P`.

---

## 1. System Architecture & Design

### High-Level Architecture
```
                           +------------------------+
                           |   Task Queue / Input   |
                           |    (task_t array)      |
                           +-----------+------------+
                                       |
                                       v
               +------------------------------------------------+
               |            Parent Dispatcher Engine            |
               |                                                |
               |   - Bounded concurrency limit: max_workers     |
               |   - Active worker counter:    active_workers   |
               |   - Task cursor / queue index: next_task_idx   |
               |   - Monotonic timer start/end tracking         |
               +-----------------------+------------------------+
                                       |
              +------------------------+------------------------+
              | fork() & execvp()                               | waitpid(-1, &status, 0)
              v                                                 ^
    +-------------------+                             +-------------------+
    |   Worker Slot 1   |                             |   Kernel Event    |
    |  Child (PID 1001) |                             |    Notification   |
    +-------------------+                             +-------------------+
    |   Worker Slot 2   |                                       |
    |  Child (PID 1002) | --------------------------------------+ (Child exits)
    +-------------------+
    |   Worker Slot K   |
    |  Child (PID 100K) |
    +-------------------+
```

### Core Design Decisions

#### 1. Dynamic Event-Driven Reaping vs. Polling
* **Decision**: Use blocking `waitpid(-1, &status, 0)` instead of non-blocking polling (`WNOHANG`) or specific PID waiting (`waitpid(pid, ...)`).
* **Rationale**:
  - Waiting for specific PIDs sequentially causes artificial head-of-line blocking if a prior task runs longer than subsequent tasks.
  - Using `-1` makes the parent event-driven: whichever child finishes first wakes up the dispatcher immediately.
  - The parent sleeps inside the kernel during execution, consuming **0% CPU** while child workers utilize 100% of CPU cores.

#### 2. Process Isolation & Clean Transformation
* **Decision**: Fork-Exec pattern with immediate `_exit(127)` fallback.
* **Rationale**:
  - Each task executes in an isolated virtual address space. Memory faults (e.g. segfaults) in one task cannot corrupt other running tasks or the dispatcher.
  - If `execvp()` fails (e.g., missing binary or permission denied), the child process must immediately call `_exit(127)` to prevent running arbitrary dispatcher code.

#### 3. Bounded Concurrency & Backpressure
* **Decision**: Strict $K$-worker ceiling.
* **Rationale**:
  - Unbounded process creation leads to thread/process exhaustion (`EAGAIN`), severe TLB thrashing, and out-of-memory kernel panics (OOM killer).
  - Bounding active children to `max_workers` matches available physical CPU cores, maximizing throughput while keeping memory footprint constant.

#### 4. Quote-Aware Shell Tokenization
* **Decision**: In-place state-machine tokenizer supporting single (`'...'`) and double (`"..."`) quotation.
* **Rationale**:
  - Enables complex commands with spaces (e.g. `python3 -c "..."` or `sh -c "..."`) without requiring an intermediate shell wrapper unless explicitly invoked.

---

## 2. Process Lifecycle & State Machine

Each task transitions through the following lifecycle:

```
[QUEUED] 
   │
   │ fork() & execvp()
   ▼
[RUNNING] ──────── (Kernel executes child process)
   │
   │ exit() / signal termination
   ▼
[ZOMBIE] ───────── (Kernel holds exit status in process table)
   │
   │ waitpid(-1, &status, 0)
   ▼
[REAPED] ───────── (Parent extracts exit_status, records elapsed_ms, decrements active_workers)
```

---

## 3. Performance & Speedup Model

### Theoretical Model
According to Amdahl's Law, for a workload consisting of $N$ perfectly parallelizable independent tasks with runtimes $t_1, t_2, \dots, t_N$, the speedup $S$ achieved across $K$ workers is:

$$S = \frac{\sum_{i=1}^{N} t_i}{T_{\text{wall}}}$$

Where:
* $\sum t_i$ is the sequential execution duration.
* $T_{\text{wall}} = \max_{k \in K} (\text{Runtime of worker slot } k) + T_{\text{overhead}}$.

### Monotonic High-Resolution Profiling
`prun` utilizes `clock_gettime(CLOCK_MONOTONIC, &ts)` rather than `gettimeofday()` or `time()`:
* **Immune to NTP Jumps**: Does not skew if the system clock adjustments or daylight savings occur during execution.
* **Nanosecond Resolution**: Sub-millisecond timing accuracy for microbenchmarks.

---

## 4. API Specification

Inspect [`prun.h`](file:///home/suraj/assign/01-parallel-runner/prun.h):

```c
typedef struct {
    char *cmd_line;         // Full command string
    char **argv;            // NULL-terminated argument vector for execvp
    pid_t pid;              // OS Process ID
    struct timespec start;  // Monotonic start timestamp
    long long elapsed_ms;   // Individual execution duration
    int exit_status;        // Normalized exit code
    bool finished;          // Completion state
    bool failed;            // Failure state (exit != 0 or signaled)
} task_t;

typedef struct {
    int total_tasks;
    int succeeded_tasks;
    int failed_tasks;
    long long wall_clock_ms;
    double speedup;         // Effective parallel acceleration factor
} run_summary_t;

int run_parallel(task_t *tasks, int num_tasks, int max_workers, run_summary_t *summary, bool verbose);
```

---

## 5. CLI Usage & Demonstration

### Build
```bash
make
```

### Run Batch Tasks
```bash
./prun -j <workers> <task_file>
```

#### Example:
```bash
./prun -j 4 sample_tasks.txt
```

#### Output:
```text
============================================================
  prun: Dispatching 8 tasks with 4 concurrent workers
============================================================

  [START] [PID 241332] python3 -c "sum(i*i for i in range(20000000))" (Slot 1/4)
  [START] [PID 241333] sh -c "head -c 50M /dev/zero | gzip -9 > /dev/null" (Slot 2/4)
  [START] [PID 241334] python3 -c "import hashlib; [hashlib.sha256(b'stanford-cs111').hexdigest() for _ in range(800000)]" (Slot 3/4)
  [START] [PID 241335] sh -c "seq 1 3000000 | sort -n > /dev/null" (Slot 4/4)
  [DONE ] [PID 241333] "sh -c "head -c 50M /dev/zero | gzip -9 > /dev/null"" in 68ms (exit 0)
  [START] [PID 241340] sleep 1.5 (Slot 4/4)
  ...

============================================================
  Execution Summary
============================================================
  Total Tasks:    8
  Succeeded:      8
  Failed:         0
  Wall Clock:     1571 ms
  Est. Speedup:   3.10x
============================================================
```

---

## 6. Verification & Quality Assurance

The test suite validates concurrency limits, speedup factors, non-zero exit propagation, and memory leak freedom:

```bash
# Unit & integration regression tests
make test

# Memory leak & buffer safety verification via AddressSanitizer
make test_asan
```
