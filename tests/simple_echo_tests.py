"""Smoke-test the human-facing example and its interactive client."""
import argparse
import contextlib
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

_deferred = False
_cancelling = False
_pending = None


def cancel(signum, frame):
    global _pending, _cancelling
    if _deferred or _cancelling:
        _pending = signum
    else:
        _cancelling = True
        raise SystemExit(128 + signum)


@contextlib.contextmanager
def child(command, **kwargs):
    global _deferred, _pending, _cancelling
    process = None
    try:
        _deferred = True
        try:
            process = subprocess.Popen(command, **kwargs)
        finally:
            _deferred = False
        if _pending:
            cancel(_pending, None)
        yield process
    finally:
        _deferred = True
        try:
            if process is not None:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                process.wait(timeout=5)
                for stream in (process.stdin, process.stdout, process.stderr):
                    if stream is not None:
                        stream.close()
        finally:
            _deferred = False
        if _pending and not _cancelling:
            cancel(_pending, None)


def wait_for(predicate):
    deadline = time.monotonic() + 5
    while not predicate():
        assert time.monotonic() < deadline, 'example did not produce expected result'
        time.sleep(0.005)


def smoke(args):
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory)/'server.txt'
        errors = Path(directory)/'errors.txt'
        with output.open('wb') as out, errors.open('wb') as err:
            with child([args.server], stdout=out, stderr=err) as server:
                def log():
                    assert server.poll() is None, errors.read_text(errors='replace')
                    return output.read_text(errors='replace')
                wait_for(lambda: 'Сервер слушает 127.0.0.1:5555' in log())
                if args.case == 'client':
                    with child([sys.executable, '-B', args.client], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as client:
                        reply, diagnostic = client.communicate('Привет, snet!\nЕщё одно сообщение\n\n/quit\n', timeout=5)
                        assert client.returncode == 0, diagnostic
                        assert 'Ответ: Привет, snet!' in reply and 'Ответ: Ещё одно сообщение' in reply, reply
                        assert reply.count('Ответ:') == 3, reply
                    wait_for(lambda: 'Клиент отключился' in log())
                    assert 'Привет, snet!' in log() and 'Ещё одно сообщение' in log(), log()
                else:
                    for payload in (b'hello\0snet\n', 'Второй клиент\n'.encode()):
                        with socket.create_connection(('127.0.0.1', 5555), timeout=5) as peer:
                            peer.sendall(payload)
                            peer.shutdown(socket.SHUT_WR)
                            reply = bytearray()
                            while True:
                                data = peer.recv(4096)
                                if not data:
                                    break
                                reply.extend(data)
                            assert reply == payload
                    wait_for(lambda: log().count('Клиент отключился') == 2)
                    assert log().count('Клиент подключился') == 2, log()
                    assert 'hello\0snet' in log() and 'Второй клиент' in log(), log()
                assert not errors.read_text(), errors.read_text()
    print(args.case+': PASS')


if __name__ == '__main__':
    signal.signal(signal.SIGINT, cancel)
    signal.signal(signal.SIGTERM, cancel)
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', required=True)
    parser.add_argument('--client', required=True)
    parser.add_argument('--case', choices=('server', 'client'), required=True)
    smoke(parser.parse_args())
