# SimpleNet

`snet` is a modular C++20 library of networking components for Linux.
The current API provides socket address values, socket ownership, a contiguous byte buffer, an established TCP connection, and a single-threaded Reactor
based on `epoll`.

- `snet::TcpConnection` owns an established TCP socket and services its input/output queues.
- `snet::Buffer` stores bytes and exposes contiguous readable and prepared writable regions.
- `snet::Socket` owns an already-open socket descriptor and closes it through RAII.
- `snet::Address` stores an IPv4, IPv6, or Unix domain socket address.
- `snet::Channel` associates an existing fd with event masks and callbacks.
- `snet::Poller` registers channels and waits for events, allowing custom loops.
- `snet::EventLoop` provides a ready-to-use event waiting and dispatch loop.

Channels do not own monitored fds. Low-level users control I/O themselves;
`TcpConnection` supplies bounded non-blocking I/O and connection lifecycle management.
Socket establishment and the owner lifetime remain explicit. Acceptor and TcpServer
are planned next. See the [TCP connection guide](docs/tcp-connection.md) and
[Reactor guide](docs/reactor.md).

## Socket ownership

`Socket` takes exclusive ownership of an already-open socket. It does not create
connections, change descriptor flags, or perform I/O:

```cpp
#include <simplenet/Simplenet.hpp>
#include <cerrno>
#include <system_error>
#include <sys/socket.h>
#include <utility>

void example()
{
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd == -1)
        throw std::system_error(errno, std::generic_category(), "socket");

    snet::Socket first(fd);                 // Ownership transfers to first.
    snet::Socket second(std::move(first)); // first is now empty.
    int borrowed_fd = second.get_fd();     // Borrowed access for system calls.
    (void)borrowed_fd;
} // second closes the descriptor; first does nothing.
```

Copying is prohibited. Move assignment closes the destination's previous socket
before taking ownership; self-move preserves it. `release()` returns the descriptor
and leaves the wrapper empty without closing it: the recipient must then own its
cleanup. `get_fd()` does not transfer ownership; never independently close or adopt
that borrowed descriptor. `is_open()` reports wrapper state, without querying the
kernel. A default-constructed or moved-from socket holds `-1`; descriptor zero is
valid. Negative adoption arguments throw `std::invalid_argument`.

Remove any associated Channel from its Poller before closing, destroying, or
replacing its owning Socket. `close()` makes the wrapper empty, attempts the Linux close system call
at most once, and preserves `errno`. Errors are not reported by this `void` API.
On Linux a close error, including `EINTR`, must not trigger another attempt because
the descriptor number may already have been released and reused; see
[close(2)](https://man7.org/linux/man-pages/man2/close.2.html).
Closing does not confirm delivery of TCP data to the peer. `Socket` adds ownership,
not TCP connection lifecycle management.

## Byte buffering

`Buffer` stores bytes independently of sockets, protocols, and connection limits.
It keeps read and write positions in contiguous storage, so consuming a prefix does
not move the remaining bytes:

```cpp
#include <simplenet/Simplenet.hpp>
#include <array>

void buffer_example()
{
    snet::Buffer buffer;
    const std::array<char, 4> input{'A', 'B', 'C', 'D'};
    buffer.append(input);
    buffer.consume(2); // data() now exposes C,D.

    auto readable = buffer.data(); // Borrowed, read-only access.
    (void)readable;
    auto writable = buffer.prepare_write(2); // May compact or grow storage.
    writable[0] = 'E';
    writable[1] = 'F';
    buffer.commit_write(2); // The useful sequence is now C,D,E,F.
}
```

`prepare_write(n)` prepares existing writable elements before an external operation
writes bytes. Its returned span may be larger than requested. `commit_write(n)`
marks the actual written count as useful without allocating; it does not inspect
whether those bytes were written. Do not modify the buffer between preparing,
writing, and committing. `consume(n)` marks a prefix as used; consuming the whole
sequence resets positions and retains memory. `readable_size()`, `writable_size()`,
and `empty()` report the current regions. Storage grows geometrically and is
compacted when consumed space suffices. There is no built-in connection limit.

`append` copies the entire source or adds nothing on allocation failure. Existing
useful bytes survive failed preparation or append. `append(buffer.data())` and
subranges of that current readable view are supported: their source is staged
before storage can move. Views of this buffer's consumed or writable regions are
not valid sources for this overload. Zero-count operations do nothing.
Over-consuming or over-committing throws `std::out_of_range` before changing the
buffer; an impossible preparation size throws `std::length_error`.

Readable and writable spans borrow memory. Obtain them again after any modifying
operation, including preparation, assignment, and movement; never keep them after
the buffer is destroyed. Copying creates independent storage. Moving leaves the
source logically empty and reusable; self-move preserves its value. For future
socket reads, prepare memory before receiving bytes so allocation failure cannot
lose bytes already taken from the kernel.

## Building

Requires Linux, CMake 3.20 or later, and a compiler with C++20 support.

```sh
cmake -S . -B build
cmake --build build
```

Include the public API with `#include <simplenet/Simplenet.hpp>`.
When using CMake, link your application to the `SimpleNet` target.

## Tests

Reactor, Socket, Buffer, and TCP connection regression tests are built by default and use GoogleTest 1.17.0.
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
Socket tests cover move ownership, release, descriptor zero, stack unwinding, and
reused descriptor numbers. An isolated test executable uses linker interception to
verify the single-attempt close policy and preservation of errno on simulated errors;
the production library contains no test hooks.
Buffer tests cover binary byte order, compaction, growth, self-append, count errors,
copy/move lifetimes, and preservation after allocation failure. A separate executable
replaces allocation functions only for those failure and allocation-count checks.
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

### Internal work and safe object lifetime

EventLoop supports prepared same-thread actions through `detail::LoopWork` and
safe owner cleanup through `detail::LoopCleanup` / `request_cleanup()`. Owners keep
their objects and `unique_ptr`s; the loop supplies a safe phase after dispatch or
handler unwind. Cleanup hooks never run in the loop destructor and must not invoke
application callbacks. Work is bounded to 64 records per iteration by default;
prepared enqueue/cancel/cleanup requests allocate no new memory. These are
component-building facilities, not a thread pool. See [the Reactor lifetime and
cleanup contract](docs/reactor.md#prepared-work-and-owner-cleanup).
