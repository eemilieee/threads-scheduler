CC = gcc
CFLAGS = -fPIC -Wall -g

.PHONY: build
build: libscheduler.so

libscheduler.so: so_scheduler.o
		$(CC) -shared -o $@ $^

so_scheduler.o: so_scheduler.c
		$(CC) $(CFLAGS) -o $@ -c $<

.PHONY: clean
clean:
		rm -f libscheduler.so
