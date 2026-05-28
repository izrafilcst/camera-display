# Host-side tests for components/espnow_link — REQ-5 peer MAC checks
# Self-contained: no ESP-IDF headers required.

SRCS     = test_peer_mac.cpp
BIN      = /tmp/test_peer_mac_host
CXXFLAGS = -std=c++17 -Wall -Wextra -Werror \
           -I../include \
           -DESPNOW_LINK_HOST_BUILD=1

.PHONY: test clean

test:
	g++ $(CXXFLAGS) $(SRCS) -o $(BIN) && $(BIN)

clean:
	rm -f $(BIN)
