#ifndef skynet_socket_server_h
#define skynet_socket_server_h

#include <stdint.h>

// socket_server_poll���ص�socket��Ϣ����
#define SOCKET_DATA		0		// �����ݵ���
#define SOCKET_CLOSE		1		// ���ӹر�
#define SOCKET_OPEN		2		// ���ӽ������������߱����������Ѽ��뵽epoll��
#define SOCKET_ACCEPT		3		// �������ӽ�������accept�ɹ������������׽��֣���δ���뵽epoll
#define SOCKET_ERROR		4		// ��������
#define SOCKET_EXIT		5		// �˳��¼�

struct socket_server;

struct socket_message {
	int		id;			// socket�ص�index�������ҵ�socket���ݽṹ
	uintptr_t	opaque;			// ��skynet�����ڱ������handle
	int		ud;			// for accept,ud�������ӵ�id; for data, ud is size of data 
	char		*data;
};

// ����socket_server
struct socket_server * socket_server_create();

// ����socket_server
void socket_server_release(struct socket_server *);

// �����¼�
int socket_server_poll(struct socket_server *, struct socket_message *result, int *more);

// �˳�socket�������������¼�ѭ���˳�
void socket_server_exit(struct socket_server *);

// �ر�socket
void socket_server_close(struct socket_server *, uintptr_t opaque, int id);

// ����socket�����ڼ����׽��ֻ����������׽��֣���Ҫ���øú�����socket�ſ�ʼ����
void socket_server_start(struct socket_server *, uintptr_t opaque, int id);

// ����: return -1 when error
int64_t socket_server_send(struct socket_server *, int id, const void * buffer, int sz);

// ����,socket, bind, listen: ctrl command below returns id
int socket_server_listen(struct socket_server *, uintptr_t opaque, const char * addr, int port, int backlog);

// �������ķ�ʽ����
int socket_server_connect(struct socket_server *, uintptr_t opaque, const char * addr, int port);

// ������Ӧbind���������ǽ�stdin��stdout����IO���뵽epoll����
int socket_server_bind(struct socket_server *, uintptr_t opaque, int fd);

// ��������ʽ����
int socket_server_block_connect(struct socket_server *, uintptr_t opaque, const char * addr, int port);

#endif

