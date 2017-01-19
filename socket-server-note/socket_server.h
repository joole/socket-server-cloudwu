#ifndef skynet_socket_server_h
#define skynet_socket_server_h

#include <stdint.h>

// socket_server_poll返回的socket消息类型
#define SOCKET_DATA 0		// 有数据到来
#define SOCKET_CLOSE 1		// 连接关闭
#define SOCKET_OPEN 2		// 连接建立（主动或者被动，并且已加入到epoll）
#define SOCKET_ACCEPT 3		// 被动连接建立（即accept成功返回已连接套接字）但未加入到epoll
#define SOCKET_ERROR 4		// 发生错误
#define SOCKET_EXIT 5		// 退出事件

struct socket_server;

struct socket_message {
	int id;
	uintptr_t opaque;	// 在skynet中用于保存服务handle
	int ud;	// for accept, ud is listen id ; for data, ud is size of data 
			// 这里作者的注释有误,for accept,ud是新连接的id
	char * data;
};

// 创建socket_server
struct socket_server * socket_server_create();
// 销毁socket_server
void socket_server_release(struct socket_server *);
// 返回事件
int socket_server_poll(struct socket_server *, struct socket_message *result, int *more);

// 退出socket服务器，导致事件循环退出
void socket_server_exit(struct socket_server *);
// 关闭socket
void socket_server_close(struct socket_server *, uintptr_t opaque, int id);
// 启动socket，对于监听套接字或者已连接套接字，都要调用该函数，socket才开始工作
void socket_server_start(struct socket_server *, uintptr_t opaque, int id);

// return -1 when error
// 发送
int64_t socket_server_send(struct socket_server *, int id, const void * buffer, int sz);

// ctrl command below returns id
// 监听,socket, bind, listen
int socket_server_listen(struct socket_server *, uintptr_t opaque, const char * addr, int port, int backlog);
// 非阻塞的方式连接
int socket_server_connect(struct socket_server *, uintptr_t opaque, const char * addr, int port);
// 并不对应bind函数，而是将stdin、stdout这类IO加入到epoll管理
int socket_server_bind(struct socket_server *, uintptr_t opaque, int fd);

// 以阻塞方式连接
int socket_server_block_connect(struct socket_server *, uintptr_t opaque, const char * addr, int port);

#endif
