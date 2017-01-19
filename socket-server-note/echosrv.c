#include "socket_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#undef __STDC_FORMAT_MACROS

#define OPAQUE_LISTEN		100
#define OPAQUE_SERVER		200
#define OPAQUE_CLIENT		300

int main(int argc, char *argv)
{
	struct socket_server *ss = socket_server_create();
	int listen_id = socket_server_listen(ss, OPAQUE_LISTEN, "", 8888, 32);
	socket_server_start(ss, OPAQUE_SERVER, listen_id);
	// 运行到这里，管道中有2个消息，'L'和'S'，而epoll对象有1个关注的句柄
	// 另外一个句柄socket(就是上面8888端口对应的socket)会在下面socket_server_poll()里面收到消息后，epoll再关注socket句柄

	// 事件循环
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
			socket_server_start(ss, OPAQUE_CLIENT, result.ud);
			break;
		}
	}

EXIT_LOOP:
	socket_server_release(ss);
	return 0;
}

