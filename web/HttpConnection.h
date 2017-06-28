#ifndef HttpConnection_h_
#define HttpConnection_h_

#include "WebServant.h"
#include "xslib/XRefCount.h"
#include "UListener.h"
#include <st.h>


extern void wp_set_web_long_poll_timeout(int seconds);
extern void wp_set_auth_cookie_name(const char *cookie_name);
extern const char* wp_auth_cookie_name();


class HttpListener: public UListener
{
	XPtr<WebServant> _ws;
public:
	HttpListener(const SettingPtr& setting, const XPtr<WebServant>& wsMan);
 
	virtual void accepted(st_netfd_t sf);
};
typedef XPtr<HttpListener> HttpListenerPtr;


class HttpConnection: virtual public XRefCount
{
protected:
	XPtr<WebServant> _ws;
	st_netfd_t _sf;
	iobuf_t _ib;
	unsigned char _ibuf[1024*4];
	st_thread_t _handle_sth;
	char _peer_ip[16];
	uint16_t _peer_port;
public:
	HttpConnection(const XPtr<WebServant>& ws, st_netfd_t sf);
	virtual ~HttpConnection();

	virtual void *handle_fiber();

private:
	virtual bool process_http_request();
};
typedef XPtr<HttpConnection> HttpConnectionPtr;


#endif
