#include "socket_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#undef __STDC_FORMAT_MACROS


int main(void)
{
	/*
	muduo::net EventLoop loop;
	muduo::net::InetAddress listenAddr(8888);
	EchoServer server(&loop, listenAddr);
	server.start();
	loop.loop();
	*/


	struct socket_server* ss = socket_server_create();
	int listen_id = socket_server_listen(ss, 100, "", 8888, 32);
	socket_server_start(ss, 200, listen_id);

	// ÊÂ¼þÑ­»·
	struct socket_message result;
	for (;;) {
		int type = socket_server_poll(ss, &result, NULL);
		// DO NOT use any ctrl command (socket_server_close , etc. ) in this thread.
		switch (type) {
		case SOCKET_EXIT:
			goto EXIT_LOOP;
		case SOCKET_DATA:
			printf("message(%" PRIuPTR ") [id=%d] size=%d\n",result.opaque,result.id, result.ud);
			socket_server_send(ss, result.id, result.data, result.ud);
			//free(result.data);
			break;
		case SOCKET_CLOSE:
			printf("close(%" PRIuPTR ") [id=%d]\n",result.opaque,result.id);
			break;
		case SOCKET_OPEN:
			printf("open(%" PRIuPTR ") [id=%d] %s\n",result.opaque,result.id,result.data);
			break;
		case SOCKET_ERROR:
			printf("error(%" PRIuPTR ") [id=%d]\n",result.opaque,result.id);
			break;
		case SOCKET_ACCEPT:
			printf("accept(%" PRIuPTR ") [id=%d %s] from [%d]\n",result.opaque, result.ud, result.data, result.id);
			socket_server_start(ss, 300, result.ud);
			break;
		}
	}

EXIT_LOOP:
	socket_server_release(ss);
	return 0;
}
