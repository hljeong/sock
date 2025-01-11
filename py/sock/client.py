import socket
import struct

# todo: fix error handling

KiB = 1024


class Message:
    MAGIC = 0x94_84_86_95
    MAX_LEN = 8 * KiB


class Client:
    def __init__(self, port=3727):
        self.socket = None
        self.port = port

    def open(self):
        if self.socket:
            return False

        self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        return True

    def close(self):
        if self.socket is None:
            return False

        self.socket.close()
        self.socket = None
        return True

    def connect(self):
        if self.socket is None:
            return False

        try:
            self.socket.connect(("", self.port))
        except socket.error:
            self.close()
            raise

        return True

    def send(self, data):
        if self.socket is None:
            return False

        if not isinstance(data, bytes):
            raise ValueError()

        if len(data) > Message.MAX_LEN - 8:
            return False

        data = (
            struct.pack("<I", Message.MAGIC) + struct.pack("<I", len(data) + 8) + data
        )
        try:
            self.socket.sendall(data)
        except socket.error:
            self.close()
            raise

        return True

    def _recv(self, n):
        if self.socket is None:
            raise RuntimeError("socket closed")

        data = bytes()
        while len(data) < n:
            recv_data = self.socket.recv(n - len(data))
            if len(recv_data) == 0:
                raise RuntimeError("connection closed")
            data += recv_data

        return data

    def receive(self):
        # todo: dont assume channel integrity and correlate?
        magic_raw = self._recv(4)
        if magic_raw is None:
            self.close()
            raise RuntimeError("could not recv magic")

        # todo: dont raise error if correlating
        magic = int.from_bytes(magic_raw, "little")
        if magic != Message.MAGIC:
            self.close()
            raise RuntimeError(
                f"bad magic: 0x{magic:08x}, expected 0x{Message.MAGIC:08x}"
            )

        # todo: dont raise error if correlating
        len_raw = self._recv(4)
        if len_raw is None:
            self.close()
            raise RuntimeError("could not recv len")

        # todo: dont raise error if correlating
        len = int.from_bytes(len_raw, "little")
        if len > Message.MAX_LEN:
            self.close()
            raise RuntimeError(f"bad len: {len}, max {Message.MAX_LEN}")

        # todo: dont raise error if correlating
        data_len = len - 8
        data = self._recv(data_len)
        if data is None:
            self.close()
            raise RuntimeError("could not recv data")

        return data
