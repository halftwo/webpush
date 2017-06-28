#ifndef HTTPREQ_H_
#define HTTPREQ_H_

#include "xslib/rope.h"
#include "xslib/ostk.h"
#include "xslib/iobuf.h"
#include "xslib/httpcode.h"
#include "xslib/XError.h"
#include "xslib/XRefCount.h"


XE_(XError, HttpError);


class HttpReq;
class HttpReply;
typedef XPtr<HttpReq> HttpReqPtr;
typedef XPtr<HttpReply> HttpReplyPtr;


class HttpReq: public XRefCount
{
	struct Cookie {
		xstr_t name;
		xstr_t value;
	};

	HttpReq(ostk_t *ostk);
	virtual ~HttpReq();
	virtual void xref_destroy();

	void _copy(xstr_t& dst, const xstr_t& src);

	static HttpReqPtr create();
public:
	ostk_t *_ostk;
	rope_t headers;

	xstr_t method;
	xstr_t uri;
	xstr_t script_name;
	xstr_t query_string;
	xstr_t host;
	xstr_t user_agent;
	xstr_t referer;
	xstr_t content_type;
	xstr_t content;
	bool chunked;

	Cookie *cookies;
	size_t num_cookie;
	bool keep_alive;

public:
	static HttpReqPtr fromIobuf(iobuf_t *ib, const char* peer_ip, uint16_t peer_port);
};

class HttpReply: public XRefCount
{
	ostk_t *_ostk;
	HttpReqPtr _req;
	int _code;
	bool _finished;
	rope_t _header_rope;
	rope_t _body_rope;
	struct iovec *_iov;
	int _iov_count;

	HttpReply(ostk_t *ostk, const HttpReqPtr& req);
	virtual void xref_destroy();
public:
	static HttpReplyPtr create(const HttpReqPtr& req);;

	void httpcode(int code);

	void header_puts(const char *s);
	void header_puts(const xstr_t& s);
	void header_printf(const char *fmt, ...);

	void body_puts(const char *s);
	void body_puts(const xstr_t& s);
	void body_printf(const char *fmt, ...);

	void clear_body();
	void finish();
	struct iovec* get_iovec(int* count);

	rope_t *body_rope() 			{ return &_body_rope; }
};


#endif
