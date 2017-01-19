ALL=socket-server echosrv echocli
all:$(ALL)

socket-server : socket_server.c test.c
	gcc -g -Wall -o $@ $^ -lpthread

echosrv : echosrv.c socket_server.c
	gcc -g -Wall -o $@ $^

echocli : echocli.c socket_server.c
	gcc -g -Wall -o $@ $^ -lpthread

clean:
	rm -f $(ALL)
