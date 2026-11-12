#!/usr/bin/env python3
import socket
import unittest
from unittest.mock import patch
from subprocess import CompletedProcess
from portcheck import require_idle


class PortCheckTest(unittest.TestCase):
    def test_listening_socket(self):
        with socket.socket() as server:
            server.bind(('127.0.0.1', 0))
            server.listen()
            with self.assertRaisesRegex(RuntimeError, 'already listening or connected'):
                require_idle([server.getsockname()[1]])

    def test_connected_endpoint_without_listener(self):
        with socket.socket() as server, socket.socket() as client:
            server.bind(('127.0.0.1', 0))
            server.listen()
            port = server.getsockname()[1]
            client.connect(('127.0.0.1', port))
            peer, _ = server.accept()
            with peer:
                server.close()
                with self.assertRaisesRegex(RuntimeError, 'already listening or connected'):
                    require_idle([port])

    def test_idle(self):
        with patch('portcheck.subprocess.run', return_value=CompletedProcess([], 1, '', '')):
            require_idle([12345])

    def test_inspection_error_fails_closed(self):
        with patch('portcheck.subprocess.run', return_value=CompletedProcess([], 1, '', 'denied')):
            with self.assertRaisesRegex(RuntimeError, 'inspection failed'):
                require_idle([12345])


if __name__ == '__main__':
    unittest.main()
