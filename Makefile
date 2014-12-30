.PHONY: all

all:
	clang -g -O3 -Wall -Wextra -Werror hijack.c -lutil -o hijack
