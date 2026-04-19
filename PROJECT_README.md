# Multi-Container Runtime with Kernel Memory Monitor

This project implements a lightweight Linux container runtime in C. It includes a long-running supervisor process, a command-line client, multi-container process management, per-container logging, test workloads, and a Linux kernel module interface for memory monitoring.

The project was built as part of an Operating Systems assignment focused on Linux namespaces, IPC, concurrency, process lifecycle management, kernel modules, and scheduler experiments.

## Project Overview

The runtime is centered around one user-space binary, `engine`, and one kernel module, `monitor.ko`.

The `engine` program works in two modes:

- `supervisor`: starts a long-running parent process that manages containers.
- CLI client: sends commands such as `start`, `run`, `ps`, `logs`, and `stop` to the supervisor.

The supervisor manages multiple containers at the same time. Each container is launched using Linux namespaces and runs inside its own root filesystem copy. The supervisor keeps metadata for every container, captures stdout/stderr through pipes, writes logs through a bounded-buffer logging system, and handles container lifecycle events.

The kernel module is intended to track registered container PIDs and enforce memory limits through soft-limit warnings and hard-limit kills.

## Repository Contents

| File / Directory | Purpose |
| --- | --- |
| `engine.c` | Main user-space runtime, supervisor, CLI client, container launcher, logging system, and control-plane IPC |
| `monitor.c` | Linux kernel module source for container memory monitoring |
| `monitor_ioctl.h` | Shared ioctl definitions used by `engine.c` and `monitor.c` |
| `Makefile` | Builds user-space binaries and the kernel module |
| `cpu_hog.c` | CPU-bound workload for scheduler experiments |
| `memory_hog.c` | Memory pressure workload for soft/hard memory-limit testing |
| `io_pulse.c` | I/O-oriented workload for scheduler experiments |
| `environment-check.sh` | VM and kernel-module environment preflight script |
| `project-guide.md` | Original project specification |
| `boilerplate/` | Starter/reference project files, generated rootfs copies, logs, and test artifacts |
| `logs/` | Runtime log directory for containers launched from the repo root |

## Implemented Features

### 1. Supervisor and CLI Architecture

The project implements a long-running supervisor model.

The supervisor is started with:

```bash
./engine supervisor <base-rootfs>
```

Client commands connect to the supervisor through a UNIX domain socket:

```text
/tmp/mini_runtime.sock
```

The supervisor accepts requests from short-lived CLI processes, handles container operations, sends responses, and remains alive while containers continue running.

Implemented commands:

```bash
./engine supervisor <base-rootfs>
./engine start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
./engine run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
./engine ps
./engine logs <id>
./engine stop <id>
```

### 2. Multi-Container Runtime

Containers are launched with `clone()` using Linux namespaces:

- `CLONE_NEWPID` for PID namespace isolation
- `CLONE_NEWUTS` for hostname/UTS isolation
- `CLONE_NEWNS` for mount namespace isolation
- `SIGCHLD` so the supervisor can reap container children

Each container is tracked with metadata including:

- Container ID
- Host PID
- Start time
- Current state
- Soft memory limit
- Hard memory limit
- Exit code
- Exit signal
- Log path
- Stop-request flag

Supported container states include:

- `starting`
- `running`
- `stopped`
- `killed`
- `exited`
- `hard_limit_killed`

The runtime supports multiple concurrently running containers as long as each container uses its own writable root filesystem copy.

### 3. Root Filesystem Isolation

The runtime expects an Alpine mini root filesystem to be prepared before launching containers.

Example setup:

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

Each live container should use a different rootfs:

```bash
./engine start alpha ./rootfs-alpha "/bin/sh"
./engine start beta ./rootfs-beta "/bin/sh"
```

The child process uses `chroot()` to enter the selected container rootfs.

### 4. Control-Plane IPC

The supervisor and CLI communicate using a UNIX domain socket.

The control socket path is:

```text
/tmp/mini_runtime.sock
```

This satisfies the control-plane IPC requirement separately from the logging pipeline. CLI commands build a `control_request_t`, send it to the supervisor, and receive a response.

### 5. Bounded-Buffer Logging Pipeline

The project implements a producer-consumer logging pipeline.

Container stdout and stderr are redirected into pipes. A producer thread reads from each container pipe and inserts log chunks into a bounded shared buffer. A consumer thread removes log items from the buffer and appends them to per-container log files.

Logging design:

- Container output is captured through file-descriptor-based IPC.
- `stdout` and `stderr` are merged into the same pipe.
- A bounded buffer stores log chunks before disk writes.
- `pthread_mutex_t` protects shared buffer state.
- `pthread_cond_t` coordinates producers and the consumer.
- Logs are written under `logs/<container-id>.log`.

The bounded buffer prevents uncontrolled memory growth when containers produce output quickly. Producers block when the buffer is full, and the consumer signals them when space becomes available.

Existing log artifacts were generated for containers such as:

- `alpha`
- `beta`
- `alpha2`
- `hi`
- `lo`
- `memtest`

These are visible under `boilerplate/logs/` in the local project directory.

### 6. Container Stop and Exit Classification

The supervisor implements a stop flow:

```bash
./engine stop <id>
```

When stopping a container, the supervisor:

1. Sets an internal `stop_requested` flag.
2. Sends `SIGTERM`.
3. Waits briefly.
4. Sends `SIGKILL` if the container is still running.

The supervisor uses the stop-request flag to distinguish a manually stopped container from a container killed by another reason.

Exit classifications include:

- Normal exit
- Manual stop
- Signal-based kill
- Hard-limit kill, when a process exits due to `SIGKILL` without a manual stop request

### 7. Test Workloads

Three workload programs are included.

#### CPU Workload

```bash
./cpu_hog [seconds]
```

This program burns CPU and prints progress once per second. It is useful for scheduler experiments, especially when comparing containers with different nice values.

#### Memory Workload

```bash
./memory_hog [chunk_mb] [sleep_ms]
```

This program allocates memory repeatedly and touches each page so RSS grows. It is useful for testing soft and hard memory-limit behavior.

#### I/O Workload

```bash
./io_pulse [iterations] [sleep_ms]
```

This program repeatedly writes to `/tmp/io_pulse.out`, calls `fsync()`, prints progress, and sleeps between iterations. It is useful for comparing I/O-oriented behavior against CPU-bound workloads.

## Build Instructions

Install dependencies on Ubuntu 22.04 or 24.04:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

Build everything:

```bash
make
```

Build only CI-safe user-space targets:

```bash
make ci
```

Clean generated files:

```bash
make clean
```

## Environment Check

The project includes a preflight script:

```bash
chmod +x environment-check.sh
sudo ./environment-check.sh
```

The script checks:

- Ubuntu version
- WSL detection
- VM environment
- Secure Boot status
- Kernel headers
- Build success
- Kernel module insertion/removal
- `/dev/container_monitor` availability

This project is intended for an Ubuntu VM with Secure Boot disabled. WSL is not supported for kernel-module testing.

## Example Usage

Start the supervisor:

```bash
./engine supervisor ./rootfs-base
```

In another terminal, start containers:

```bash
./engine start alpha ./rootfs-alpha "/bin/sh -c 'echo hello from alpha; sleep 5'"
./engine start beta ./rootfs-beta "/bin/sh -c 'echo hello from beta; sleep 5'"
```

List containers:

```bash
./engine ps
```

View logs:

```bash
./engine logs alpha
```

Stop a container:

```bash
./engine stop alpha
```

Run a CPU experiment:

```bash
cp ./cpu_hog ./rootfs-alpha/
cp ./cpu_hog ./rootfs-beta/

./engine start hi ./rootfs-alpha "/cpu_hog 10" --nice 0
./engine start lo ./rootfs-beta "/cpu_hog 10" --nice 10
```

Run a memory experiment:

```bash
cp ./memory_hog ./rootfs-alpha/

./engine start memtest ./rootfs-alpha "/memory_hog 8 1000" --soft-mib 40 --hard-mib 64
```

## Scheduler Experiment Design

The included workloads allow scheduler experiments such as:

1. Run two CPU-bound containers with different nice values.
2. Compare a CPU-bound container with an I/O-oriented container.
3. Observe completion time, responsiveness, and log output timing.

Example comparison:

```bash
./engine start hi ./rootfs-hi "/cpu_hog 15" --nice 0
./engine start lo ./rootfs-lo "/cpu_hog 15" --nice 10
```

Expected behavior:

- The lower nice value should receive more favorable CPU scheduling.
- The higher nice value should still make progress, but may show less responsive progress under CPU pressure.
- I/O-oriented workloads should often remain responsive because they sleep and yield CPU while waiting between bursts.

## Kernel Monitor Design

The intended kernel monitor design uses:

- `/dev/container_monitor`
- `MONITOR_REGISTER` ioctl
- `MONITOR_UNREGISTER` ioctl
- A kernel linked list of monitored container PIDs
- Lock-protected list access
- Periodic RSS checks
- Soft-limit warning logs
- Hard-limit process termination

The user-space supervisor already attempts to open:

```text
/dev/container_monitor
```

and registers each launched container with:

```c
MONITOR_REGISTER
```

It unregisters containers after exit with:

```c
MONITOR_UNREGISTER
```

## Current Status and Known Limitations

The user-space runtime contains the main supervisor, CLI, container launch, metadata, and logging logic.

The current top-level `monitor.c` is incomplete compared with the intended kernel-monitor design. It loads as a basic module, but it does not currently implement the full character-device and ioctl interface required for `/dev/container_monitor`. Because of this, memory-limit enforcement may not work until the monitor module is completed or restored.

Other known limitations:

- The `run` command waits for container completion but should be improved to return the actual container exit status.
- CLI responses should be converted to a clean text protocol instead of mixing raw structs and text output.
- Container child configuration lifetime should be made safer around `clone()`.
- Signal handling should avoid doing complex pthread and I/O work directly inside the `SIGCHLD` handler.
- `/proc` should be mounted after entering the container rootfs.
- Generated binaries, rootfs folders, logs, and downloaded tarballs should not be committed to GitHub.

## Verification Done

The following checks were performed locally:

```bash
gcc -O2 -Wall -Wextra -fsyntax-only engine.c
make ci
./engine
```

Results:

- `engine.c` passed syntax checking with `-Wall -Wextra`.
- `make ci` completed because user-space targets were already built.
- `./engine` with no arguments printed usage and exited with a non-zero status, matching the smoke-check expectation.

Full kernel-module and container runtime testing should be done inside an Ubuntu VM with root privileges, kernel headers installed, and Secure Boot disabled.

## GitHub Notes

Before pushing to GitHub, avoid committing generated artifacts such as:

- `engine`
- `cpu_hog`
- `io_pulse`
- `memory_hog`
- `monitor.ko`
- `*.o`
- `*.mod`
- `Module.symvers`
- `modules.order`
- `logs/`
- `rootfs-base/`
- `rootfs-*`
- downloaded Alpine `.tar.gz` files

The `.gitignore` already contains rules for most of these files.

## Conclusion

This project demonstrates a supervised multi-container runtime in C using Linux namespaces, UNIX socket IPC, pipe-based logging, bounded-buffer synchronization, container metadata tracking, lifecycle commands, and workload programs for scheduling and memory experiments.

The main user-space runtime has been implemented, and the remaining major work is to complete the kernel monitor so memory-limit enforcement works through `/dev/container_monitor` and ioctl registration.
