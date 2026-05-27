SRCS = test_patterns.cpp ../test_patterns.cpp
BIN  = /tmp/test_patterns_host
CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -I../include

.PHONY: test clean

test:
	g++ $(CXXFLAGS) $(SRCS) -o $(BIN) && $(BIN)

clean:
	rm -f $(BIN)
