from pytest import raises

from py_utils.test import Parameters, parametrize
from sock import TCPClient

parameters = [
    Parameters("one byte", b"\x12"),
    Parameters("two bytes", b"\x34\x56"),
]


@parametrize("data", parameters)
def test_sock(data):
    with TCPClient() as c:
        c.send(data)

        assert c.receive() == data


def test_sock_close():
    with TCPClient() as c:
        c.send(b"")

        with raises(TCPClient.ServerClosedError):
            c.receive()
