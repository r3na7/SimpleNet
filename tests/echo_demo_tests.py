"""Executable-boundary checks; all subprocesses and peers have bounded cleanup."""
import argparse
import contextlib
import os
import select
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time


def run_server(server, *args):
    return subprocess.run([server, *args], capture_output=True, text=True, timeout=5)


@contextlib.contextmanager
def running_server(server, host):
    with tempfile.TemporaryFile() as errors:
        process = subprocess.Popen([server, '--host', host, '--port', '0'],
                                   stdout=subprocess.PIPE, stderr=errors)
        try:
            data = b''
            deadline = time.monotonic() + 5
            while b'\n' not in data:
                remaining = deadline - time.monotonic()
                assert remaining > 0, 'server never became ready'
                assert select.select([process.stdout], [], [], remaining)[0], 'READY timeout'
                chunk = os.read(process.stdout.fileno(), 4096)
                assert chunk, 'server exited before READY'
                data += chunk
            fields = data.decode().strip().split()
            assert len(fields) == 3 and fields[:2] == ['READY', host], data
            port = int(fields[2])
            assert 0 < port <= 65535
            yield process, port
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
            process.wait(timeout=5)
            process.stdout.close()
            errors.seek(0)
            diagnostic = errors.read().decode(errors='replace')
            assert process.returncode == 0, (process.returncode, diagnostic)
            assert 'Sanitizer' not in diagnostic, diagnostic


def peer(host, port):
    family = socket.AF_INET6 if ':' in host else socket.AF_INET
    sock = socket.socket(family, socket.SOCK_STREAM)
    try:
        sock.settimeout(5)
        sock.connect((host, port))
        return sock
    except BaseException:
        sock.close()
        raise


def roundtrip(host, port, payload):
    with peer(host, port) as sock:
        failures = []
        def send():
            try:
                sock.sendall(payload)
                sock.shutdown(socket.SHUT_WR)
            except OSError as error:
                failures.append(error)
        sender = threading.Thread(target=send)
        sender.start()
        reply = bytearray()
        try:
            while True:
                data = sock.recv(1024)
                if not data:
                    break
                reply.extend(data)
                time.sleep(0.001)
        finally:
            sock.close()
            sender.join(timeout=5)
        assert not sender.is_alive(), 'sender stuck'
        assert not failures, failures
        assert reply == payload, (len(reply), len(payload))


def case_help(args):
    result = run_server(args.server, '--help')
    assert result.returncode == 0 and '--host' in result.stdout and '--port' in result.stdout
    assert 'READY ' not in result.stdout


def case_invalid_cli(args):
    for flags in [('--port', '-1'), ('--port', '65536'), ('--port', '12x'),
                  ('--port', ''), ('--port',), ('--port', '9999999999999999999999'),
                  ('--host', 'not-an-ip'), ('--host', '::1%lo'), ('--host',),
                  ('--unknown',), ('--port', '1', '--port', '2'),
                  ('--host', '127.0.0.1', '--host', '::1')]:
        result = run_server(args.server, *flags)
        assert result.returncode == 2 and result.stderr and 'READY ' not in result.stdout, (flags, result)


def case_occupied_address(args):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as occupied:
        occupied.bind(('127.0.0.1', 0))
        occupied.listen()
        result = run_server(args.server, '--port', str(occupied.getsockname()[1]))
        assert result.returncode == 1 and result.stderr and 'READY ' not in result.stdout, result


def echo_family(args, host):
    with running_server(args.server, host) as (process, port):
        roundtrip(host, port, bytes(range(256)) * 1024)
        with peer(host, port) as reset:
            reset.sendall(b'reset')
            reset.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        roundtrip(host, port, b'after-reset\0ok')
        assert process.poll() is None


def signal_case(args, selected, active=False):
    with running_server(args.server, '127.0.0.1') as (process, port):
        with contextlib.ExitStack() as peers:
            if active:
                sock = peers.enter_context(peer('127.0.0.1', port))
                sock.sendall(bytes(range(256)) * 256)
            process.send_signal(selected)
            assert process.wait(timeout=5) == 0


CASES = {
    'help': case_help,
    'invalid_cli': case_invalid_cli,
    'occupied_address': case_occupied_address,
    'startup_cleanup': case_occupied_address,
    'echo_ipv4': lambda a: echo_family(a, '127.0.0.1'),
    'echo_ipv6': lambda a: echo_family(a, '::1'),
    'signal_int': lambda a: signal_case(a, signal.SIGINT),
    'signal_term': lambda a: signal_case(a, signal.SIGTERM),
    'signal_active': lambda a: signal_case(a, signal.SIGTERM, True),
}

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', required=True)
    parser.add_argument('--case', required=True, choices=CASES)
    args = parser.parse_args()
    CASES[args.case](args)
    print(args.case + ': PASS')
