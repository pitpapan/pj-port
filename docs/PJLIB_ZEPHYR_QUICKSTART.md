# PJLIB Zephyr Port: Environment, Installation, and Quick Start

This guide describes the environment used to develop and validate the PJLIB
Zephyr port, how to reproduce it on Linux, and how to run the validation
applications.

The primary development target is Zephyr `mps2/an385` under QEMU. Zephyr is an
external platform dependency: use documented `west` commands and build output
when working in this repository; do not inspect or modify files under
`zephyr/`.

## Validated environment

The following versions produced the passing Stage 9 and Stage 10 results:

| Component | Validated version or value |
| --- | --- |
| Host | Linux 5.15.0-187-generic, x86_64 |
| Workspace | `/home/pitpapan/zephyrproject` |
| Zephyr | 4.4.0 |
| West | 1.5.0 |
| Python | 3.12.13 in `.venv` |
| CMake | 4.4.2 in `.venv` |
| Ninja | 1.10.1 |
| Zephyr SDK | 1.0.1 |
| ARM compiler | Zephyr SDK GCC 14.3.0 |
| QEMU | 10.0.2 |
| PJPROJECT/PJLIB | 2.16 |
| Board target | `mps2/an385` |
| Emulated CPU | ARM Cortex-M3 |
| C library | Picolibc |
| C++ standard library | LLVM libc++ (`CONFIG_GLIBCXX_LIBCPP`) |
| PJLIB ioqueue | `select`, 32 handles, safe unregister enabled |

The SDK is installed at:

```text
/home/pitpapan/zephyr-sdk-1.0.1
```

The ARM compiler is installed at:

```text
/home/pitpapan/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc
```

QEMU comes from the SDK host tools. It does not need to be on the interactive
shell's `PATH`; the Zephyr runner locates it when `west build -t run` is used.

`CONFIG_PICOLIBC=y` and `CONFIG_GLIBCXX_LIBCPP=y` are not competing C-library
choices. The first selects the C library and the second selects the C++
standard library.

## Workspace layout

```text
zephyrproject/
├── .venv/                         Python and west environment
├── .west/                         West workspace metadata
├── zephyr/                        External Zephyr dependency
├── modules/                       Projects managed by west
├── pjproject/                     PJPROJECT source tree
│   └── zephyr/                    Zephyr-only CMake module entry point
├── applications/pjlib_minimal/
│   ├── prj.conf                   Common port configuration
│   ├── stage8.conf                Core-runtime validation overlay
│   ├── stage9.conf                Networking validation overlay
│   ├── stage10.conf               ioqueue validation overlay
│   └── src/
│       ├── main.c
│       ├── stage8_core.c
│       ├── stage9_network.c
│       └── stage10_ioqueue.c
└── docs/
    ├── PJLIB_ZEPHYR_PORT_PLAN.md
    └── PJLIB_ZEPHYR_QUICKSTART.md
```

The Stage 9 and Stage 10 source files are test harnesses. They exercise the
existing PJLIB BSD/POSIX networking and select ioqueue implementations; they
are not replacement networking or ioqueue backends.

## Installation from scratch on Ubuntu/Linux

These instructions reproduce the validated tool versions where practical.
The general workflow follows the official
[Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html)
and [west workspace documentation](https://docs.zephyrproject.org/latest/develop/west/basics.html).

### 1. Install host packages

```bash
sudo apt update
sudo apt install --no-install-recommends \
  git ninja-build gperf ccache dfu-util device-tree-compiler wget ripgrep \
  python3.12 python3.12-dev python3.12-venv python3-tk \
  xz-utils file make gcc gcc-multilib g++-multilib libsdl2-dev libmagic1
```

On an ARM64 Linux host, omit `gcc-multilib` and `g++-multilib` if the
distribution does not provide them. Zephyr documents additional distributions
in its [Linux installation guide](https://docs.zephyrproject.org/latest/develop/getting_started/installation_linux.html).

### 2. Obtain the port repository

Replace the placeholder with the URL of the repository containing this port:

```bash
git clone <pjlib-zephyr-port-repository-url> ~/zephyrproject
cd ~/zephyrproject
```

If the port was supplied as an archive, extract it as `~/zephyrproject` and
enter that directory instead.

### 3. Create the Python environment

```bash
python3.12 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install west==1.5.0 cmake==4.4.2
```

Activate `.venv` in every new terminal used for this workspace.

### 4. Install the pinned Zephyr workspace

Clone Zephyr 4.4.0 as the local west manifest repository:

```bash
git clone --branch v4.4.0 https://github.com/zephyrproject-rtos/zephyr.git zephyr
west init -l zephyr
west update
```

`west update` downloads the module revisions selected by Zephyr's manifest. It
can take several minutes and requires network access.

Install the Python packages selected by the checked-out workspace and export
Zephyr to CMake:

```bash
west packages pip --install
west zephyr-export
```

### 5. Install the Zephyr SDK

Install SDK 1.0.1 with the ARM GNU toolchain and host tools:

```bash
source zephyr/zephyr-env.sh
west sdk install \
  --version 1.0.1 \
  --install-base ~ \
  --gnu-toolchains arm-zephyr-eabi
```

Leaving host tools enabled installs the QEMU runner needed by `mps2/an385`.
The official [`west sdk` documentation](https://docs.zephyrproject.org/latest/develop/west/zephyr-cmds.html)
describes toolchain selection and alternative install directories.

Normally Zephyr discovers an SDK installed as `~/zephyr-sdk-1.0.1`. If it does
not, set the location explicitly for the current terminal:

```bash
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-1.0.1
```

### 6. Verify the installation

```bash
source .venv/bin/activate
source zephyr/zephyr-env.sh
west --version
west topdir
python --version
cmake --version
ninja --version
west boards | grep '^mps2$'
```

The important results are:

```text
West version: v1.5.0
.../zephyrproject
Python 3.12.x
cmake version 4.4.2
mps2
```

## Starting an existing checkout

For the environment already installed at `/home/pitpapan/zephyrproject`, each
new terminal only needs:

```bash
cd /home/pitpapan/zephyrproject
source .venv/bin/activate
source zephyr/zephyr-env.sh
west topdir
```

Expected `west topdir` result:

```text
/home/pitpapan/zephyrproject
```

Do not run `west update` routinely in this validated checkout. It can change
external module state. Use it when initially installing the workspace or after
intentionally changing the Zephyr manifest revision.

## Build and run Stage 9: networking

Use a pristine configuration when selecting the overlay:

```bash
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always \
  -b mps2/an385 \
  applications/pjlib_minimal \
  -d build-stage9 \
  -- -DEXTRA_CONF_FILE=stage9.conf
```

west build -p always \
  -b mps2/an385 \
  applications/pjlib_minimal \
  -d build-stage5 \
  -- -DEXTRA_CONF_FILE=stage5_probe.conf
Run it under QEMU:

```bash
timeout 30s west build -d build-stage9 -t run
```

The successful run ends with:

```text
STAGE 9 RESULT: PASSED
```

Stage 9 validates DNS, UDP, TCP, socket options, nonblocking error mapping,
connect, accept, data exchange, and peer close behavior.

## Build and run Stage 10: ioqueue

```bash
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always \
  -b mps2/an385 \
  applications/pjlib_minimal \
  -d build-stage10 \
  -- -DEXTRA_CONF_FILE=stage10.conf
```

Run it under QEMU:

```bash
timeout 30s west build -d build-stage10 -t run
```

The successful run ends with:

```text
[Stage 10] configured maximum of 32 handles and overflow rejection: PASSED
STAGE 10 RESULT: PASSED
```

The command normally returns status 124 because `timeout` terminates QEMU
after the validation application returns and Zephyr enters its idle state. The
test passed if `STAGE 10 RESULT: PASSED` appeared before termination.

Without `timeout`, exit QEMU by pressing `Ctrl+A`, then `X`.

Stage 10 validates:

- the selected `select` backend and compile-time policy;
- readable readiness and multiple sockets;
- immediate or queued write completion;
- asynchronous TCP connect/accept completion;
- poll timeout behavior;
- unregister/close while another thread is blocked in `select()`;
- suppression of stale callbacks after unregister;
- simultaneous callbacks from two polling threads;
- repeated registration and safe-unregister key recycling; and
- the configured 32-handle limit and overflow rejection.

## Stage 10 resource configuration

PJLIB advertises `PJ_IOQUEUE_MAX_HANDLES=32`. Zephyr's smaller defaults must be
raised so the validation reaches that PJLIB limit instead of failing at a
platform resource ceiling first. `stage10.conf` supplies:

```text
CONFIG_NET_MAX_CONTEXTS=40
CONFIG_NET_MAX_CONN=40
CONFIG_ZVFS_OPEN_MAX=48
CONFIG_ZVFS_POLL_MAX=40
CONFIG_MAX_PTHREAD_MUTEX_COUNT=48
CONFIG_DYNAMIC_THREAD_POOL_SIZE=8
CONFIG_DYNAMIC_THREAD_STACK_SIZE=8192
```

Verify the effective build configuration with:

```bash
rg 'CONFIG_(PJLIB_STAGE10_TEST|NET_MAX_CONTEXTS|NET_MAX_CONN|ZVFS_OPEN_MAX|ZVFS_POLL_MAX|MAX_PTHREAD_MUTEX_COUNT|DYNAMIC_THREAD_POOL_SIZE)=' \
  build-stage10/zephyr/.config
```

Do not assign `CONFIG_POSIX_OPEN_MAX` directly. It is a derived Kconfig value;
`CONFIG_ZVFS_OPEN_MAX` is the application-configurable descriptor capacity.

## Incremental rebuilds

After changing only application or PJLIB port source:

```bash
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -d build-stage10
```

Use `-p always` again after changing a Kconfig option, configuration overlay,
board target, or CMake integration.

## Troubleshooting

### `west: command not found`

Activate the workspace virtual environment:

```bash
source /home/pitpapan/zephyrproject/.venv/bin/activate
```

### CMake cannot locate Zephyr

Activate the environment and load Zephyr's shell environment:

```bash
source .venv/bin/activate
source zephyr/zephyr-env.sh
west zephyr-export
```

### The SDK is not detected

```bash
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-1.0.1
```

Then rerun the pristine build command.

### `qemu-system-arm` is not found in the interactive shell

This is normal for this installation. Invoke QEMU through the Zephyr runner:

```bash
west build -d build-stage10 -t run
```

### Stage 10 reports insufficient space or resource exhaustion

Confirm that `stage10.conf` was supplied as `EXTRA_CONF_FILE` and inspect the
effective values in `build-stage10/zephyr/.config`. In particular, the pthread
mutex, VFS poll, network connection, context, and descriptor limits must not
fall back to their smaller defaults.

### Confirm that no QEMU process remains

Runs launched with `timeout` terminate automatically. Check with:

```bash
ps -C qemu-system-arm -o pid=,stat=,args=
```

No output means there is no running QEMU ARM process.

## Current validation state

- Stage 8 core PJLIB runtime validation: completed.
- Stage 9 basic networking validation: passed.
- Stage 10 select ioqueue validation: passed twice under QEMU.
- Stage 11: not started.
