# Host-side tests for components/pair_nvs
# These tests link against pair_nvs.cpp compiled with PAIR_NVS_HOST_BUILD=1,
# which replaces ESP-IDF nvs_* calls with an in-memory shim.
#
# Running 'make test' will FAIL (link error) until Wave 2 (Coder) provides:
#   1. components/pair_nvs/pair_nvs.cpp  (with #ifdef PAIR_NVS_HOST_BUILD shim)
#   2. pair_nvs_is_valid_mac_for_persist implemented correctly
#
# Once implemented, all 13 tests must pass.

SRCS     = test_pair_nvs_logic.cpp ../pair_nvs.cpp
BIN      = /tmp/test_pair_nvs_host
CXXFLAGS = -std=c++17 -Wall -Wextra -Werror \
           -I../include \
           -DPAIR_NVS_HOST_BUILD=1

.PHONY: test clean

test:
	g++ $(CXXFLAGS) $(SRCS) -o $(BIN) && $(BIN)

clean:
	rm -f $(BIN)
