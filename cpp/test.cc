#include <cassert>
#include <memory>

#include "../lib/cpp_utils/srv/srv.h"
#include "sock.h"

using namespace sock;
using namespace srv;
using namespace std;
using namespace std::chrono;
using TestData = vector<uint8_t>;

vector<TestData> all_test_data = {
    {0x12},
    {0x34, 0x56},
};

size_t test_data_idx = 0;

void callback(const uint8_t *data, uint32_t len);

AutoDispatch s(make_unique<TCPCallbackServer>(3727, callback), 1s);

void callback(const uint8_t *data, uint32_t len) {
  if (len == 0) {
    s.signal_stop();
    return;
  }

  s->send(data, len);

  assert(test_data_idx < all_test_data.size());

  const TestData &test_data = all_test_data[test_data_idx++];
  assert(len == test_data.size());

  for (size_t i = 0; i < test_data.size(); ++i) {
    assert(data[i] == test_data[i]);
  }
}

int main() {
  s.join();
  // assert(test_data_idx == all_test_data.size());
}
