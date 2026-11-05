# make           build the logger, the fake node and the course exercise
# make test      unit tests and the integration test (fake node on a pseudo-terminal)
CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer

all: build/logger build/fake_node build/coder

build:
	mkdir -p build

build/logger: src/logger.c src/writer.c src/frame.c src/*.h | build
	$(CC) $(CFLAGS) src/logger.c src/writer.c src/frame.c -o $@

build/fake_node: tools/fake_node.c src/frame.c src/frame.h | build
	$(CC) $(CFLAGS) tools/fake_node.c src/frame.c -o $@ -lm

build/test_frame: tests/test_frame.c src/frame.c src/frame.h | build
	$(CC) $(CFLAGS) $(SAN) tests/test_frame.c src/frame.c -o $@

build/coder: exercises/coder/coder.c exercises/coder/secret.h | build
	$(CC) $(CFLAGS) exercises/coder/coder.c -o $@

test: all build/test_frame
	./build/test_frame
	python3 -m unittest discover -s tests -p "test_*.py" -v

clean:
	rm -rf build

.PHONY: all test clean
