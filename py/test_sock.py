from py_utils.test import Parameters, parametrize
from sock import Client

parameters = [
    Parameters("empty", b""),
    Parameters("multi", b"\x12\x34"),
]


@parametrize("data", parameters)
def test_sock(data):
    c = Client()
    c.open()
    c.connect()
    c.send(data)
    assert c.receive() == data
    c.close()
