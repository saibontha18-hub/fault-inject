CC      = gcc
CFLAGS  = -Wall -Wextra -Werror -std=c11 -pedantic -O2
INCDIR  = src

SRC     = src/fi.c
DEMO_SRC = demo/reasm.c demo/safety_demo.c

all: safety_demo

safety_demo: $(SRC) $(DEMO_SRC)
	$(CC) $(CFLAGS) -I$(INCDIR) -o $@ $(SRC) $(DEMO_SRC)

test: safety_demo
	./safety_demo

clean:
	rm -f safety_demo

.PHONY: all test clean
