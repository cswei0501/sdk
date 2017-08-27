#include "http-server-internal.h"
#include "aio-tcp-transport.h"
#include "http-reason.h"
#include "http-parser.h"
#include "http-bundle.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#if defined(OS_WINDOWS)
#define iov_base	buf
#define iov_len		len
#endif

static void http_session_bundle_clear(struct http_session_t *session);
static int http_session_bundle_fill(struct http_session_t *session, void** bundles, int num);
static socket_bufvec_t* socket_bufvec_alloc(struct http_session_t *session, int count);

static const char* s_http_header_end = "\r\n";

static void http_session_ondestroy(void* param)
{
	struct http_session_t *session;
	session = (struct http_session_t *)param;
	session->transport = NULL;

	if (session->parser)
	{
		http_parser_destroy(session->parser);
		session->parser = NULL;
	}

	if (session->__vec)
	{
		free(session->__vec);
		session->__vec = NULL;
		session->vec_capacity = 0;
	}

	if (session->header != (char*)(session + 1))
	{
		assert(session->header_capacity > 2 * 1024);
		free(session->header);
		session->header = NULL;
		session->header_size = 0;
		session->header_capacity = 0;
	}

#if defined(DEBUG) || defined(_DEBUG)
	memset(session, 0xCC, sizeof(*session));
#endif
	free(session);
}

static void http_session_onrecv(void* param, const void* data, size_t bytes)
{
	int r;
	size_t remain;
	struct http_session_t *session;
	session = (struct http_session_t *)param;

	remain = bytes;
	do
	{
		r = http_parser_input(session->parser, (char*)data + (bytes - remain), &remain);
		if (0 == r)
		{
			// clear for save user-defined header
			session->header_size = 0;

			// call
			// user must reply(send/send_vec/send_file) in handle
			if (session->handler)
			{
				const char* uri = http_get_request_uri(session->parser);
				const char* method = http_get_request_method(session->parser);
				session->handler(session->param, session, method, uri);
			}

			http_parser_clear(session->parser); // reset parser
		}
	} while (remain > 0 && r >= 0);
}

static void http_session_onsend(void* param, int code, size_t bytes)
{
	struct http_session_t *session;
	session = (struct http_session_t*)param;
	http_session_bundle_clear(session);
	(void)code; (void)bytes;
}

int http_session_create(struct http_server_t *server, socket_t socket, const struct sockaddr* sa, socklen_t salen)
{
	struct http_session_t *session;
	struct aio_tcp_transport_handler_t handler;

	handler.ondestroy = http_session_ondestroy;
	handler.onrecv = http_session_onrecv;
	handler.onsend = http_session_onsend;

	session = (struct http_session_t *)malloc(sizeof(*session) + 2 * 1024);
	if (!session) return -1;

	memset(session, 0, sizeof(*session));
	session->header_capacity = 2 * 1024;
	session->header = (char*)(session + 1);
	session->parser = http_parser_create(HTTP_PARSER_SERVER);
	assert(AF_INET == sa->sa_family || AF_INET6 == sa->sa_family);
	assert(salen <= sizeof(session->addr));
	memcpy(&session->addr, sa, salen);
	session->addrlen = salen;
	session->param = server->param;
	session->handler = server->handler;
	session->transport = aio_tcp_transport_create(socket, &handler, session);
	return aio_tcp_transport_start(session->transport);
}

int http_server_send(struct http_session_t *session, int code, void* bundle)
{
	return http_server_send_vec(session, code, bundle ? &bundle : NULL, bundle ? 1 : 0);
}

int http_server_send_vec(struct http_session_t *session, int code, void** bundles, int num)
{
	int r;
	char content_length[32];
	r = http_session_bundle_fill(session, bundles, num);
	if (r < 0) 
		return r;

	// HTTP Response Header
	r = snprintf(content_length, sizeof(content_length) , "%d", r);
	http_session_add_header(session, "Content-Length", content_length, r);

	r = snprintf(session->status_line, sizeof(session->status_line), "HTTP/1.1 %d %s\r\n", code, http_reason_phrase(code));
	socket_setbufvec(session->vec, 0, session->status_line, r);
	socket_setbufvec(session->vec, 1, session->header, session->header_size);
	socket_setbufvec(session->vec, 2, (void*)s_http_header_end, 2);

	r = aio_tcp_transport_sendv(session->transport, session->vec, session->vec_count);
	if (0 != r) http_session_bundle_clear(session);
	return r;
}

static socket_bufvec_t* socket_bufvec_alloc(struct http_session_t *session, int count)
{
	void* p;
	if (count <= 5)
		return session->vec5;

	if (count > session->vec_capacity)
	{
		p = realloc(session->__vec, count * sizeof(socket_bufvec_t));
		if (NULL == p)
			return NULL;

		session->__vec = (socket_bufvec_t *)p;
		session->vec_capacity = count;
	}

	return session->__vec;
}

static int http_session_bundle_fill(struct http_session_t *session, void** bundles, int num)
{
	int i, len = 0;
	struct http_bundle_t *bundle;

	assert(NULL == session->vec);
	assert(0 == session->vec_count);
	session->vec = socket_bufvec_alloc(session, num + 3);
	if (!session->vec || num < 0)
		return -1;

	// HTTP Response Data
	for (i = 0; i < num; i++)
	{
		bundle = (struct http_bundle_t*)bundles[i];
		assert(bundle->len > 0);
		http_bundle_addref(bundle); // addref
		socket_setbufvec(session->vec, i + 3, bundle->ptr, bundle->len);
		len += bundle->len;
	}

	session->vec_count = num + 3;
	return len;
}

static void http_session_bundle_clear(struct http_session_t *session)
{
	int i;
	for (i = 3; i < session->vec_count; i++)
	{
		//http_bundle_free(session->vec[i].buf);
		http_bundle_free((struct http_bundle_t *)session->vec[i].iov_base - 1);
	}
	session->vec_count = 0;
	session->vec = NULL;
}

int http_session_add_header(struct http_session_t* session, const char* name, const char* value, size_t bytes)
{
	void* ptr;
	size_t len;
	len = strlen(name ? name : "");
	if (len < 1 || bytes < 1)
		return -EINVAL;

	if (session->header_size + len + bytes + 4 > session->header_capacity)
	{
		if (session->header == (char*)(session + 1))
		{
			ptr = malloc(session->header_capacity + len + bytes + 2 * 1024);
			if (ptr)
				memcpy(ptr, session->header, session->header_size);
		}
		else
		{
			ptr = realloc(session->header, session->header_capacity + len + bytes + 2 * 1024);
		}

		if (!ptr)
			return -ENOMEM;
		session->header = ptr;
		session->header_capacity += len + bytes + 2 * 1024;
	}

	session->header_size += snprintf(session->header + session->header_size, session->header_capacity - session->header_size, "%s: %s\r\n", name, value ? value : "");
	return 0;
}
