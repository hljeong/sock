#include <cassert>

#include "sock.h"

using namespace sock;

using TestData = std::vector<uint8_t>;

std::vector<TestData> all_test_data = {
    {0x12},
    {0x34, 0x56},
};

std::size_t test_data_idx = 0;

std::unique_ptr<Server> s;

void callback(const uint8_t *data, const uint32_t len) {
  s->send(data, len);

  assert(test_data_idx < all_test_data.size());

  const TestData &test_data = all_test_data[test_data_idx++];
  assert(len == test_data.size());

  for (std::size_t i = 0; i < test_data.size(); ++i) {
    assert(data[i] == test_data[i]);
  }
}

int main() {
  using namespace std::chrono;
  using namespace std::this_thread;

  s = std::make_unique<Server>(callback);

  s->start(5s);
  s->wait_for_stop();
  s->close();

  assert(test_data_idx == all_test_data.size());
}
