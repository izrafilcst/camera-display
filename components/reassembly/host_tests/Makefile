# Host-side tests for components/reassembly
# These tests link against a stub reassembly.cpp that is NOT yet implemented.
# Running 'make test' will FAIL (undefined references) until Wave 2 (Coder) provides
# the implementation — that is the intended RED state for Sprint 2 Wave 1.
#
# Once the coder delivers reassembly.cpp, this Makefile should produce:
#   All N tests passed.

SRCS     = test_reassembly.cpp ../reassembly.cpp
BIN      = /tmp/test_reassembly_host
CXXFLAGS = -std=c++17 -Wall -Wextra -Werror \
           -I../include \
           -I../../espnow_link/include \
           -DREASSEMBLY_HOST_BUILD=1

.PHONY: test clean

test:
	g++ $(CXXFLAGS) $(SRCS) -o $(BIN) && $(BIN)

clean:
	rm -f $(BIN)
