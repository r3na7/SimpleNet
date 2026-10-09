"""Verify byte-for-byte echo with multiple slowly reading TCP peers."""
import argparse
import errno
import ipaddress
import selectors
import socket
import sys
import time


def make_payload(index):
    return (str(index).encode('ascii') + b'\0' + bytes(range(256)) * 1024)[:256 * 1024]


def number(value, maximum=None):
    if not value or not value.isascii() or not value.isdecimal():
        raise argparse.ArgumentTypeError('expected a positive decimal integer')
    try:
        result = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from error
    if result < 1 or (maximum is not None and result > maximum):
        raise argparse.ArgumentTypeError('integer outside allowed range')
    return result


def numeric_host(value):
    try:
        if '%' in value:
            raise ValueError('zone IDs are unsupported')
        return str(ipaddress.ip_address(value))
    except ValueError as error:
        raise argparse.ArgumentTypeError('expected a numeric IPv4/IPv6 address') from error


class Peer:
    def __init__(self, sock, payload, connected, next_read):
        self.socket = sock
        self.payload = payload
        self.connected = connected
        self.written = self.received = 0
        self.next_read = next_read
        self.registered = 0
        self.eof = False


def run_clients(host, port, clients):
    started = time.monotonic()
    deadline = started + 15
    family = socket.AF_INET6 if ':' in host else socket.AF_INET
    peers = []
    with selectors.DefaultSelector() as selector:
        try:
            for index in range(clients):
                if time.monotonic() >= deadline:
                    raise TimeoutError('Overall client deadline (15s) exceeded during setup')
                sock = socket.socket(family, socket.SOCK_STREAM)
                state = Peer(sock, b'', False, started + 0.250)
                peers.append(state) # Close even if preparation/connect fails.
                state.payload = make_payload(index)
                sock.setblocking(False)
                result = sock.connect_ex((host, port))
                if result not in (0, errno.EINPROGRESS, errno.EWOULDBLOCK, errno.EALREADY, errno.EINTR):
                    raise OSError(result, 'connect failed')
                state.connected = result == 0
            finished = 0
            while finished != clients:
                now = time.monotonic()
                if now >= deadline:
                    raise TimeoutError('Overall client deadline (15s) exceeded')
                wait = deadline - now
                for state in peers:
                    if state.eof:
                        continue
                    mask = selectors.EVENT_WRITE if not state.connected or state.written < len(state.payload) else 0
                    if state.connected:
                        if now >= state.next_read:
                            mask |= selectors.EVENT_READ
                        else:
                            wait = min(wait, state.next_read - now)
                    if mask != state.registered:
                        if state.registered:
                            selector.unregister(state.socket)
                        if mask:
                            selector.register(state.socket, mask, state)
                        state.registered = mask
                for key, events in selector.select(wait):
                    state = key.data
                    if not state.connected:
                        error = state.socket.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
                        if error:
                            raise OSError(error, 'connect failed')
                        state.connected = True
                    if events & selectors.EVENT_WRITE and state.written < len(state.payload):
                        try:
                            sent = state.socket.send(memoryview(state.payload)[state.written:])
                        except BlockingIOError:
                            sent = None
                        if sent == 0:
                            raise RuntimeError('socket accepted zero bytes')
                        if sent is not None:
                            state.written += sent
                            if state.written == len(state.payload):
                                state.socket.shutdown(socket.SHUT_WR)
                    if events & selectors.EVENT_READ:
                        try:
                            data = state.socket.recv(1024)
                        except BlockingIOError:
                            continue
                        if not data:
                            if state.received != len(state.payload) or state.written != len(state.payload):
                                raise RuntimeError('early EOF before complete echo')
                            selector.unregister(state.socket)
                            state.registered = 0
                            state.socket.close()
                            state.eof = True
                            finished += 1
                        else:
                            expected = state.payload[state.received:state.received + len(data)]
                            if data != expected:
                                raise RuntimeError('echo bytes differ or exceed payload')
                            state.received += len(data)
                            state.next_read = time.monotonic() + 0.005
        finally:
            for state in peers:
                state.socket.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', type=numeric_host, default='127.0.0.1')
    parser.add_argument('--port', type=lambda v: number(v, 65535), default=5555)
    parser.add_argument('--clients', type=number, default=2)
    args = parser.parse_args(argv)
    try:
        run_clients(args.host, args.port, args.clients)
    except (OSError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        return 1
    print('Verified {} clients: 262144 echo bytes each and EOF'.format(args.clients))
    return 0


if __name__ == '__main__':
    sys.exit(main())
