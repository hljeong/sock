from client import Client

all_test_data = [
    b"",
    b"\x12\x34",
]


def main():
    c = Client()
    c.open()
    c.connect()
    for test_data in all_test_data:
        c.send(test_data)
        assert c.receive() == test_data
    c.close()


if __name__ == "__main__":
    main()
