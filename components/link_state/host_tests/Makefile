SRCS = test_link_state.cpp ../link_state.cpp
BIN  = /tmp/test_link_state_host
CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -I../include

.PHONY: test clean

test:
	g++ $(CXXFLAGS) $(SRCS) -o $(BIN) && $(BIN)

clean:
	rm -f $(BIN)
