# Simple echo example

The example consists of two source files:

- `examples/echo_server.cpp`: a C++ snet server.
- `examples/echo_client.py`: a small interactive Python client.

## Run

In the first terminal:

```sh
cmake -S . -B build -DSIMPLENET_BUILD_EXAMPLES=ON
cmake --build build -j2
./build/examples/snet_echo_server
```

The server listens on `127.0.0.1:5555` and prints client connection, incoming data
and disconnection messages. Stop it with Ctrl+C (ordinary process termination).

In a second terminal:

```sh
python3 examples/echo_client.py
```

Type a message and press Enter. The client prints the echoed reply and waits for
another message using the same connection. `/quit`, Ctrl+D or Ctrl+C disconnect it.
The client limits each encoded message to 64 KiB including the newline, matching
this small request/reply example; network operations have a five-second timeout.

## Code flow

The server creates a non-blocking listener and transfers it to TcpServer.
`on_connection` prints the connection notice and installs the client handlers.
`on_data` prints the incoming bytes and sends them back. Only the accepted prefix
is consumed; if output is full, reading pauses and `on_output_available` retries
the retained suffix. EOF allows the remaining echo output to finish before closure.
`on_closed` prints the disconnection notice.

TCP is a byte stream: the server can print a message in multiple incoming chunks.
The Python client sends a newline with the text and receives exactly as many echo
bytes as it sent, so it does not assume that a single recv contains the full reply.

To build the server alone without test or documentation dependencies:

```sh
cmake -S . -B build-example -DBUILD_TESTING=OFF -DSIMPLENET_BUILD_EXAMPLES=ON -DSIMPLENET_BUILD_DOCS=OFF
cmake --build build-example -j2
```

With examples and testing enabled, two smoke tests check server notices/echo and
interactive-client replies. They serialize access to port 5555; stop a manually
running example before running these tests. Library IPv4/IPv6, partial I/O and
lifecycle behavior remains covered by the independent component tests.
