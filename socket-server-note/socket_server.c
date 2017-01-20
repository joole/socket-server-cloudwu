#include "socket_server.h"
#include "socket_poll.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#define MAX_INFO		128

#define MAX_SOCKET_P		16		// MAX_SOCKET will be 2^MAX_SOCKET_P
#define MAX_EVENT		64		// 用于epoll_wait的第三个参数，即每次epoll返回的最多事件数
#define MIN_READ_BUFFER		64		// read最小分配的缓冲区大小

// ioctl()/FIONREAD
// readv
#define SOCKET_TYPE_INVALID	0		// 无效的套接字
#define SOCKET_TYPE_RESERVE	1		// 预留，已被申请，即将投入使用
#define SOCKET_TYPE_PLISTEN	2		// 监听套接字，未加入epoll管理
#define SOCKET_TYPE_LISTEN	3		// 监听套接字，已加入epoll管理
#define SOCKET_TYPE_CONNECTING	4		// 尝试连接中的套接字
#define SOCKET_TYPE_CONNECTED	5		// 已连接套接，主动或被动(connect,accept成功，并已加入epoll管理)
#define SOCKET_TYPE_HALFCLOSE	6		// 应用层已发起关闭套接字请求，应用层发送缓冲区尚未发送完，未调用close
#define SOCKET_TYPE_PACCEPT	7		// accept返回的已连接套接字，但未加入epoll管理
#define SOCKET_TYPE_BIND	8		// 其它类型的文件描述符，比如stdin,stdout等

#define MAX_SOCKET		(1 << MAX_SOCKET_P)	// 最多支持64K个socket

struct write_buffer {
	struct write_buffer	*next;
	char			*ptr;		// 指向当前块未发送缓冲区首字节
	int			sz;		// 当前块未发送的字节数
	void			*buffer;	// 发送缓冲区
};

// 应用层对socket的抽象
struct socket {
	int			fd;		// 文件描述符
	int			id;		// 应用层维护的一个与fd相对应的id
	int			type;		// socket类型(或状态)
	int			size;		// 下一次read操作要分配的缓冲区大小
	int64_t			wb_size;	// 发送缓冲区中未发送的字节数
	uintptr_t		opaque;		// 在skynet中用于保存服务handle
	struct write_buffer	*head;		// 发送缓冲区链表头指针
	struct write_buffer	*tail;		// 发送缓冲区链表尾指针
};

struct socket_server {
	int			recvctrl_fd;	// 管道读端，用于接受控制命令
	int			sendctrl_fd;	// 管道写端，用于发送控制命令
	int			checkctrl;	// 是否检查控制命令
	poll_fd			event_fd;	// epoll fd
	int			alloc_id;	// 用于分配id
	int			event_n;	// epoll_wait返回的事件个数
	int			event_index;	// 当前处理的事件序号，从0开始
	struct event		ev[MAX_EVENT];		// 用于epoll_wait
	struct socket		slot[MAX_SOCKET];	// 应用层预先分配的socket数组(即socket池)
	char			buffer[MAX_INFO];	// 临时数据，比如保存新建连接的对等端的地址信息
	fd_set			rfds;		// 用于select
};

// 以下6个结构体是控制命令数据包包体结构
struct request_open {
	int id;
	int port;
	uintptr_t opaque;
	char host[1];
};

struct request_send {
	int id;
	int sz;
	char * buffer;
};

struct request_close {
	int id;
	uintptr_t opaque;
};

struct request_listen {
	int id;
	int fd;
	uintptr_t opaque;
	char host[1];
};

struct request_bind {
	int id;
	int fd;
	uintptr_t opaque;
};

struct request_start {
	int id;
	uintptr_t opaque;
};

// 控制命令请求包
struct request_package {
	uint8_t	header[8];			// 6 bytes dummy，header[0]~header[5]未使用,header[6] for type,header[7] for len，len是指包体长度
	union {
		char buffer[256];
		struct request_open	open;	// socket_server_connect
		struct request_send	send;	// socket_server_send
		struct request_close	close;	// socket_server_close
		struct request_listen	listen;	// socket_server_listen
		struct request_bind	bind;	// socket_server_bind
		struct request_start	start;	// socket_server_start
	} u;
	uint8_t dummy[256];
};

// 网际IP地址
union sockaddr_all {
	struct sockaddr		s;
	struct sockaddr_in	v4;
	struct sockaddr_in6	v6;
};

#define MALLOC	malloc
#define FREE	free

static void
socket_keepalive(int fd) {
	int keepalive = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepalive , sizeof(keepalive));  
}

// 从socket池中获取一个空的socket，并为其分配一个id(0~2147483647即2^31-1)
static int
reserve_id(struct socket_server *ss) {
	int i;
	for (i=0;i<MAX_SOCKET;i++) {
		int id = __sync_add_and_fetch(&(ss->alloc_id), 1);
		if (id < 0) {
			// 此时id = 0x80000000即-2147483648
			id = __sync_and_and_fetch(&(ss->alloc_id), 0x7fffffff);		// id = 0
		}
		struct socket *s = &ss->slot[id % MAX_SOCKET];
		if (s->type == SOCKET_TYPE_INVALID) {
			if (__sync_bool_compare_and_swap(&s->type, SOCKET_TYPE_INVALID, SOCKET_TYPE_RESERVE)) {
				return id;
			} else {
				// retry
				--i;
			}
		}
	}
	return -1;
}

// 创建一个socket_server对象: 里面包含epoll句柄，以及它管理的socket对象池，以及一个管道，epoll关注管道读端的可读事件
struct socket_server * 
socket_server_create() {
	int i;
	int fd[2];
	poll_fd efd = sp_create();
	if (sp_invalid(efd)) {
		fprintf(stderr, "socket-server: create event pool failed.\n");
		return NULL;
	}
	// 创建一个管道
	if (pipe(fd)) {
		sp_release(efd);
		fprintf(stderr, "socket-server: create socket pair failed.\n");
		return NULL;
	}
	// epoll关注管道读端的可读事件
	if (sp_add(efd, fd[0], NULL)) {
		// add recvctrl_fd to event poll
		fprintf(stderr, "socket-server: can't add server fd to event pool.\n");
		close(fd[0]);
		close(fd[1]);
		sp_release(efd);
		return NULL;
	}

	struct socket_server *ss = MALLOC(sizeof(*ss));
	ss->event_fd = efd;
	ss->recvctrl_fd = fd[0];
	ss->sendctrl_fd = fd[1];
	ss->checkctrl = 1;

	// 初始化64K个socket
	for (i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];
		s->type = SOCKET_TYPE_INVALID;
		s->head = NULL;
		s->tail = NULL;
	}
	ss->alloc_id = 0;
	ss->event_n = 0;
	ss->event_index = 0;
	FD_ZERO(&ss->rfds);
	assert(ss->recvctrl_fd < FD_SETSIZE);

	return ss;
}

// 强制关闭套接字
static void
force_close(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	result->id = s->id;
	result->ud = 0;
	result->data = NULL;
	result->opaque = s->opaque;
	if (s->type == SOCKET_TYPE_INVALID) {
		return;
	}
	assert(s->type != SOCKET_TYPE_RESERVE);
	// 销毁发送缓冲区
	struct write_buffer *wb = s->head;
	while (wb) {
		struct write_buffer *tmp = wb;
		wb = wb->next;
		FREE(tmp->buffer);
		FREE(tmp);
	}
	s->head = s->tail = NULL;
	if (s->type != SOCKET_TYPE_PACCEPT && s->type != SOCKET_TYPE_PLISTEN) {
		sp_del(ss->event_fd, s->fd);	// epoll取消关注该套接字
	}
	// SOCKET_TYPE_BIND类型不需要close(比如stdin,stdout等)，其它socket类型需要close
	if (s->type != SOCKET_TYPE_BIND) {
		close(s->fd);
	}
	s->type = SOCKET_TYPE_INVALID;
}

// 销毁socket_server
void 
socket_server_release(struct socket_server *ss) {
	int i;
	struct socket_message dummy;
	for (i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];
		if (s->type != SOCKET_TYPE_RESERVE) {
			force_close(ss, s , &dummy);
		}
	}
	close(ss->sendctrl_fd);
	close(ss->recvctrl_fd);
	sp_release(ss->event_fd);
	FREE(ss);
}

// 为新预留的socket初始化，如果add为true还会将该套接字加入epoll管理
static struct socket *
new_fd(struct socket_server *ss, int id, int fd, uintptr_t opaque, bool add) {
	struct socket * s = &ss->slot[id % MAX_SOCKET];
	assert(s->type == SOCKET_TYPE_RESERVE);

	if (add) {
		if (sp_add(ss->event_fd, fd, s)) {
			s->type = SOCKET_TYPE_INVALID;
			return NULL;
		}
	}

	s->id = id;
	s->fd = fd;
	s->size = MIN_READ_BUFFER;
	s->opaque = opaque;
	s->wb_size = 0;
	assert(s->head == NULL);
	assert(s->tail == NULL);
	return s;
}

// 用于connect
// return -1 when connecting
// 连接成功返回SOCKET_OPEN，未连接成功（处于连接中）返回-1，出错返回SOCKET_ERROR
static int
open_socket(struct socket_server *ss, struct request_open * request, struct socket_message *result, bool blocking) {
	int id = request->id;
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = NULL;
	struct socket *ns;
	int status;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	struct addrinfo *ai_ptr = NULL;
	char port[16];
	sprintf(port, "%d", request->port);
	memset( &ai_hints, 0, sizeof( ai_hints ) );
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_STREAM;
	ai_hints.ai_protocol = IPPROTO_TCP;

	// 获取地址列表，(不用gethostbyname、gethostbyaddr，这两个函数仅支持IPv4)
	// 第一个参数是IP地址或主机名称，第二个参数是服务名(可以是端口号或服务名称，如ftp、http等)
	status = getaddrinfo( request->host, port, &ai_hints, &ai_list );
	if ( status != 0 ) {
		goto _failed;
	}
	int sock= -1;
	for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next ) {
		sock = socket( ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol );
		if ( sock < 0 ) {
			continue;
		}
		socket_keepalive(sock);
		if (!blocking) {
			sp_nonblocking(sock);	// blocking为false，设置为非阻塞模式，即用非阻塞模式connect
		}
		status = connect( sock, ai_ptr->ai_addr, ai_ptr->ai_addrlen);
		if ( status != 0 && errno != EINPROGRESS) {
			close(sock);
			sock = -1;
			continue;	// 连接出错，跳过本次循环，连接下一个地址
		}
		if (blocking) {
			sp_nonblocking(sock);	// 到此为止，不管blocking是真是假，都设为非阻塞模式，即IO模型使用的是non blocking + event loop + threadpool
		}
		break;
	}

	if (sock < 0) {
		goto _failed;
	}

	ns = new_fd(ss, id, sock, request->opaque, true);	// 加入epoll管理
	if (ns == NULL) {
		close(sock);
		goto _failed;
	}

	if(status == 0) {	// 说明connect已连接成功
		ns->type = SOCKET_TYPE_CONNECTED;
		struct sockaddr * addr = ai_ptr->ai_addr;
		void * sin_addr = (ai_ptr->ai_family == AF_INET) ? (void*)&((struct sockaddr_in *)addr)->sin_addr : (void*)&((struct sockaddr_in6 *)addr)->sin6_addr;
		if (inet_ntop(ai_ptr->ai_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
			result->data = ss->buffer;
		}
		freeaddrinfo( ai_list );
		return SOCKET_OPEN;
	} else {	// 说明非阻塞套接字尝试连接中
		ns->type = SOCKET_TYPE_CONNECTING;
		sp_write(ss->event_fd, ns->fd, ns, true);	// 非阻塞套接字尝试连接中，必需将关注其可写事件，稍后epoll才能捕获到连接出错了还是成功了
	}

	freeaddrinfo( ai_list );
	return -1;
_failed:
	freeaddrinfo( ai_list );
	ss->slot[id % MAX_SOCKET].type = SOCKET_TYPE_INVALID;	// 归还socket
	return SOCKET_ERROR;
}

// 可写事件到来，从应用层发送缓冲区取数据发送
static int
send_buffer(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	while (s->head) {
		struct write_buffer * tmp = s->head;
		for (;;) {
			int sz = write(s->fd, tmp->ptr, tmp->sz);
			if (sz < 0) {
				switch(errno) {
				case EINTR:
					continue;
				case EAGAIN:			// 内核发送缓冲区已满
					return -1;
				}
				force_close(ss,s, result);	// 发生严重错误，强制关闭
				return SOCKET_CLOSE;
			}
			s->wb_size -= sz;			// 发送缓冲区未发的字节数需更新
			if (sz != tmp->sz) {			// 未将该块缓冲区完全发送出去
				tmp->ptr += sz;			// 该块缓冲区未发送数据首地址更新
				tmp->sz -= sz;			// 当前块未发送字节数更新
				return -1;
			}
			break;
		}
		s->head = tmp->next;				// 取下一块缓冲区
		FREE(tmp->buffer);				// 销毁已发送的缓冲区块
		FREE(tmp);
	}
	s->tail = NULL;
	sp_write(ss->event_fd, s->fd, s, false);		// 应用层发送缓冲区数据发完，取消关注可写事件

	// 半关闭状态socket不可再调用send_buffer，如果发现这样做，直接强制关闭
	if (s->type == SOCKET_TYPE_HALFCLOSE) {
		force_close(ss, s, result);
		return SOCKET_CLOSE;
	}

	return -1;
}

// 将未发送完的数据追加到socket发送缓冲区中，n表示从第n个字节开始
static void
append_sendbuffer(struct socket *s, struct request_send * request, int n) {
	struct write_buffer * buf = MALLOC(sizeof(*buf));
	buf->ptr = request->buffer+n;
	buf->sz = request->sz - n;
	buf->buffer = request->buffer;
	buf->next = NULL;
	s->wb_size += buf->sz;		// 未发送字节数更新
	if (s->head == NULL) {
		s->head = s->tail = buf;
	} else {
		assert(s->tail != NULL);
		assert(s->tail->next == NULL);
		s->tail->next = buf;
		s->tail = buf;
	}
}

static int
send_socket(struct socket_server *ss, struct request_send * request, struct socket_message *result) {
	int id = request->id;
	struct socket * s = &ss->slot[id % MAX_SOCKET];
	if (s->type == SOCKET_TYPE_INVALID || s->id != id 
		|| s->type == SOCKET_TYPE_HALFCLOSE
		|| s->type == SOCKET_TYPE_PACCEPT) {
		FREE(request->buffer);
		return -1;
	}
	assert(s->type != SOCKET_TYPE_PLISTEN && s->type != SOCKET_TYPE_LISTEN);
	if (s->head == NULL) {
		// 应用层缓冲区中没有数据，直接发送
		int n = write(s->fd, request->buffer, request->sz);
		if (n<0) {
			switch(errno) {
			case EINTR:
			case EAGAIN:	// 内核缓冲区满
				n = 0;
				break;
			default:
				fprintf(stderr, "socket-server: write to %d (fd=%d) error.",id,s->fd);
				force_close(ss,s,result);
				return SOCKET_CLOSE;
			}
		}
		if (n == request->sz) {
			FREE(request->buffer);
			return -1;
		}
		// 运行到这里，说明未将要发送的数据全部拷贝到内核缓冲区
		// 将要未发送的数据添加到应用层缓冲区
		append_sendbuffer(s, request, n);
		sp_write(ss->event_fd, s->fd, s, true);		// 关注可写事件
	} else {
		// 直接将要发送的数据追加到应用层缓冲区后面
		append_sendbuffer(s, request, 0);
	}
	return -1;
}

static int
listen_socket(struct socket_server *ss, struct request_listen * request, struct socket_message *result) {
	int id = request->id;
	int listen_fd = request->fd;
	// 初始化新申请到的socket，注意下面函数最后一个参数false
	struct socket *s = new_fd(ss, id, listen_fd, request->opaque, false);
	if (s == NULL) {
		goto _failed;
	}
	s->type = SOCKET_TYPE_PLISTEN;
	return -1;
_failed:
	close(listen_fd);
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = NULL;
	ss->slot[id % MAX_SOCKET].type = SOCKET_TYPE_INVALID;	// 归还socket

	return SOCKET_ERROR;
}

static int
close_socket(struct socket_server *ss, struct request_close *request, struct socket_message *result) {
	int id = request->id;
	struct socket * s = &ss->slot[id % MAX_SOCKET];
	if (s->type == SOCKET_TYPE_INVALID || s->id != id) {	// 说明已关闭或者无效的socket
		result->id = id;
		result->opaque = request->opaque;
		result->ud = 0;
		result->data = NULL;
		return SOCKET_CLOSE;
	}
	if (s->head) { 
		int type = send_buffer(ss,s,result);	// 将应用层发送缓冲区数据发送出去
		if (type != -1)
			return type;
	}
	if (s->head == NULL) {		// 说明已经发送完毕
		force_close(ss,s,result);
		result->id = id;
		result->opaque = request->opaque;
		return SOCKET_CLOSE;
	}
	s->type = SOCKET_TYPE_HALFCLOSE;	// 说明应用层发送缓冲区未发送完毕，并且还未调用close，即应用层假定它为半关闭状态，不可以再发送数据了

	return -1;
}

// 将stdin、stdout这种类型的文件描述符加入epoll管理，这种类型称之为SOCKET_TYPE_BIND
static int
bind_socket(struct socket_server *ss, struct request_bind *request, struct socket_message *result) {
	int id = request->id;
	result->id = id;
	result->opaque = request->opaque;
	result->ud = 0;
	struct socket *s = new_fd(ss, id, request->fd, request->opaque, true);
	if (s == NULL) {
		result->data = NULL;
		return SOCKET_ERROR;
	}
	sp_nonblocking(request->fd);
	s->type = SOCKET_TYPE_BIND;
	result->data = "binding";
	return SOCKET_OPEN;
}

// 对于SOCKET_TYPE_PACCEPT或者SOCKET_TYPE_PLISTEN这两种类型socket，将其加入epoll管理，并更新其状态
static int
start_socket(struct socket_server *ss, struct request_start *request, struct socket_message *result) {
	int id = request->id;
	result->id = id;
	result->opaque = request->opaque;
	result->ud = 0;
	result->data = NULL;
	struct socket *s = &ss->slot[id % MAX_SOCKET];
	if (s->type == SOCKET_TYPE_INVALID || s->id !=id) {	// 无效的socket
		return SOCKET_ERROR;
	}
	if (s->type == SOCKET_TYPE_PACCEPT || s->type == SOCKET_TYPE_PLISTEN) {
		if (sp_add(ss->event_fd, s->fd, s)) {
			s->type = SOCKET_TYPE_INVALID;
			return SOCKET_ERROR;
		}
		s->type = (s->type == SOCKET_TYPE_PACCEPT) ? SOCKET_TYPE_CONNECTED : SOCKET_TYPE_LISTEN;
		s->opaque = request->opaque;
		result->data = "start";
		return SOCKET_OPEN;
	}
	return -1;
}

// 从管道读数据
static void
block_readpipe(int pipefd, void *buffer, int sz) {
	for (;;) {
		int n = read(pipefd, buffer, sz);
		if (n<0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "socket-server : read pipe error %s.",strerror(errno));
			return;
		}
		// must atomic read from a pipe
		assert(n == sz);
		return;
	}
}

static int
has_cmd(struct socket_server *ss) {
	struct timeval tv = {0,0};
	int retval;

	FD_SET(ss->recvctrl_fd, &ss->rfds);

	retval = select(ss->recvctrl_fd+1, &ss->rfds, NULL, NULL, &tv);
	if (retval == 1) {
		return 1;
	}
	return 0;
}

// return type
static int
ctrl_cmd(struct socket_server *ss, struct socket_message *result) {
	int fd = ss->recvctrl_fd;
	// the length of message is one byte, so 256+8 buffer size is enough.
	uint8_t buffer[256];
	uint8_t header[2];
	block_readpipe(fd, header, sizeof(header));		// 包头
	int type = header[0];
	int len = header[1];
	block_readpipe(fd, buffer, len);			// 包体
	// ctrl command only exist in local fd, so don't worry about endian.
	switch (type) {
	case 'S':
		// 正常流程下，下面函数调用后，socket_server对象的epoll会关注socket
		return start_socket(ss,(struct request_start *)buffer, result);
	case 'B':
		return bind_socket(ss,(struct request_bind *)buffer, result);
	case 'L':
		return listen_socket(ss,(struct request_listen *)buffer, result);
	case 'K':
		return close_socket(ss,(struct request_close *)buffer, result);
	case 'O':
		return open_socket(ss, (struct request_open *)buffer, result, false);
	case 'X':
		result->opaque = 0;
		result->id = 0;
		result->ud = 0;
		result->data = NULL;
		return SOCKET_EXIT;
	case 'D':
		return send_socket(ss, (struct request_send *)buffer, result);
	default:
		fprintf(stderr, "socket-server: Unknown ctrl %c.\n",type);
		return -1;
	};

	return -1;
}

// 可读事件到来，执行该函数
// return -1 (ignore) when error
static int
forward_message(struct socket_server *ss, struct socket *s, struct socket_message * result) {
	int sz = s->size;
	char * buffer = MALLOC(sz);
	int n = (int)read(s->fd, buffer, sz);
	if (n<0) {
		FREE(buffer);
		switch(errno) {
		case EINTR:
			break;
		case EAGAIN:
			fprintf(stderr, "socket-server: EAGAIN capture.\n");
			break;
		default:
			// close when error
			force_close(ss, s, result);		// 错误则强制关闭
			return SOCKET_ERROR;
		}
		return -1;
	}
	if (n==0) {		// 对等端关闭
		FREE(buffer);
		force_close(ss, s, result);
		return SOCKET_CLOSE;
	}

	if (s->type == SOCKET_TYPE_HALFCLOSE) {
		// discard recv data
		FREE(buffer);
		return -1;
	}

	if (n == sz) {
		s->size *= 2;
	} else if (sz > MIN_READ_BUFFER && n*2 < sz) {
		s->size /= 2;	// s->size的最小值为MIN_READ_BUFFER
	}

	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = n;
	result->data = buffer;
	return SOCKET_DATA;
}

// 尝试连接中的套接字可写事件发生，可能是连接成功，也可能是连接出错
static int
report_connect(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	int error;
	socklen_t len = sizeof(error);  
	int code = getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &len);  
	if (code < 0 || error) {  
		force_close(ss,s, result);
		return SOCKET_ERROR;
	} else {
		s->type = SOCKET_TYPE_CONNECTED;
		result->opaque = s->opaque;
		result->id = s->id;
		result->ud = 0;
		sp_write(ss->event_fd, s->fd, s, false);	// 连接成功，取消关注可写事件
		union sockaddr_all u;
		socklen_t slen = sizeof(u);
		if (getpeername(s->fd, &u.s, &slen) == 0) {
			void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
			if (inet_ntop(u.s.sa_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
				result->data = ss->buffer;
				return SOCKET_OPEN;
			}
		}
		result->data = NULL;
		return SOCKET_OPEN;
	}
}

// 监听套接字可读事件到来，调用该函数
// return 0 when failed
static int
report_accept(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	union sockaddr_all u;
	socklen_t len = sizeof(u);
	int client_fd = accept(s->fd, &u.s, &len);
	if (client_fd < 0) {
		return 0;
	}
	int id = reserve_id(ss);	// 从socket池中申请一个socket
	if (id < 0) {		// 说明应用层socket已用完
		close(client_fd);
		return 0;
	}
	socket_keepalive(client_fd);
	sp_nonblocking(client_fd);		// 设置为非阻塞
	struct socket *ns = new_fd(ss, id, client_fd, s->opaque, false);	// 初始化申请到的socket，未加入epoll管理
	if (ns == NULL) {
		close(client_fd);
		return 0;
	}
	ns->type = SOCKET_TYPE_PACCEPT;
	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = id;
	result->data = NULL;

	void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
	if (inet_ntop(u.s.sa_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
		result->data = ss->buffer;
	}

	return 1;
}


// return type
int 
socket_server_poll(struct socket_server *ss, struct socket_message * result, int * more) {
	for (;;) {
		// 控制命令的检查没有纳入到epoll管理(本质是读取管道)，目的是为了提高控制命令检查频率
		if (ss->checkctrl) {
			if (has_cmd(ss)) {
				int type = ctrl_cmd(ss, result);
				if (type != -1)
					return type;
				else
					continue;
			} else { // 没有控制命令
				ss->checkctrl = 0;
			}
		}

		// 从epoll中获取事件(所有socket事件), 每次最多获取8个，存到ss->ev，比如收到5个消息，event_index是0，event_n是5
		// 然后调整到下面的default分支，处理e->write和e->read，每次处理一个，处理完event_index++
		// 然后重新到函数最开始的循环，继续event_index=1的事件，一直到event_index=5表示5个消息处理完了，然后epoll再取消息
		if (ss->event_index == ss->event_n) {
			ss->event_n = sp_wait(ss->event_fd, ss->ev, MAX_EVENT);
			ss->checkctrl = 1;
			if (more) {
				*more = 0;
			}
			ss->event_index = 0;
			if (ss->event_n <= 0) {
				ss->event_n = 0;
				return -1;
			}
		}

		// 处理从epoll消息得到的某个event事件
		struct event *e = &ss->ev[ss->event_index++];
		struct socket *s = e->s;
		if (s == NULL) {
			// dispatch pipe message at beginning
			continue;
		}
		switch (s->type) {
		case SOCKET_TYPE_CONNECTING:
			return report_connect(ss, s, result);
		case SOCKET_TYPE_LISTEN:
			if (report_accept(ss, s, result)) {
				return SOCKET_ACCEPT;
			} 
			break;
		case SOCKET_TYPE_INVALID:
			fprintf(stderr, "socket-server: invalid socket\n");
			break;
		default:
			if (e->write) {
				int type = send_buffer(ss, s, result);	// 可写事件到来，从应用层缓冲区中取出数据发送
				if (type == -1)
					break;
				return type;
			}
			if (e->read) {
				int type = forward_message(ss, s, result);
				if (type == -1)
					break;
				return type;
			}
			break;
		}
	}
}

// 向管道发送请求
static void
send_request(struct socket_server *ss, struct request_package *request, char type, int len) {
	request->header[6] = (uint8_t)type;
	request->header[7] = (uint8_t)len;
	for (;;) {
		int n = write(ss->sendctrl_fd, &request->header[6], len+2);
		if (n<0) {
			if (errno != EINTR) {
				fprintf(stderr, "socket-server : send ctrl command error %s.\n", strerror(errno));
			}
			continue;
		}
		assert(n == len+2);
		return;
	}
}

// 用于connect,准备一个请求包
static int
open_request(struct socket_server *ss, struct request_package *req, uintptr_t opaque, const char *addr, int port) {
	int len = strlen(addr);
	if (len + sizeof(req->u.open) > 256) {
		fprintf(stderr, "socket-server : Invalid addr %s.\n",addr);
		return 0;
	}
	int id = reserve_id(ss);	// 申请一个应用层socket
	req->u.open.opaque = opaque;
	req->u.open.id = id;
	req->u.open.port = port;
	memcpy(req->u.open.host, addr, len);
	req->u.open.host[len] = '\0';

	return len;
}

int 
socket_server_connect(struct socket_server *ss, uintptr_t opaque, const char * addr, int port) {
	struct request_package request;
	int len = open_request(ss, &request, opaque, addr, port);	// 准备请求包
	send_request(ss, &request, 'O', sizeof(request.u.open) + len);
	return request.u.open.id;
}

int 
socket_server_block_connect(struct socket_server *ss, uintptr_t opaque, const char * addr, int port) {
	struct request_package request;
	struct socket_message result;
	open_request(ss, &request, opaque, addr, port);
	int ret = open_socket(ss, &request.u.open, &result, true);
	if (ret == SOCKET_OPEN) {
		return result.id;
	} else {
		return -1;
	}
}

// return -1 when error
int64_t 
socket_server_send(struct socket_server *ss, int id, const void * buffer, int sz) {
	struct socket * s = &ss->slot[id % MAX_SOCKET];
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {
		return -1;
	}
	assert(s->type != SOCKET_TYPE_RESERVE);

	struct request_package request;
	request.u.send.id = id;
	request.u.send.sz = sz;
	request.u.send.buffer = (char *)buffer;

	send_request(ss, &request, 'D', sizeof(request.u.send));
	return s->wb_size;
}

void
socket_server_exit(struct socket_server *ss) {
	struct request_package request;
	send_request(ss, &request, 'X', 0);
}

void
socket_server_close(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.close.id = id;
	request.u.close.opaque = opaque;
	send_request(ss, &request, 'K', sizeof(request.u.close));
}

static int
do_listen(const char * host, int port, int backlog) {
	// only support ipv4
	// todo: support ipv6 by getaddrinfo
	uint32_t addr = INADDR_ANY;
	if (host[0]) {
		addr=inet_addr(host);
	}
	int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		return -1;
	}
	int reuse = 1;
	if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (void *)&reuse, sizeof(int))==-1) {
		goto _failed;
	}

	struct sockaddr_in my_addr;
	memset(&my_addr, 0, sizeof(struct sockaddr_in));
	my_addr.sin_family = AF_INET;
	my_addr.sin_port = htons(port);
	my_addr.sin_addr.s_addr = addr;
	if (bind(listen_fd, (struct sockaddr *)&my_addr, sizeof(struct sockaddr)) == -1) {
		goto _failed;
	}
	if (listen(listen_fd, backlog) == -1) {
		goto _failed;
	}
	return listen_fd;
_failed:
	close(listen_fd);
	return -1;
}

// 函数分3个步骤:
// 1.创建socket, bind然后listen, 句柄是fd
// 2.从socket池中获取一个空的socket，得到id
// 3.构造一个对象，里面包含socket句柄和socket池中对象id，然后发给管道
// 因为epoll关注了管道读端的可读事件，所以epoll马上可以读到该事件，至于opaque，可以假象系统中有一个listen的actor对象，opaque是它的句柄，这里应该只是方便调试
// 返回值是socket池中对象id
// 到这里只是创建了socket，但是epoll还没有关注这个句柄
int 
socket_server_listen(struct socket_server *ss, uintptr_t opaque, const char * addr, int port, int backlog) {
	int fd = do_listen(addr, port, backlog);
	if (fd < 0) {
		return -1;
	}
	struct request_package request;
	int id = reserve_id(ss);
	request.u.listen.opaque = opaque;
	request.u.listen.id = id;
	request.u.listen.fd = fd;
	send_request(ss, &request, 'L', sizeof(request.u.listen));
	return id;
}

int
socket_server_bind(struct socket_server *ss, uintptr_t opaque, int fd) {
	struct request_package request;
	int id = reserve_id(ss);
	request.u.bind.opaque = opaque;
	request.u.bind.id = id;
	request.u.bind.fd = fd;
	send_request(ss, &request, 'B', sizeof(request.u.bind));
	return id;
}

// 发消息到管道，epoll对象会收到消息再处理
void 
socket_server_start(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.start.id = id;
	request.u.start.opaque = opaque;
	send_request(ss, &request, 'S', sizeof(request.u.start));
}


