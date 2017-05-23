PORT=50553
CFLAGS = -DPORT=$(PORT) -g -Wall -std=gnu99
DEPENDENCIES = hash.h ftree.h

all: rcopy_server rcopy_client

rcopy_server: rcopy_server.o
	gcc ${CFLAGS} -o $@ $^

rcopy_client: rcopy_client.o
	gcc ${CFLAGS} -o $@ $^

hash_functions: hash_functions.o
	gcc ${CFLAGS} -o $@ $^

%.o: %.c
	gcc ${CFLAGS} -c $<

clean:
	rm -f *.o rcopy_server rcopy_client hash
