# SimpleNet

`snet` is a modular C++20 library of networking components for Linux.
The current API provides socket address values and a single-threaded Reactor
based on `epoll`.

- `snet::Address` stores an IPv4, IPv6, or Unix domain socket address.
- `snet::Channel` associates an existing fd with event masks and callbacks.
- `snet::Poller` registers channels and waits for events, allowing custom loops.
- `snet::EventLoop` provides a ready-to-use event waiting and dispatch loop.

Channels do not own monitored fds. The caller controls socket creation,
non-blocking mode, I/O, and resource lifetimes. TCP lifecycle management and
buffering are not yet implemented. Contracts and examples for both loop models
are described in the [Reactor guide](docs/reactor.md).

## Building

Requires Linux, CMake 3.20 or later, and a compiler with C++20 support.

```sh
cmake -S . -B build
cmake --build build
```

Include the public API with `#include <simplenet/Simplenet.hpp>`.
When using CMake, link your application to the `SimpleNet` target.

## Tests

Reactor regression tests are built by default and use GoogleTest 1.17.0.
CMake downloads and builds this pinned release through FetchContent; a separate
GoogleTest installation is not needed. The first configuration requires Git
and internet access. GoogleMock and GoogleTest installation targets are disabled.
Run the tests with:

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

CMake discovers GoogleTest cases automatically. Each case runs in a separate
process with a ten-second timeout. Tests use real
Linux eventfd descriptors, pipes, socket pairs, and signals. They cover registration
failures, mask changes, channel removal during dispatch, callback guards, hangup and
half-close, loop termination, continuation after exceptions, and interrupted waits.
GoogleTest assertions remain active in Release builds.

To build only the library without requiring GoogleTest, configure with
`-DBUILD_TESTING=OFF`; GoogleTest is not downloaded in this mode.

For an offline build, provide an existing GoogleTest source tree, preferably
the same 1.17.0 release:

```sh
cmake -S . -B build -DBUILD_TESTING=ON \
    -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest
```

## Doxygen documentation

Install Doxygen and reconfigure CMake, then run:

```sh
cmake -S . -B build -DSIMPLENET_BUILD_DOCS=ON
cmake --build build --target docs
```

Open `build/docs/html/index.html`. The main page, Reactor guide, and API reference
are generated from README, `docs/reactor.md`, and the public headers.
Comments document parameters, return values, preconditions, exceptions,
resource ownership, and limitations of the current implementation. Doxygen
warnings are saved to `build/doxygen-warnings.log` and cause generation to fail.
Graphviz is not required.

The library builds without Doxygen, but the `docs` target is unavailable.
You can explicitly disable Doxygen discovery:

```sh
cmake -S . -B build -DSIMPLENET_BUILD_DOCS=OFF
```
