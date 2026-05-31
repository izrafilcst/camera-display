# Host-side tests for components/ble_pair (state machine only — NimBLE stack
# is not testable on the host). Compiles ble_pair_state.cpp + the test
# harness against a stub implementation.
#
# Until Wave 2 (Coder) delivers ble_pair_state.cpp, this make fails to link.

SRCS     = test_ble_pair_state.cpp ../ble_pair_state.cpp
BIN      = /tmp/test_ble_pair_state_host
CXXFLAGS = -std=c++17 -Wall -Wextra -Werror \
           -I../include \
           -I../include/internal \
           -DBLE_PAIR_HOST_BUILD=1

.PHONY: test clean

test:
	g++ $(CXXFLAGS) $(SRCS) -o $(BIN) && $(BIN)

clean:
	rm -f $(BIN)
