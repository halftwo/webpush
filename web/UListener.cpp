#include "UListener.h"
#include "xic/Engine.h"
#include "xic/sthread.h"
#include "xslib/xnet.h"
#include "xslib/xlog.h"
#include "dlog/dlog.h"
#include <sstream>
#include <string>

static std::string get_netloc(int fd)
{
	std::ostringstream ss;
	uint32_t ip;
	uint16_t port = xnet_get_sock_ipv4(fd, &ip);

	if (port)
	{
		int sotype = -1;
		socklen_t sotypelen = sizeof(sotype);
		const char *type = "unknown";

		if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &sotype, &sotypelen) == 0)
		{
			if (sotype == SOCK_STREAM)
				type = "tcp";
			else if (sotype == SOCK_DGRAM)
				type = "udp";
		}

		char ipstr[16];
		if (ip)
		{
			xnet_ipv4_ntoa(ip, ipstr);
			ss << " @ " << type << ':' << ipstr << ':' << port;
		}
		else
		{
			uint32_t ips[16];
			int n = xnet_ipv4_get_all(ips, 16);
			for (int i = 0; i < n; ++i)
			{
				xnet_ipv4_ntoa(ips[i], ipstr);
				ss << " @ " << type << ':' << ipstr << ':' << port;
			}
		}
	}
	return ss.str();
}

UListener::UListener(unsigned short port)
{
	_port = port;
	int fd = xnet_tcp_bind(NULL, _port);
        if (fd < 0)
                throw XERROR_FMT(XError, "xnet_tcp_bind() failed, errno=%d, port=%d", errno, _port);
        _sf = st_netfd_open(fd);
	_netloc = get_netloc(st_netfd_fileno(_sf));
}

UListener::~UListener()
{
	st_netfd_close(_sf);
}

void UListener::start()
{
	if (::listen(st_netfd_fileno(_sf), 256) < 0)
                throw XERROR_FMT(XError, "listen() failed, errno=%d", errno);

	xref_inc();
	sthread_create(this, &UListener::listen_fiber, 0);
}

void UListener::accepted(st_netfd_t sf)
{
	st_netfd_close(sf);
}

void UListener::listen_fiber()
{
	while (true)
	{
		struct sockaddr_in peer_addr;
		int peer_len = sizeof(peer_addr);
		st_netfd_t sf = st_accept(_sf, (struct sockaddr *)&peer_addr, &peer_len, ST_UTIME_NO_TIMEOUT);
		if (sf == NULL)
		{
			dlog("ERROR", "st_accept error:errno=%d", errno);
			if (errno == ECONNABORTED) //ignore
				continue;
			break;
		}
		accepted(sf);
	}
	xref_dec();
	exit(1);
}


