# FlexQL Makefile
# ────────────────────────────────────────────────────────────
# Outputs:
#   bin/flexql-server  —  database engine + TCP server
#   bin/flexql-client  —  REPL + C API (no server code)
#
# Usage:
#   make              build both binaries
#   make clean        remove build artefacts
# ────────────────────────────────────────────────────────────

CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -I include
LDFLAGS  = -lpthread

SRC_COMMON = src/parser/lexer.cpp \
             src/parser/parser.cpp \
             src/cache/lru_cache.cpp \
             src/storage/storage.cpp \
             src/query/engine.cpp

SRC_API    = src/client/flexql_api.cpp

.PHONY: all clean test

all: bin/flexql-server bin/flexql-client

bin/flexql-server: $(SRC_COMMON) src/server/server.cpp
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "  Built: $@"

bin/flexql-client: $(SRC_API) src/client/client.cpp
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "  Built: $@"

bin/test_all: tests/test_all.cpp $(SRC_API)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "  Built: $@"

bin/benchmark_flexql: tests/benchmark_flexql.cpp $(SRC_API)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "  Built: $@"

# Build the official benchmark binary
benchmark: bin/benchmark_flexql

# Run the full test suite (starts a temporary server on port 9099)
test: bin/flexql-server bin/test_all
	@echo "Starting test server on port 9099..."
	@./bin/flexql-server 9099 & echo $$! > /tmp/flexql_test_server.pid; \
	sleep 0.5; \
	./bin/test_all 127.0.0.1 9099; \
	RESULT=$$?; \
	kill $$(cat /tmp/flexql_test_server.pid) 2>/dev/null; \
	rm -f /tmp/flexql_test_server.pid; \
	exit $$RESULT

clean:
	rm -rf bin build
