#include "HttpReq.h"
#include "dlog/dlog.h"
#include "xslib/xatomic.h"
#include "xslib/rdtsc.h"
#include "xslib/xlog.h"
#include "xslib/ScopeGuard.h"
#include <st.h>
#include <string.h>
#include <sys/uio.h>
#include <pthread.h>
#include <ctype.h>


#define SERVER_NAME	"WP"
#define CHUNK_SIZE	2048
#define MAX_CONTENT_LEN	(1024*1024*16)


static char *httpdate()
{
	/* Date: Tue, 09 Jun 2009 08:59:50 GMT */
	static const char *_months[] = { 
		"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};
	static const char *_wdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
	static uint64_t _last_tsc;
	static char _buf[32];

	uint64_t tsc = rdtsc();
	if (tsc - _last_tsc > cpu_frequency())
	{
		struct tm tm;
		time_t t = time(NULL);
		_last_tsc = tsc;
		gmtime_r(&t, &tm);
		xfmt_snprintf(NULL, _buf, sizeof(_buf), "%s, %02d %s %d %02d:%02d:%02d GMT",
			_wdays[tm.tm_wday], tm.tm_mday, _months[tm.tm_mon], tm.tm_year + 1900,
			tm.tm_hour, tm.tm_min, tm.tm_sec);
	}
	return _buf;
}


HttpReq::HttpReq(ostk_t *ostk)
{
	this->_ostk = ostk;
	rope_init(&headers, 0, &ostk_xmem, ostk);

	method = xstr_null;
	uri = xstr_null;
	script_name = xstr_null;
	query_string = xstr_null;
	host = xstr_null;
	user_agent = xstr_null;
	referer = xstr_null;
	content_type = xstr_null;
	content = xstr_null;

	cookies = NULL;
	num_cookie = 0;
	keep_alive = false;
	chunked = false;
}

HttpReq::~HttpReq()
{
}

void HttpReq::xref_destroy()
{
	ostk_t *o = this->_ostk;
	this->~HttpReq();
	ostk_destroy(o);
}

HttpReqPtr HttpReq::create()
{
	ostk_t *ostk = ostk_create(CHUNK_SIZE);
	void *p = ostk_alloc(ostk, sizeof(HttpReq));
	return HttpReqPtr(new(p) HttpReq(ostk));
}

// '\r\n' trimed
static int _get_line(iobuf_t *ib, xstr_t *buf, int timeout)
{
	while (true)
	{
		char *line;
		int n = iobuf_getline(ib, &line);
		if (n < 0)
		{
			if (n == -2)
				return 0;

			throw XERROR_FMT(XError, "iobuf_getline()=%d", n);
		}
		else if (n > 0)
		{
			--n;
			if (n > 0 && line[n-1] == '\r')
				--n;

			buf->data = (unsigned char *)line;
			buf->len = n;
			return 1;
		}

		int rc = st_netfd_poll((st_netfd_t)ib->cookie, POLLIN, timeout * 1000000);
		if (rc != 0)
		{
			throw XERROR_FMT(XError, "st_netfd_poll()=%d", rc);
		}
	}

	assert(!"Can't reach here!");
	return -1;
}

// '\r\n' trimed
static int _get_lines(iobuf_t *ib, xstr_t *lines, int count, xstr_t *buf, int timeout)
{
	int i = 0;
	char *end = NULL;

	assert(count > 0);
	buf->data = NULL;
	while (i < count)
	{
		char *line;
		int n = iobuf_getdelim(ib, &line, '\n', i == 0);
		if (n < 0)
		{
			if (n == -2)
				break;

			throw XERROR_FMT(XError, "iobuf_getdelim()=%d", n);
		}
		else if (n > 0)
		{
			if (i == 0)
				buf->data = (unsigned char *)line;
			end = line + n;

			--n;
			if (n > 0 && line[n-1] == '\r')
				--n;

			xstr_init(&lines[i++], (unsigned char *)line, n);
			if (n == 0)
				break;
		}
		else
		{
			if (i > 0)
				break;

			int rc = st_netfd_poll((st_netfd_t)ib->cookie, POLLIN, timeout * 1000000);
			if (rc != 0)
			{
				throw XERROR_FMT(XError, "st_netfd_poll()=%d", rc);
			}
		}
	}

	buf->len = (unsigned char *)end - buf->data;
	return i;
}

static void _read_content(iobuf_t *ib, xstr_t *content, int timeout)
{
	ssize_t len = 0; 
	while (len < content->len)
	{
		int n = iobuf_read(ib, content->data + len, content->len - len);
		if (n < 0)
			throw XERROR_FMT(XError, "iobuf_read()=%d", n);
		else if (n > 0)
		{
			len += n;
		}
		else
		{
			int rc = st_netfd_poll((st_netfd_t)ib->cookie, POLLIN, timeout * 1000000);
			if (rc != 0)
			{
				throw XERROR_FMT(XError, "st_netfd_poll()=%d", rc);
			}
		}
	}
}

static char* _read_chunked(iobuf_t *ib, ssize_t &len, int timeout)
{
	xstr_t line;
	len = 0;
	char *result = NULL;
	while (_get_line(ib, &line, 60) > 0)
	{
		int c = xstr_to_long(&line, NULL, 16);
		if (c == 0)
		{
			//read the last CRLF
			_get_line(ib, &line, 60);
			break;
		}
		if (!result)
		{
			result = (char*)malloc(c);
			if (!result)
				throw XERROR(XMemoryError);
		}
		else 
		{
			char* t = (char*)realloc(result, len + c);
			if (!t)
				throw XERROR(XMemoryError);
			result = t;
		}
		xstr_t content;
		content.data = (unsigned char *)result + len;
		content.len = c;
		
		_read_content(ib, &content, timeout);
		len += c;
		//read CRLF
		_get_line(ib, &line, 60);
	}
	return result;
}

void HttpReq::_copy(xstr_t& dst, const xstr_t& src)
{
	dst.len = src.len;
	dst.data = (unsigned char *)ostk_copy(this->_ostk, src.data, src.len);
}

HttpReqPtr HttpReq::fromIobuf(iobuf_t *ib, const char* peer_ip, uint16_t peer_port)
{
	HttpReqPtr ret;
	HttpReq *req;

	xstr_t reqline;
	int rc = _get_line(ib, &reqline, 60);
	if (rc <= 0)
		return ret;

	xlog(3, "WIM_REQ %s+%d %.*s", peer_ip, peer_port, XSTR_P(&reqline));
	xstr_t method, uri;
	xstr_delimit_char(&reqline, ' ', &method);
	xstr_delimit_char(&reqline, ' ', &uri);
	if (!xstr_case_start_with_cstr(&reqline, "HTTP/1."))
		throw XERROR_FMT(XError, "Unknown HTTP protocol (%.*s)", XSTR_P(&reqline));

	ret = HttpReq::create();
	req = ret.get();

	req->keep_alive = xstr_case_equal_cstr(&reqline, "HTTP/1.1");
	req->_copy(req->method, method);
	req->_copy(req->uri, uri);
	req->query_string = req->uri;
	xstr_delimit_char(&req->query_string, '?', &req->script_name);

	while (true)
	{
		xstr_t lines[32];
		xstr_t buf;
		int cnt = _get_lines(ib, lines, 32, &buf, 60);
		if (cnt <= 0)
			throw XERROR_FMT(XError, "Unexpected connection close");

		bool header_finish = false;
		if (lines[cnt-1].len == 0)
		{
			--cnt;
			header_finish = true;
		}

		rope_block_t *rb = rope_append_block(&req->headers, buf.data, buf.len);
		ssize_t delta = rb->buf - buf.data;
		for (int i = 0; i < cnt; ++i)
		{
			xstr_t line = lines[i];
			line.data += delta;

			xstr_t name, value, tmp = line;
			xstr_key_value(&tmp, ':', &name, &value);
			if (name.len == 0 || value.len == 0)
				throw XERROR_FMT(XError, "Invalid HTTP Header (%.*s)", XSTR_P(&line));

			if (xstr_case_equal_cstr(&name, "Content-Type"))
			{
				req->content_type = value;
			}
			else if (xstr_case_equal_cstr(&name, "Content-Length"))
			{
				req->content.len = xstr_atoi(&value);
				if (req->content.len > MAX_CONTENT_LEN)
					throw XERROR_FMT(XError, "Content too big (%zd)", req->content.len);
			}
			else if (xstr_case_equal_cstr(&name, "Host"))
			{
				req->host = value;
			}
			else if (xstr_case_equal_cstr(&name, "User-Agent"))
			{
				req->user_agent = value;
			}
			else if (xstr_case_equal_cstr(&name, "Referer"))
			{
				req->referer = value;
			}
			else if (xstr_case_equal_cstr(&name, "Transfer-Encoding") && xstr_case_equal_cstr(&value, "chunked"))
			{
				req->chunked = true;
			}
			else if (xstr_case_equal_cstr(&name, "Cookie"))
			{
				xlog(3, "WIM_REQ %s+%d %.*s:%.*s", peer_ip, peer_port, XSTR_P(&name), XSTR_P(&value));
				xstr_t ck, cv;
				int n = xstr_count_char(&value, ';');
				Cookie *cookies = (Cookie *)ostk_alloc(req->_ostk, (req->num_cookie + n + 1) * sizeof(Cookie));
				if (req->num_cookie)
					memcpy(cookies, req->cookies, req->num_cookie * sizeof(Cookie));
				req->cookies = cookies;
				while (xstr_delimit_char(&value, ';', &cv))
				{
					xstr_trim(&cv);
					xstr_delimit_char(&cv, '=', &ck);
					if (ck.len && cv.len)
					{
						req->cookies[req->num_cookie].name = ck;
						req->cookies[req->num_cookie].value = cv;
						++req->num_cookie;
					}
				}
			}
			else if (xstr_case_equal_cstr(&name, "Connection"))
			{
				if (xstr_case_equal_cstr(&value, "close"))
					req->keep_alive = false;
				else if (xstr_case_equal_cstr(&value, "keep-alive"))
					req->keep_alive = true;
			}
		}

		if (header_finish)
			break;
	}

	if (req->content.len > 0)
	{
		req->content.data = (unsigned char *)ostk_alloc(req->_ostk, req->content.len);
		if (!req->content.data)
			throw XERROR(XMemoryError);

		_read_content(ib, &req->content, 60);
	}
	else if (req->chunked)
	{
		char* content = _read_chunked(ib, req->content.len, 60);
		if (content)
		{
			req->content.data = (unsigned char *)ostk_copy(req->_ostk, content, req->content.len);
			free(content);
		}
	}

	return ret;
}


HttpReply::HttpReply(ostk_t *ostk, const HttpReqPtr& req)
	: _ostk(ostk), _req(req)
{
	_code = 200;
	_finished = false;
	_iov = NULL;
	_iov_count = 0;

	rope_init(&_header_rope, 200, &ostk_xmem, _ostk);
	rope_init(&_body_rope, 200, &ostk_xmem, _ostk);
}

void HttpReply::xref_destroy()
{
	ostk_t *ostk = this->_ostk;
	this->~HttpReply();
	ostk_destroy(ostk);
}

HttpReplyPtr HttpReply::create(const HttpReqPtr& req)
{
	ostk_t *ostk = ostk_create(CHUNK_SIZE);
	void *p = ostk_alloc(ostk, sizeof(HttpReply));
	return HttpReplyPtr(new(p) HttpReply(ostk, req));
}

void HttpReply::httpcode(int code)
{
	_code = code;
}

void HttpReply::header_puts(const char *s)
{
	rope_puts(&_header_rope, s);
}

void HttpReply::header_puts(const xstr_t& s)
{
	rope_write(&_header_rope, s.data, s.len);
}

void HttpReply::header_printf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vxformat(NULL, rope_xio.write, &_header_rope, buf, sizeof(buf), fmt, ap);
	va_end(ap);
}

void HttpReply::body_puts(const char *s)
{
	rope_puts(&_body_rope, s);
}

void HttpReply::body_puts(const xstr_t& s)
{
	rope_write(&_body_rope, s.data, s.len);
}

void HttpReply::body_printf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vxformat(NULL, rope_xio.write, &_body_rope, buf, sizeof(buf), fmt, ap);
	va_end(ap);
}

void HttpReply::clear_body()
{
	rope_clear(&_body_rope);
}

void HttpReply::finish()
{
	if (_finished)
		return;
	_finished = true;
	
	header_puts("\r\n");

	char buf[128];
	int len = snprintf(buf, sizeof(buf), "HTTP/1.1 %d %s\r\n"
		"Date: %s\r\nServer: %s\r\n"
		"Content-Length: %zd\r\n",
		_code, httpcode_description(_code),
		httpdate(), SERVER_NAME,
		_body_rope.length);

	rope_insert(&_header_rope, 0, buf, len);
}

struct iovec* HttpReply::get_iovec(int* count)
{
	if (!_finished)
		finish();
	
	if (!_iov)
	{
		_iov_count = _header_rope.block_count + _body_rope.block_count;
		_iov = (struct iovec *)ostk_alloc(_ostk, sizeof(*_iov) * _iov_count);
		rope_iovec(&_header_rope, _iov);
		rope_iovec(&_body_rope, &_iov[_header_rope.block_count]);
	}

	*count = _iov_count;
	return _iov;
}

