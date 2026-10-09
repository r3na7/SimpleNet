# snet v1 readiness

The agreed library scope is Linux, C++20, TCP IPv4/IPv6, non-blocking sockets,
level-triggered epoll and one Reactor thread. Components can be used independently
or assembled through TcpServer.

## Criteria and evidence

| Criterion | Implementation and checks |
|---|---|
| Public API | Public headers/Simplenet.hpp; external add_subdirectory consumer fixture |
| Reactor | Channel, Poller, EventLoop; registration, dispatch, budgets and saved-batch tests |
| TCP lifecycle | TcpConnection, Acceptor, TcpServer; EOF, half-close, finish, close and stable owner cleanup |
| Input/output buffering | Buffer and bounded queues; partial I/O, threshold callbacks and retained-input retries |
| Error handling | Socket/resource/registration/exception and allocation-failure tests |
| RAII | Socket ownership, fd reuse, stable addresses, map rehash and callback-lifetime tests |
| Examples | One-file C++ echo server and interactive Python client; two executable smoke tests |
| Build/documentation | CMake dependency gating, README, guides and Doxygen warnings-as-errors |

There are 270 independent library regression tests, including 31 allocator-shim cases.
The two current echo smoke tests add checks for server messages, exact echo, successive
clients and multiple interactive messages in one connection. The previous complex
signal/slow-client demonstration and its 28 tests have been removed at the user's
request; they do not describe the current example. Library contracts remain unchanged.

The queued-output exception regression sends before the unrelated exception and
verifies delivery after loop restart without a second send. The external consumer
fixture uses only public includes and the SimpleNet CMake target.

## Reproduce

```sh
cmake -S . -B build -DSIMPLENET_BUILD_EXAMPLES=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
cmake --build build --target docs
```

Use `-DCMAKE_BUILD_TYPE=Debug` or `Release` in separate build directories. For offline
GoogleTest, add `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest`; 1.17.0
remains the default pin. The local development override uses source 1.14.

Library-only builds can disable BUILD_TESTING, SIMPLENET_BUILD_EXAMPLES and
SIMPLENET_BUILD_DOCS. The C++ server alone needs examples ON and testing OFF.
Examples+testing require Python 3.8+ for the smoke tests. Tests use port 5555, so stop
a manually running example before invoking them.

Copy `tests/consumer/` outside the source tree, configure with
`-DSNET_SOURCE_DIR=/absolute/path/to/SimpleNet`, build and run `snet_consumer_check`.
No install/find_package package is exported.

For sanitizer checks, configure Clang with compiler flags
`-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie` and linker flags
`-fsanitize=address,undefined -no-pie`, then run:

```sh
ctest --test-dir build-sanitize --output-on-failure \
  -E '^(loopalloc|bufferalloc|tcpalloc|acceptalloc|serveralloc)[.]'
```

The 31 allocator-shim tests run in ordinary builds. Sanitizer checks select the other
library tests and the enabled example smoke tests.

## Limits

All use is in the Reactor thread. Owners must survive callbacks/dispatch/work and
unregister Channels before closing monitored fds. Server-owned references expire
after owner cleanup. Exceptions do not roll back application state.

send reports queue acceptance, not delivery. EOF ends one direction; finish_sending
drains output, while close/stop discard it. The minimal example uses ordinary process
termination for Ctrl+C and provides no graceful signal-shutdown machinery.

The library supports IPv4/IPv6; the deliberately small executable uses a fixed IPv4
loopback address. DNS/outgoing connection factories, UDP/TLS/HTTP, concurrency,
coroutines, timers, installers and benchmarks are outside the agreed version scope.
This is demonstration readiness, not a claim of long-duration/load validation or
general production readiness. Publication and tagging are separate decisions.
