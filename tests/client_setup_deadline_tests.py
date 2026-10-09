"""Virtual-clock deadline checks with real, tracked TCP socket ownership."""
import importlib.util
from pathlib import Path
import socket
import sys
import unittest
from unittest import mock
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('slow_client', Path(__file__).resolve().parents[1]/'examples/slow_echo_client.py')
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)


class SetupDeadlineTest(unittest.TestCase):
    def expired_setup(self, times, expected_created):
        created = []
        real_socket = socket.socket
        def track(*args, **kwargs):
            value = real_socket(*args, **kwargs)
            created.append(value)
            return value
        with real_socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            with mock.patch.object(client.time, 'monotonic', side_effect=times), \
                 mock.patch.object(client.socket, 'socket', side_effect=track):
                with self.assertRaises(TimeoutError):
                    client.run_clients('127.0.0.1', listener.getsockname()[1], 2)
        self.assertEqual(len(created), expected_created)
        self.assertTrue(all(value.fileno() == -1 for value in created))

    def test_expiration_before_first_peer_acquires_nothing(self):
        self.expired_setup([0, 16, 16], 0)

    def test_expiration_between_peers_closes_first_peer(self):
        self.expired_setup([0, 0, 16], 1)


if __name__ == '__main__':
    unittest.main()
