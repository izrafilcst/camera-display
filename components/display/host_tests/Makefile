SRCS = test_patterns.cpp ../test_patterns.cpp
BIN  = /tmp/test_patterns_host

.PHONY: test clean

test:
	g++ -std=c++17 -Wall -Wextra -Werror $(SRCS) -o $(BIN) && $(BIN)

clean:
	rm -f $(BIN)
