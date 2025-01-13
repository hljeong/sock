from py_utils.context import Resource

import socket
import struct

# todo: fix error handling

KiB = 1024


class Message:
    MAGIC = 0x94_84_86_95
    MAX_LEN = 8 * KiB
    HEADER_LEN = 8
    MAX_DATA_LEN = MAX_LEN - HEADER_LEN


class Client(Resource):
    class ServerClosedError(Exception):
        pass

    class BadDataReceivedError(Exception):
        pass

    def __init__(self, port=3727):
        super().__init__()
        self.socket = None
        self.port = port

    def acquire(self):
        # todo: [0] these are supposed to be typecheck-only asserts, make them so
        assert self.socket is None
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.socket.connect(("", self.port))
        return self

    def release(self):
        # todo: see [0]
        assert self.socket is not None
        self.socket.close()
        self.socket = None

    def send(self, data):
        self.ensure_open()
        # todo: see [0]
        assert self.socket is not None

        if not isinstance(data, bytes):
            raise ValueError(f"data must be of type bytes: {data!r}")

        if len(data) > Message.MAX_DATA_LEN:
            raise ValueError(
                f"data too long: length {len(data)}, max {Message.MAX_DATA_LEN}"
            )

        data = struct.pack("<I", Message.MAGIC) + struct.pack("<I", len(data)) + data
        self.socket.sendall(data)

    def _recv(self, n):
        self.ensure_open()
        # todo: see [0]
        assert self.socket is not None

        data = bytes()
        while len(data) < n:
            recv_data = self.socket.recv(n - len(data))
            if len(recv_data) == 0:
                raise Client.ServerClosedError("connection closed")
            data += recv_data

        return data

    def receive(self):
        self.ensure_open()
        # todo: see [0]
        assert self.socket is not None

        # todo: dont assume channel integrity and correlate?
        # todo: magic number
        magic_raw = self._recv(4)

        # todo: [2] dont raise error if correlating
        magic = int.from_bytes(magic_raw, "little")
        if magic != Message.MAGIC:
            raise Client.BadDataReceivedError(
                f"bad magic: 0x{magic:08x}, expected 0x{Message.MAGIC:08x}"
            )

        # todo: see [2]
        len_raw = self._recv(4)

        # todo: see [2]
        len = int.from_bytes(len_raw, "little")
        if len > Message.MAX_DATA_LEN:
            raise Client.BadDataReceivedError(
                f"bad data length: {len}, max {Message.MAX_DATA_LEN}"
            )

        # todo: see [2]
        return self._recv(len)

    def stop_server(self):
        self.send(b"")
