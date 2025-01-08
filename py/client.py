import socket
import struct

KiB = 1024


class Message:
    MAGIC = 0x94_84_86_95
    MAX_LEN = 4 * KiB


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
            self.socket.connect(("localhost", self.port))
        except socket.error:
            self.close()
            return False
        return True

    def send(self, data):
        if self.socket is None:
            return False

        if not isinstance(data, bytes):
            return False

        if len(data) > Message.MAX_LEN - 8:
            return False

        data = (
            struct.pack("<I", Message.MAGIC) + struct.pack("<I", len(data) + 8) + data
        )
        try:
            self.socket.sendall(data)
        except socket.error:
            self.close()
            return False
        return True


def main():
    c = Client()
    c.open()
    c.connect()
    c.send(b"\x12\x34")
    c.close()


if __name__ == "__main__":
    main()
