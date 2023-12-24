#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libaio.h>
#include <pthread.h>

#include "aws.h"
#include "debug.h"
#include "util.h"
#include "sock_util.h"
#include "w_epoll.h"
#include "http-parser/http_parser.h"

int num_ops = 10;
#define AIO_SIZE BUFSIZ

static int listenfd;
static int epollfd;

static http_parser parser;

static char *request_path;

int request_path_cb(http_parser *p, const char *buf, size_t len)
{
	assert(p == &parser);
	memcpy(request_path, buf, len);
	dlog(LOG_DEBUG, "Path read in header is %s\n", buf);

	return 0;
}

void free_connection(connection_t *con)
{
	close(con->filefd);
	free(con->path);
	free(con->buffer);
	free(con->message);
	free(con);
}

void clean_up(connection_t *con)
{
	int rc = w_epoll_remove_ptr(epollfd, con->sockfd, con);
	DIE(rc < 0, "w_epoll_remove_ptr");
	/* close local socket */

	if (con->thread_id == 0) {
		close(con->sockfd);
		free_connection(con);
	}
}

static int send_file_static(connection_t *con)
{
	int rc, total_sent = 0;

	while (total_sent < con->filesize) {
		rc = sendfile(con->sockfd, con->filefd, NULL, con->filesize);
		DIE(rc < 0, "sendfile");

		total_sent += rc;
	}

	return total_sent;
}

void *async_rw(void *data)
{
	connection_t *con = data;

	struct io_event *events = malloc(con->efd_val * sizeof(struct io_event));
	int rc = io_getevents(con->ctx, con->efd_val, con->efd_val, events, NULL);
	DIE(rc < 0, "io_getevents");
	dlog(LOG_INFO, "events read %d\n", rc);

	free(events);
	
	rc = io_destroy(con->ctx);
	DIE(rc < 0, "io_destroy");

	free(con->iocb_read);
	free(con->piocb_read);

	free(con->iocb_write);
	free(con->piocb_write);

	for (size_t i = 0; i < con->iocb_num; i++)
		free(con->iocb_buffer[i]);
	
	free(con->iocb_buffer);
	dlog(LOG_INFO, "finished thread %ld\n", pthread_self());

	close(con->sockfd);
	free_connection(con);
	return NULL;
}

static int send_file_dynamic(connection_t *con)
{
	int rc;

	con->eventfd = eventfd(0, 0);

	// Calculate ceil for iocb numbers
	con->iocb_num = con->filesize / AIO_SIZE + (con->filesize % AIO_SIZE != 0);

	rc = io_setup(con->iocb_num * 2, &con->ctx);
	DIE(rc < 0, "io_setup");

	con->iocb_buffer = calloc(con->iocb_num, sizeof(char *));
	for (int i = 0; i < con->iocb_num; i++)
		con->iocb_buffer[i] = calloc(AIO_SIZE, sizeof(char));

	con->iocb_read = malloc(con->iocb_num * sizeof(struct iocb));
	con->iocb_write = malloc(con->iocb_num * sizeof(struct iocb));
	DIE(!con->iocb_read || !con->iocb_write, "iocb alloc");
	con->piocb_read = malloc(con->iocb_num * sizeof(struct iocb *));
	con->piocb_write = malloc(con->iocb_num * sizeof(struct iocb *));
	DIE(!con->piocb_read || !con->piocb_write, "piocb alloc");

	for (size_t i = 0; i < con->iocb_num; i++) {
		con->piocb_read[i] = &con->iocb_read[i];
		io_prep_pread(con->piocb_read[i], con->filefd, con->iocb_buffer[i], 
					  AIO_SIZE, i * AIO_SIZE);
		io_set_eventfd(con->piocb_read[i], con->eventfd);

		con->piocb_write[i] = &con->iocb_write[i];
		io_prep_pwrite(con->piocb_write[i], con->sockfd, con->iocb_buffer[i], 
					   AIO_SIZE, 0);
		io_set_eventfd(con->piocb_write[i], con->eventfd);
	}

	rc = io_submit(con->ctx, con->iocb_num, con->piocb_read);
	DIE(rc < 0, "io_submit");

	rc = io_submit(con->ctx, con->iocb_num, con->piocb_write);
	DIE(rc < 0, "io_submit");

	rc = read(con->eventfd, &con->efd_val, sizeof(con->efd_val));
	DIE(rc < 0, "efd_val");

	pthread_create(&con->thread_id, NULL, async_rw, con);

	return con->filesize;
}

static http_parser_settings parser_settings =
	{.on_message_begin = 0
	,.on_header_field = 0
	,.on_header_value = 0
	,.on_path = request_path_cb
	,.on_url = 0
	,.on_fragment = 0
	,.on_query_string = 0
	,.on_body = 0
	,.on_headers_complete = 0
	,.on_message_complete = 0
};

static void accept_connection(int listenfd)
{
	int sockfd;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	connection_t *con = calloc(1, sizeof(connection_t));

	/* accept new connection */
	sockfd = accept(listenfd, (SSA *) &addr, &addrlen);
	DIE(sockfd < 0, "accept");

	dlog(LOG_INFO, "Accepted connection from %s:%d\n",
		inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

	fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL) | O_NONBLOCK);

	DIE(!con, "con calloc");

	con->sockfd = sockfd;
	con->buffer = malloc(BUFSIZ);
	DIE(!con->buffer, "buffer malloc");
	con->path = malloc(BUFSIZ);
	DIE(!con->path, "path malloc");
	con->message = malloc(BUFSIZ);
	DIE(!con->message, "message malloc");

	w_epoll_add_ptr_in(epollfd, sockfd, con);
}

static void receive_request(connection_t *con)
{
	char abuffer[64];
	int bytes_read;
	int rc;

	rc = get_peer_address(con->sockfd, abuffer, 64);
	if (rc < 0 || con->status == 1) {
		ERR("get_peer_address");
		clean_up(con);
		return;
	}

	memset(con->buffer, 0, BUFSIZ);
	con->buffer_len = 0;

	do {
		bytes_read = recv(con->sockfd, con->buffer + con->buffer_len, 
						  BUFSIZ, 0);
		con->buffer_len += bytes_read;
		dlog(LOG_INFO, "Read %d bytes\n", bytes_read);
	} while (bytes_read > 0);

	if (con->buffer_len < 0) {		/* error in communication */
		dlog(LOG_ERR, "Error in communication from %s\n", abuffer);
		clean_up(con);
		return;
	}
	if (con->buffer_len == 0) {		/* connection closed */
		dlog(LOG_INFO, "Connection closed from %s\n", abuffer);
		clean_up(con);
		return;
	}

	dlog(LOG_INFO, "Received message from %s\n", abuffer);
	dlog(LOG_INFO, "Received %s with len %d\n", con->buffer, 
		 con->buffer_len);

	memset(request_path, 0, BUFSIZ);
	http_parser_init(&parser, HTTP_REQUEST);
	http_parser_execute(&parser, &parser_settings, con->buffer, 
						con->buffer_len);

	memset(con->path, 0, BUFSIZ);
	sprintf(con->path, AWS_DOCUMENT_ROOT "%s", request_path + 1);
	dlog(LOG_INFO, "Connection path is %s\n", con->path);

	con->filefd = open(con->path, O_RDWR);

	memset(con->message, 0, BUFSIZ);
	if (con->filefd == -1) {
		strcat(con->message, "HTTP/1.1 404 Not Found\r\n\r\n");
		con->message_len = strlen(con->message);
		dlog(LOG_ERR, "Bad file descriptor\n");
	} else {
		struct stat st;
		fstat(con->filefd, &st);
		con->filesize = st.st_size;
		sprintf(con->message, 
				"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n"
				"Connection: close\r\n"
				"Content-Type: text/html\r\n\r\n", con->filesize);
		con->message_len = strlen(con->message);
	}

	dlog(LOG_INFO, "Read from path %s the message %s\n", request_path, 
		 con->buffer);

	rc = w_epoll_update_ptr_inout(epollfd, con->sockfd, con);
	DIE(rc < 0, "w_epoll_update_ptr_inout");
}

void send_status(connection_t *con)
{
	ssize_t bytes_sent;
	char abuffer[64];
	int rc;

	rc = get_peer_address(con->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		clean_up(con);
		return;
	}

	bytes_sent = send(con->sockfd, con->message, strlen(con->message) * 2, 0);
	if (bytes_sent < 0) {		/* error in communication */
		dlog(LOG_ERR, "Error in communication to %s\n", abuffer);
		clean_up(con);
		return;
	}
	if (bytes_sent == 0) {		/* connection closed */
		dlog(LOG_INFO, "Connection closed to %s\n", abuffer);
		clean_up(con);
		return;
	}

	dlog(LOG_INFO, "Sent message to %s (bytes: %ld)\n", abuffer,
		bytes_sent);

	if (con->filefd != -1) {
		// Searches for the y in dynamic, if found should we go to the else
		if (!strchr(con->path, 'y'))
			bytes_sent += send_file_static(con);
		else
			bytes_sent += send_file_dynamic(con);

	}

	dlog(LOG_INFO, "Sent file to %s (total bytes: %ld)\n", abuffer,
		bytes_sent);

	rc = w_epoll_update_ptr_in(epollfd, con->sockfd, con);
	DIE(rc < 0, "w_epoll_update_ptr_in");
}

int main() {
	int rc;

	request_path = malloc(BUFSIZ);
	DIE(!request_path, "path malloc");

	epollfd = w_epoll_create();
	DIE(epollfd < 0, "create epoll");

	listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "create listener");

	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "add epoll fd");

	dlog(LOG_INFO, "Started listening on port %d\n", AWS_LISTEN_PORT);

	dlog(LOG_INFO, "Current fd of listener is %d\n", listenfd);

	while (true) {
		struct epoll_event event;

		rc = w_epoll_wait_infinite(epollfd, &event);
		DIE(rc < 0, "w_epoll_wait_infinite");

		dlog(LOG_DEBUG, "Found something on fd %d\n", event.data.fd);

		if (event.data.fd == listenfd) {
			if (event.events & EPOLLIN)
				accept_connection(listenfd);
		} else {
			if (event.events & EPOLLOUT) {
				dlog(LOG_DEBUG, "Ready to send message\n");
				send_status(event.data.ptr);
			}

			if (event.events & EPOLLIN) {
				dlog(LOG_DEBUG, "New message\n");
				receive_request(event.data.ptr);
			}
		}
	}

	free(request_path);
	return 0;
}
