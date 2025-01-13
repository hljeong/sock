from pytest import raises

from py_utils.test import Parameters, parametrize
from sock import Client

parameters = [
    Parameters("one byte", b"\x12"),
    Parameters("two bytes", b"\x34\x56"),
]


@parametrize("data", parameters)
def test_sock(data):
    with Client() as c:
        c.send(data)

        assert c.receive() == data


def test_sock_close():
    with Client() as c:
        c.stop_server()

        with raises(Client.ServerClosedError):
            c.receive()
