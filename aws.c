// SPDX-License-Identifier: BSD-3-Clause

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libaio.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "aws.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/util.h"
#include "utils/w_epoll.h"

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

/* aio context */
static io_context_t ctx;

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;

	memcpy(conn->request_path, buf, len); /* copy path to buffer */
	conn->request_path[len] = '\0';
	conn->have_path = 1;

	return 0;
}

static void connection_prepare_send_reply_header(struct connection *conn)
{
	const char *header_prefix =
						"HTTP/1.1 200 OK\r\n"
						"Content-Type: text/html\r\n"
						"Connection: close\r\n";

	char header_content_length[128];

	/* format the content-length header based on the file size */
	snprintf(header_content_length, sizeof(header_content_length),
	"Content-Length: %lu\r\n\r\n", (unsigned long)conn->file_size);

	/* combine headers into the send buffer and calculate total length */
	conn->send_len = snprintf(conn->send_buffer, sizeof(conn->send_buffer),
				"%s%s", header_prefix, header_content_length);

	conn->state = STATE_SENDING_HEADER;
	conn->send_pos = 0;
}

static void connection_prepare_send_404(struct connection *conn)
{
	const char *body_404 = "<html><body>404 Not Found</body></html>\n";
	size_t body_len = strlen(body_404);

	/* prepare the 404 response header */
	int header_len = snprintf(conn->send_buffer, sizeof(conn->send_buffer),
						"HTTP/1.1 404 Not Found\r\n"
						"Connection: close\r\n"
						"Content-Type: text/html\r\n"
						"Content-Length: %zu\r\n"
						"\r\n"
						"%s",
						body_len, body_404);

	conn->send_len = header_len;
	conn->send_pos = 0;
	conn->state = STATE_SENDING_404;
}

static enum resource_type
connection_get_resource_type(struct connection *conn)
{
	if (!conn->have_path)
		return RESOURCE_TYPE_NONE;

	const char *p = conn->request_path;

	if (p[0] == '/')
		p++;

	if (strncmp(p, AWS_REL_STATIC_FOLDER,
		strlen(AWS_REL_STATIC_FOLDER)) == 0) {
		return RESOURCE_TYPE_STATIC;
	} else if (strncmp(p, AWS_REL_DYNAMIC_FOLDER,
				strlen(AWS_REL_DYNAMIC_FOLDER))
				== 0) {
		return RESOURCE_TYPE_DYNAMIC;
	}

	return RESOURCE_TYPE_NONE;
}

struct connection *connection_create(int sockfd)
{
	struct connection *conn = calloc(1, sizeof(*conn));
	
	DIE(conn == NULL, "calloc");

	conn->sockfd = sockfd;
	conn->fd = -1; /* not opened yet */
	conn->eventfd = eventfd(
	    0, EFD_CLOEXEC | EFD_NONBLOCK); /* create an eventfd for signaling */
	DIE(conn->eventfd < 0, "eventfd");

	conn->state = STATE_RECEIVING_DATA;
	conn->ctx = ctx;

	/* prepare HTTP parser */
	http_parser_init(&conn->request_parser, HTTP_REQUEST);
	conn->request_parser.data = conn;

	return conn;
}

void connection_start_async_io(struct connection* conn) {
	size_t remaining = conn->file_size - conn->file_pos;
	size_t chunk_size = BUFSIZ;

	/* Choose the smaller of BUFSIZ and what's left in the file. */
	if (remaining < chunk_size)
		chunk_size = remaining;

	/* prepare an asynchronous read operation */
	io_prep_pread(&conn->iocb, conn->fd, conn->send_buffer, chunk_size,
	              conn->file_pos);

	conn->async_read_len = chunk_size;
	conn->iocb.data = conn; /* link back to this conn */
	io_set_eventfd(
	    &conn->iocb,
	    conn->eventfd); /* link eventfd for completion notification */

	conn->piocb[0] = &conn->iocb;

	/* submit async read */
	int ret = io_submit(conn->ctx, 1, conn->piocb);
	if (ret != 1) {
		perror("io_submit");
		io_destroy(conn->ctx);
		exit(1);
	}

	conn->state = STATE_ASYNC_ONGOING;
}

void connection_remove(struct connection* conn) {
	int rc;

	/* remove socket from epoll. */
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	rc = w_epoll_remove_ptr(epollfd, conn->eventfd, conn);
	(void) rc;

	close(conn->eventfd);

	if (conn->sockfd >= 0)
		close(conn->sockfd);
	if (conn->fd >= 0)
		close(conn->fd);
	free(conn);
}

void handle_new_connection(void) {
	int rc;
	int sockfd;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	struct connection* conn;

	/* accept new connection. */
	sockfd = accept(listenfd, (struct sockaddr*)&addr, &addrlen);
	DIE(sockfd < 0, "accept");

	/* set socket to be non-blocking. */
	rc = fcntl(sockfd, F_SETFL, O_NONBLOCK);
	DIE(rc < 0, "fcntl");

	conn = connection_create(sockfd);

	rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
	DIE(rc < 0, "w_epoll_add_ptr_in");

	rc = w_epoll_add_ptr_in(epollfd, conn->eventfd, conn);
	DIE(rc < 0, "w_epoll_add_ptr_in");
}

void receive_data(struct connection* conn) {
	ssize_t bytes_recv;

	while (1) {
		bytes_recv = recv(conn->sockfd, conn->recv_buffer + conn->recv_len,
		                  BUFSIZ - conn->recv_len, 0);
		if (bytes_recv < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				/* no more data at the moment */
				break;
			}
			/* error; remove connection */
			goto remove_connection;
		}
		if (bytes_recv == 0) {
			/* peer closed; we won't remove yet.
			 * we'll parse what we have and potentially send a response. */
			break;
		}
		conn->recv_len += bytes_recv;

		/* if we've filled the buffer, break */
		if (conn->recv_len >= BUFSIZ)
			break;
	}

	conn->state = STATE_REQUEST_RECEIVED;
	return;

remove_connection:
	connection_remove(conn);
}

int connection_open_file(struct connection* conn) {
	conn->fd = open(conn->filename, O_RDONLY);
	if (conn->fd < 0)
		return -1;

	/* get file metadata */
	struct stat st;
	if (fstat(conn->fd, &st) < 0) {
		close(conn->fd);
		conn->fd = -1;
		return -1;
	}

	conn->file_size = st.st_size;
	conn->file_pos = 0;
	conn->async_read_len = 0;

	return 0;
}

void connection_complete_async_io(struct connection* conn) {
	conn->state = STATE_DATA_SENT;
	conn->send_len = conn->async_read_len;
	conn->send_pos = 0;

	conn->file_pos += conn->async_read_len;
	conn->async_read_len = 0;

	/* switch the socket to EPOLLOUT */
	{
		int rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
		DIE(rc < 0, "w_epoll_update_ptr_out");
	}
}

int parse_header(struct connection* conn) {
	/* use mostly null settings except for on_path callback */
	http_parser_settings settings_on_path = { .on_message_begin = 0,
		                                      .on_header_field = 0,
		                                      .on_header_value = 0,
		                                      .on_path = aws_on_path_cb,
		                                      .on_url = 0,
		                                      .on_fragment = 0,
		                                      .on_query_string = 0,
		                                      .on_body = 0,
		                                      .on_headers_complete = 0,
		                                      .on_message_complete = 0 };

	/* parse the received http headers */
	size_t parsed =
	    http_parser_execute(&conn->request_parser, &settings_on_path,
	                        conn->recv_buffer, conn->recv_len);

	/*
	 * if parsed < recv_len, it indicates a parse error or partial parse.
	 * we'll treat that as an error => 404.
	 */
	if (parsed != conn->recv_len) {
		return -1;
	}

	conn->res_type = connection_get_resource_type(conn);
	if (conn->res_type == RESOURCE_TYPE_NONE) {
		return -1;
	}

	/* construct the absolute file path based on the resource type */
	if (conn->res_type == RESOURCE_TYPE_STATIC) {
		size_t prefix_len = strlen(AWS_REL_STATIC_FOLDER);
		if (conn->request_path[0] == '/')
			prefix_len++;
		snprintf(conn->filename, BUFSIZ, "%s%s", AWS_ABS_STATIC_FOLDER,
		         conn->request_path + prefix_len);
	} else {
		size_t prefix_len = strlen(AWS_REL_DYNAMIC_FOLDER);
		if (conn->request_path[0] == '/')
			prefix_len++;
		snprintf(conn->filename, BUFSIZ, "%s%s", AWS_ABS_DYNAMIC_FOLDER,
		         conn->request_path + prefix_len);
	}

	return 0;
}

enum connection_state connection_send_static(struct connection* conn) {
	off_t offset = conn->file_pos;
	ssize_t to_send = conn->file_size - conn->file_pos;
	if (to_send <= 0)
		return STATE_DATA_SENT;

	ssize_t sent = sendfile(conn->sockfd, conn->fd, &offset, to_send);
	if (sent < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return STATE_SENDING_DATA;
		} else {
			perror("sendfile");
			return STATE_CONNECTION_CLOSED; /* terminate on send error */
		}
	}

	conn->file_pos += sent;

	/* check if the entire file has been sent */
	if (conn->file_pos >= conn->file_size)
		return STATE_DATA_SENT;
	return STATE_SENDING_DATA;
}

int connection_send_data(struct connection* conn) {
	while (conn->send_pos < conn->send_len) {
		ssize_t bytes_sent =
		    send(conn->sockfd, conn->send_buffer + conn->send_pos,
		         conn->send_len - conn->send_pos, 0);
		if (bytes_sent < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return -2; /* not ready, would block, retry later */
			}
			return -1; /* fatal error */
		}
		if (bytes_sent == 0) {
			return -1; /* peer closed */
		}
		conn->send_pos += bytes_sent;
	}

	return 0; /* success */
}

int connection_send_dynamic(struct connection* conn) {
	if (connection_open_file(conn) < 0)
		return -1;

	connection_prepare_send_reply_header(conn);
	return 0;
}

void handle_input(struct connection* conn) {
	long ret;
	struct io_event io_event;

	switch (conn->state) {
	case STATE_RECEIVING_DATA:
		/* read from socket */
		receive_data(conn);
		if (conn->state != STATE_REQUEST_RECEIVED)
			break;
		/* fallthrough to parse request */

	case STATE_REQUEST_RECEIVED:
		/* parse the HTTP header */
		if (parse_header(conn) < 0) {
			/* invalid > 404 */
			connection_prepare_send_404(conn);
			{
				int rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
				DIE(rc < 0, "w_epoll_update_ptr_out");
			}
			break;
		}

		/* handle static or dynamic resources */
		if (conn->res_type == RESOURCE_TYPE_STATIC) {
			if (connection_open_file(conn) < 0) {
				/* 404 if can't open file */
				connection_prepare_send_404(conn);
				conn->state = STATE_SENDING_404;
				{
					int rc =
					    w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
					DIE(rc < 0, "w_epoll_update_ptr_out");
				}
				break;
			}
			connection_prepare_send_reply_header(conn);
			conn->state = STATE_SENDING_HEADER;
			{
				int rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
				DIE(rc < 0, "w_epoll_update_ptr_out");
			}
		} else {
			/* dynamic */
			int rc = connection_send_dynamic(conn);
			if (rc < 0) {
				connection_prepare_send_404(conn);
				conn->state = STATE_SENDING_404;
				{
					int rc2 =
					    w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
					DIE(rc2 < 0, "w_epoll_update_ptr_out");
				}
				break;
			}
			{
				int rc2 = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
				DIE(rc2 < 0, "w_epoll_update_ptr_out");
			}
		}
		break;

	case STATE_ASYNC_ONGOING:
		ret = read(conn->eventfd, &ret, sizeof(ret));
		DIE(ret < 0, "read");

		ret = io_getevents(conn->ctx, 1, 1, &io_event, NULL);
		if (ret < 0) {
			perror("io_getevents");
			exit(1);
		}
		/* now finalize the async read => send the chunk */
		connection_complete_async_io((struct connection*)io_event.data);

		break;

	default:
		/* unexpected > remove */
		connection_remove(conn);
		break;
	}
}

void handle_output(struct connection* conn) {
	int rc;
	enum connection_state next_state;

	switch (conn->state) {
	case STATE_SENDING_HEADER:
	case STATE_SENDING_404:
		/* send the header or the 404 body */
		rc = connection_send_data(conn);
		if (rc == 0) {
			/* finished sending the header or 404 */
			if (conn->state == STATE_SENDING_404) {
				conn->state = STATE_404_SENT;
			} else {
				conn->state = STATE_HEADER_SENT;
			}
		} else if (rc == -1) {
			connection_remove(conn);
			break;
		}
		if (conn->state == STATE_404_SENT) {
			/* done > remove */
			connection_remove(conn);
			break;
		}
		/* if we just sent a 200 OK header, move on */
		if (conn->state == STATE_HEADER_SENT) {
			if (conn->res_type == RESOURCE_TYPE_STATIC) {
				conn->state = STATE_SENDING_DATA;
				next_state = connection_send_static(conn);
				if (next_state == STATE_CONNECTION_CLOSED
				    || next_state == STATE_DATA_SENT) {
					connection_remove(conn);
					break;
				}
			} else {
				/* dynamic > start async read */
				conn->state = STATE_ASYNC_ONGOING;
				connection_start_async_io(conn);
				rc = w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);
				DIE(rc < 0, "w_epoll_update_ptr_in");
			}
		}
		break;

	case STATE_SENDING_DATA:
		/* static file partial send */
		next_state = connection_send_static(conn);
		if (next_state == STATE_DATA_SENT
		    || next_state == STATE_CONNECTION_CLOSED) {
			connection_remove(conn);
		}
		break;

	case STATE_DATA_SENT:
		/* dynamic data part was read > we are sending it now */
		if (conn->res_type == RESOURCE_TYPE_DYNAMIC) {
			rc = connection_send_data(conn);
			if (rc == 0) {
				/* portion fully sent > see if there's more */
				if (conn->file_pos >= conn->file_size) {
					connection_remove(conn);
				} else {
					/* read the next portion */
					conn->state = STATE_ASYNC_ONGOING;
					connection_start_async_io(conn);
					rc = w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);
					DIE(rc < 0, "w_epoll_update_ptr_in");
				}
			} else if (rc == -1) {
				connection_remove(conn);
			}
		} else {
			/* static normally won't come here > remove */
			connection_remove(conn);
		}
		break;

	default:
		/* unexpected > remove */
		connection_remove(conn);
		break;
	}
}

void handle_client(uint32_t event, struct connection* conn) {
	if (event & EPOLLIN) {
		handle_input(conn);
	}
	if (event & EPOLLOUT) {
		handle_output(conn);
	}
	if (event & (EPOLLERR | EPOLLHUP)) {
		connection_remove(conn);
	}
}

int main(void) {
	int rc;
	/* initialize aio context */
	memset(&ctx, 0, sizeof(ctx));
	rc = io_setup(1024, &ctx);
	if (rc < 0) {
		perror("io_setup");
		exit(1);
	}

	/* initialize multiplexing */
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");

	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");

	/* server main loop */
	while (1) {
		struct epoll_event rev;

		/* wait for events */
		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");

		if (rev.data.fd == listenfd) {
			if (rev.events & EPOLLIN) {
				handle_new_connection();
			}
		} else {
			if (rev.events & EPOLLIN)
				handle_client(EPOLLIN, rev.data.ptr);
			if (rev.events & EPOLLOUT)
				handle_client(EPOLLOUT, rev.data.ptr);
			if (rev.events & (EPOLLERR | EPOLLHUP))
				handle_client(EPOLLHUP, rev.data.ptr);
		}
	}

	return 0;
}
