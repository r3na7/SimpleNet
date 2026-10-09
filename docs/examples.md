# Runnable TCP demonstration

The demonstration uses the public C++20 API on Linux. It assembles existing components;
its echo and process-stop policies are application code.

## Build and run

```sh
cmake -S . -B build -DSIMPLENET_BUILD_EXAMPLES=ON
cmake --build build -j2
./build/examples/snet_echo_server --host 127.0.0.1 --port 5555
```

The server prints `READY 127.0.0.1 5555` after successful activation. In another terminal:

```sh
python3 examples/slow_echo_client.py --host 127.0.0.1 --port 5555 --clients 2
```

The client verifies 256 KiB of binary echo per connection, then EOF. It sends and reads
concurrently across peers, initially waits 250 ms before reading, then reads at most
1024 bytes every 5 ms per peer. Its overall deadline is 15 seconds. It uses Python 3.8+
and the standard library. A corrupted, missing, extra or incomplete response is a failure.

For IPv6, use `--host ::1` on both commands. The IPv6 listener is IPv6-only. Hosts must
be numeric addresses, without DNS names or IPv6 zone IDs. Both programs offer `--help`.
Server port zero is useful for tests: its READY line reports the assigned port.

Use Ctrl+C or SIGTERM to stop the server. Signal stop returns zero, argument errors
return two, and infrastructure failures return one. The client returns zero only on
complete verification, two on argument errors and one on verification/network failures.

To build the C++ server without GoogleTest or Python:

```sh
cmake -S . -B build-example -DBUILD_TESTING=OFF -DSIMPLENET_BUILD_EXAMPLES=ON -DSIMPLENET_BUILD_DOCS=OFF
cmake --build build-example -j2
```

Python is needed to run the client and, when testing and examples are both enabled,
to configure/run the subprocess tests. See README for offline GoogleTest configuration.

## Ownership and the Reactor

```text
application creates listening Socket and EventLoop
   |
   +-- TcpServer owns Acceptor and accepted TcpConnection objects
   |        |
   |        +-- connection owns socket, Channel, input and output queues
   |
   +-- SignalStop owns signalfd, its Channel registration and saved signal mask

EventLoop delivers readiness and scheduled work in one thread.
Owners remove registrations before closing fds or destroying Channels.
```

The signal mask is blocked before creating signalfd. SIGINT/SIGTERM become readable
records; the Channel callback drains them and calls `server.stop()` and `loop.quit()`
in ordinary loop execution. No async signal handler invokes C++ application code.
SignalStop unregisters its Channel before closing its fd and restoring the previous
mask. Constructor failures roll back the mask/fd too. The server is destroyed after
loop dispatch returns; the loop outlives all registrations and owners.

## Two queues and echo pumping

The demonstration fixes input to 64 KiB, output to 8 KiB and the output notification
threshold to 4 KiB. Standard per-iteration I/O budgets remain unchanged.

`on_connection` installs handlers before the server activates a connection. Shared
captures own small echo state, while TcpServer exclusively owns the connection.

1. `on_data` runs the pump: send the readable input span into the output queue.
2. Consume only `accepted_bytes`. If output was full, retain the input suffix and pause reading.
3. `on_output_available` runs the same pump when output crosses the low watermark downward.
4. After consuming the suffix, resume reading. The suffix needs processing even if no new network event arrives.
5. On EOF, remember that input has ended. After the remaining input is processed, request `finish_sending()` once.

A terminal send result ends pumping for that connection. Final input observed alongside
a socket error is not promised an echo response. The callback does not resend after
finish was requested. No framing is performed: TCP supplies bytes, not requests.

EOF and process stop have different policies. EOF lets output drain before SHUT_WR
and normal closure. SIGINT/SIGTERM uses immediate `stop()`, which can discard queued
responses. It is not a graceful shutdown with deadlines or a delivery guarantee.
A client reset affects that client; listener/resource/registration/allocation errors
abort this example with diagnostics. A different application can choose its own recovery.

The [two-client fragment](tcp-server.md#two-client-echo-example) deliberately quits
after two closed clients; this executable continues accepting until a stop signal.

## Checks and interpretation

With examples and testing enabled, CTest runs real subprocess scenarios for IPv4/IPv6,
slow simultaneous clients, later clients, signals, malformed arguments and occupied
addresses. Client failure cases use local controlled peers for corruption, early EOF
and timeout. Harnesses use deadlines and always wait for their child processes.

Direct signal-owner tests exercise mask restoration and DEL-before-close ordering,
including failed signalfd creation and failed Channel registration. Existing library
tests cover partial I/O, EAGAIN, EOF, budgets, exceptions and allocation failures.

An executable run proves exact observed echo and termination; it does not guarantee
that a particular kernel call hit EAGAIN on that run, prove fairness under every load,
or confirm delivery from `send()` queue acceptance alone. See the [v1 status](v1-status.md)
for the readiness evidence and the [connection contract](tcp-connection.md) for details.
