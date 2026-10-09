# snet v1 readiness

This version targets a demonstrable modular C++20 networking library on Linux:
TCP, IPv4/IPv6, one Reactor execution thread, non-blocking I/O and level-triggered epoll.
Components can be used independently or assembled through TcpServer.

## Criteria and evidence

| Criterion | Implementation | Verification |
|---|---|---|
| Public API | Public headers and Simplenet.hpp; independent components plus TcpServer | External add_subdirectory consumer builds/runs in Debug and Release |
| Reactor and readiness | Channel, Poller, EventLoop | Registration/dispatch/cancellation, saved-batch and budget tests |
| TCP lifecycle | TcpConnection, Acceptor, TcpServer | EOF/half-close/finish/close, multiple clients, stop operations and owner cleanup |
| Input/output queues | Buffer and bounded ConnectionOptions | Partial reads/writes, queue limits, threshold callbacks and retained-input retries |
| Socket/system errors | Per-connection terminal errors; paused resource exhaustion; infrastructure exceptions | Real TCP and isolated syscall-failure tests |
| RAII and stable ownership | Socket; server unique_ptr owners; owner cleanup phase | Descriptor reuse, map rehash, callbacks/exceptions, allocation failures and sanitizer checks |
| Examples | snet_echo_server and slow_echo_client.py | Real IPv4/IPv6 processes, binary echo larger than limits, slow simultaneous clients and EOF |
| Process stopping | Example-local SignalStop | SIGINT/SIGTERM, active clients, mask rollback and unregister-before-close tests |
| CMake | Optional examples; public SimpleNet target | Library-only/example-only dependency checks and external consumer |
| Documentation | README, component and demonstration guides, public API comments | Executed quick-start commands; Doxygen with warnings treated as errors |
| Structure | include/simplenet, src, tests, examples, docs | Build products in ignored build directories; no product test hooks |

The strengthened `UnrelatedExceptionCancelsClosedButPreservesLiveOutput` regression
queues output before another action throws. Restarting the loop delivers those bytes
without another send; closed candidates are independently removed. A deliberate
cancellation mutation makes the regression fail.

The baseline had 270 tests. The demonstration adds 7 signal-owner cases and
18 executable/client cases: **295 ordinary tests**, including 31 allocation-shim cases.
Clean Debug and Release suites both pass. GoogleTest 1.17.0 remains the pinned default;
local checks use the explicitly supplied system source 1.14 as an offline override.
IPv6 tests were executed, not skipped.

## Reproduce verification

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DSIMPLENET_BUILD_EXAMPLES=ON
cmake --build build-debug -j2
ctest --test-dir build-debug --output-on-failure
cmake --build build-debug --target docs
```

Repeat in another build directory with `-DCMAKE_BUILD_TYPE=Release`. To use offline
GoogleTest sources, add `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest`.
For a build without test/client/documentation dependencies, disable BUILD_TESTING,
SIMPLENET_BUILD_EXAMPLES and SIMPLENET_BUILD_DOCS. The server alone needs examples ON
and testing OFF. Enabling both examples and tests explicitly requires Python 3.8+;
missing Python fails configuration instead of silently omitting executable tests.

A separate consumer fixture lives in `tests/consumer/`. Copy it outside the library
source tree, configure with `-DSNET_SOURCE_DIR=/absolute/path/to/SimpleNet`, build and
run `snet_consumer_check`. It includes only public headers and links SimpleNet;
no install/find_package package is claimed.

Sanitizer configuration for Clang:

```sh
cmake -S . -B build-sanitize -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug \
  -DSIMPLENET_BUILD_EXAMPLES=ON \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined -no-pie'
cmake --build build-sanitize -j2
ctest --test-dir build-sanitize --output-on-failure \
  -E '^(loopalloc|bufferalloc|tcpalloc|acceptalloc|serveralloc)\.'
```

The 31 tests replacing allocation functions run in ordinary builds, separately from
ASan. All remaining **264 tests pass under ASan/UBSan/LSan**, including signal/subprocess
cases that launch the sanitized server. Doxygen generates with zero warnings.
The final verification record is retained with the development plan.

## Supported limits

All library use is in the Reactor thread; cross-thread calls and wakeups are unsupported.
Owners must survive their callbacks/dispatch/work and remove Channel registrations
before closing monitored descriptors. Server-owned references expire after cleanup.
Callbacks may throw, but the loop does not roll back application state.

send accepts a copied prefix into the library queue, not a delivery acknowledgement.
EOF ends one TCP direction. finish_sending drains output; close/stop discard output.
The demo's signal stop is immediate and its listener/resource failure policy exits;
applications choose their own recovery and graceful shutdown policies.

No DNS/client-connection factory, UDP/TLS/HTTP, thread pool, multiple reactors,
coroutines, timers, installer package or benchmark claim is included. Address can
represent an existing Unix address value; v1 networking scenarios remain TCP IPv4/IPv6.

This is readiness for demonstration of the agreed v1 scope. A green regression suite
and examples do not replace long-duration/load testing or establish general
production readiness. Remote publication and version tagging are separate decisions.
