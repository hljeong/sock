from py_utils.test import Parameters, parametrize
from sock import Client

import socket

parameters = [
    Parameters("one byte", b"\x12"),
    Parameters("two bytes", b"\x34\x56"),
]


@parametrize("data", parameters)
def test_sock(data):
    c = Client()

    # todo: make context
    try:
        c.open()
        c.connect()
        c.send(data)

        assert c.receive() == data

    finally:
        c.close()


def test_sock_close():
    c = Client()

    try:
        c.open()
        c.connect()
        c.send(b"")

        try:
            c.receive()
        except Client.ServerClosedError:
            # expect server closed
            return

        assert False, "server is expected to be closed"

    finally:
        c.close()
