#include "ctypedef.h"
#include "cstringext.h"
#include "string-util.h"
#include "http-header-host.h"
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <assert.h>

int http_header_host(const char* field, char host[], size_t bytes, unsigned short *port)
{
	const char* p;

	p = strchr(field, ':');
	if(p)
	{
		if(bytes < (size_t)(p - field))
			return -1;

		memcpy(host, field, p-field-1);
		host[p-field] = '\0';
		*port = (unsigned short)atoi(p+1);
	}
	else
	{
		if(bytes < strlen(field) + 1)
			return -1;

		strcpy(host, field);
	}

	return 0;
}

#if defined(_DEBUG) || defined(DEBUG)
void http_header_host_test(void)
{
	char host[16] = {0};
	unsigned short port = 0;

	assert(0 == http_header_host("www.baidu.com", host, sizeof(host), &port) && 0==port && 0==strcmp("www.baidu.com", host));
	assert(0 == http_header_host("www.baidu.com:80", host, sizeof(host), &port) && 80==port && 0==strcmp("www.baidu.com", host));

	port = 0;
	assert(0 == http_header_host("114.21.2.11", host, sizeof(host), &port) && 0==port && 0==strcmp("114.21.2.11", host));
	assert(0 == http_header_host("114.21.2.11:8081", host, sizeof(host), &port) && 8081==port && 0==strcmp("114.21.2.11", host));

	assert(0 != http_header_host("www.verylongdnsname:com:80", host, sizeof(host), &port));
}
#endif
