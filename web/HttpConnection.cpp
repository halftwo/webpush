#include "HttpConnection.h"
#include "HttpReq.h"
#include "dlog/dlog.h"
#include "xslib/urlparse.h"
#include "xslib/xlog.h"
#include "xslib/xnet.h"
#include "xslib/hex.h"
#include "xslib/escape.h"
#include "xslib/vbs_json.h"

#define WAIT_MIN	5
#define WAIT_DFT	55
#define WAIT_MAX	900

static int _web_long_poll_timeout = WAIT_DFT;
static char _auth_cookie_name[64] = "wp_permit";

void wp_set_web_long_poll_timeout(int seconds)
{
	if (seconds >= WAIT_MIN && seconds <= WAIT_MAX)
		_web_long_poll_timeout = seconds;
}

void wp_set_auth_cookie_name(const char *cookie_name)
{
	strncpy(_auth_cookie_name, cookie_name, sizeof(_auth_cookie_name));
	_auth_cookie_name[sizeof(_auth_cookie_name)-1] = 0;
}

const char* wp_auth_cookie_name()
{
	return _auth_cookie_name;
}


static xstr_t json_meta = XSTR_CONST("\r\n\t\b\f\v\0\"\\");

static int _json_escape_char(xio_write_function xio_write, void *cookie, unsigned char ch)
{
	static xstr_t json_subst_array[] = {
		XSTR_CONST("\\r"),
		XSTR_CONST("\\n"),
		XSTR_CONST("\\t"),
		XSTR_CONST("\\b"),
		XSTR_CONST("\\f"),
		XSTR_CONST("\\v"),
		XSTR_CONST("\\u0000"),
	};

	unsigned char *p = (unsigned char *)memchr(json_meta.data, ch, json_meta.len);
	if (!p)
		return 0;

	int n = p - json_meta.data;
	if (n < (int)XS_ARRCOUNT(json_subst_array))
	{
		const xstr_t& xs = json_subst_array[n];
		xio_write(cookie, xs.data, xs.len);
	}
	else
	{
		char buf[2];
		buf[0] = '\\';
		buf[1] = ch;
		xio_write(cookie, buf, 2);
	}
	return 1;
}

static void json_escape(xio_write_function xio_write, void *cookie, const xstr_t& xs)
{
	static bset_t json_bset = make_bset_by_add_xstr(&empty_bset, &json_meta);
	escape_xstr(xio_write, cookie, &json_bset, _json_escape_char, &xs);
}


static void *handle_sthread(void *arg)
{
	HttpConnection *con = (HttpConnection *)arg;
	void *r = con->handle_fiber();
	con->xref_dec();
	return r;
}

static ssize_t st_xio_read(void *cookie, void *data, size_t size)
{
	return read(st_netfd_fileno((st_netfd_t)cookie), data, size);
}

static ssize_t st_xio_write(void *cookie, const void *data, size_t size)
{
	return write(st_netfd_fileno((st_netfd_t)cookie), data, size);
}

const xio_t st_xio = {
	st_xio_read,
	st_xio_write,
	NULL,
	NULL,
};

HttpConnection::HttpConnection(const XPtr<WebServant>& ws, st_netfd_t sf)
	: _ws(ws), _sf(sf)
{
	_peer_port = xnet_get_peer_ip_port(st_netfd_fileno(sf), _peer_ip);

	iobuf_init(&_ib, &st_xio, _sf, _ibuf, sizeof(_ibuf));
	xref_inc();
	_handle_sth = st_thread_create(handle_sthread, this, 0, 0);
}

HttpConnection::~HttpConnection()
{
	if (_sf)
		st_netfd_close(_sf);
}

void *HttpConnection::handle_fiber()
{
	try {
		bool keep_alive = true;
		while (keep_alive)
		{
			keep_alive = process_http_request();
		}
	}
	catch (std::exception& ex)
	{
		dlog("WP_WEB_ERROR", "%s+%d %s", _peer_ip, _peer_port, ex.what());
	}

	_handle_sth = 0;
	return NULL;
}


static void urldecode(xstr_t* xs)
{
	char *s = (char *)xs->data;
	char *end = s + xs->len;
	char *d = (char *)xs->data;
	while (s < end)
	{
		if (*s == '%' && s + 2 < end && isxdigit(s[1]) && isxdigit(s[2]))
		{
			char dst[1];
			unhexlify(dst, s+1, 2);
			s += 3;
			*d++ = dst[0];
		}
		else
		{
			*d++ = *s++;
		}
	}
	xs->len = d - (char *)xs->data;
}

#define HEX_CHARS    "0123456789ABCDEF"
static int _encode(xio_write_function xio_write, void *cookie, unsigned char ch)
{
	char buf[3];
	buf[0] = '%';
	buf[1] = HEX_CHARS[ch >> 4];
	buf[2] = HEX_CHARS[ch & 0xf];
	return xio_write(cookie, buf, 3);
}

static void urlencode(xio_write_function xio_write, void *cookie, const xstr_t& xs)
{
	static bset_t meta_bset = make_bset_by_add_cstr(&cntrl_bset, " <>'\"");
	escape_xstr(xio_write, cookie, &meta_bset, _encode, &xs);
}

static bool is_valid_url(const std::vector<xstr_t>& domains, const xstr_t& url)
{
	static bset_t valid_bset = make_bset_by_add_cstr(&alnum_bset, "./-_,!|");

	struct urlpart part;
	if (urlparse(&url, &part) < 0)
		return false;

	if (!xstr_equal_cstr(&part.scheme, "http")
		|| part.host.len == 0
		|| part.path.len == 0
		|| part.query.len != 0
		|| part.fragment.len != 0)
	{
		return false;
	}

	if (xstr_find_not_in_bset(&part.path, 0, &valid_bset) >= 0)
		return false;

	if (domains.size() == 0)
		return true;

	for (size_t i = 0; i < domains.size(); ++i)
	{
		const xstr_t& domain = domains[i];
		if (domain.len > 0 && xstr_end_with(&part.host, &domain))
		{
			if (part.host.len == domain.len || xstr_char_equal(&part.host, -domain.len-1, '.'))
			{
				return true;
			}
		}
	}

	return false;
}

static void http_answer_error(HttpReplyPtr reply, const char *fmt, ...)
{
	va_list ap;
	char buf[256];

	va_start(ap, fmt);
	size_t len = xfmt_vsnprintf(NULL, buf, sizeof(buf), fmt, ap);
	va_end(ap);

	reply->body_puts("{\"t\":\"error\", \"detail\":\"");
	if (len > 0)
	{
		xstr_t ss = XSTR_INIT((unsigned char *)buf, (len < sizeof(buf) ? len : sizeof(buf)-1));
		json_escape(rope_xio.write, reply->body_rope(), ss);
	}
	reply->body_puts("\"}");
}

static void verify_cookie(const HttpReq *req, const xstr_t& user)
{
	if (!req || !req->cookies || req->num_cookie == 0)
	{
		throw XERROR_MSG(XError, "No cookie given"); 
	}

	bool found = false;
	for (unsigned int i = 0; i < req->num_cookie; ++i)
	{
		const xstr_t& name = req->cookies[i].name;
		const xstr_t& value = req->cookies[i].value;
		if (xstr_equal_cstr(&name, _auth_cookie_name))
		{
			found = true;
			int t = verify_check((char *)value.data, value.len, (char *)user.data, user.len);
			if (t < 0)
				throw XERROR_FMT(XError, "Cookie \"%s\" invalid", _auth_cookie_name);

			if (t + (3600 * 24) < st_time())
				throw XERROR_FMT(XError, "Cookie \"%s\" expired", _auth_cookie_name);

			break;
		}
	}

	if (!found)
		throw XERROR_FMT(XError, "Cookie \"%s\" not found", _auth_cookie_name);
}

static void http_answer_refresh(HttpReplyPtr reply, int seq)
{
	reply->body_printf("{\"t\":\"refresh\", \"seq\":%d}", seq);
}

static void http_answer_inform(HttpReplyPtr reply, const StringSeq& msgs, int seq)
{
	reply->body_printf("{\"t\":\"inform\", \"seq\":%d, \"msgs\":[", seq);
	for (size_t i = 0; i < msgs.size(); ++i)
	{
		if (i != 0)
			reply->body_puts(",");

		const xstr_t& xs = make_xstr(msgs[i]);
		vbs_to_json(xs.data, xs.len, rope_xio.write, reply->body_rope(), 0);
	}
	reply->body_puts(" ]}");
}

static void http_reply_iframe(HttpReplyPtr reply, const HttpReq *req, const std::vector<xstr_t>& js_domains)
{
	reply->body_puts("<html><head>");
	reply->body_puts("<meta charset=\"utf-8\">");

	bool not_allowed_found = false;

	xstr_t it;
	xstr_t qry = req->query_string;
	while (xstr_delimit_char(&qry, '&', &it))
	{
		xstr_t name;
		xstr_delimit_char(&it, '=', &name);
		if (xstr_equal_cstr(&name, "r") && it.len)
		{
			urldecode(&it);
			if (is_valid_url(js_domains, it))
			{
				reply->body_puts("<script type=\"text/javascript\" src=\"");
				reply->body_puts(it);
				reply->body_puts("\"></script>");
			}
			else
			{
				dlog("NOT_ALLOWED_JS", "%.*s", XSTR_P(&it));
				not_allowed_found = true;
				reply->body_puts("<!-- not-allowed-url: ");
				urlencode(rope_xio.write, reply->body_rope(), it);
				reply->body_puts(" -->");
			}
		}
	}
	reply->body_puts("</head><body></body></html>");

	if (not_allowed_found)
		reply->header_puts("Cache-Control: max-age=60\r\n");
	else
		reply->header_puts("Cache-Control: max-age=86400\r\n");
}

static int rope_xfmt(iobuf_t *ob, const xfmt_spec_t *spec, void *p)
{
        if (xstr_equal_cstr(&spec->ext, "ROPE"))
        {
                rope_t *rope = (rope_t *)p;
                rope_dump(rope, (xio_write_function)iobuf_write, ob);
                return 0;
        }
        return -1;
}

bool HttpConnection::process_http_request()
{
	HttpReqPtr tmp = HttpReq::fromIobuf(&_ib, _peer_ip, _peer_port);
	if (!tmp)
	return false;

	bool keep_alive = false;
	HttpReplyPtr reply = HttpReply::create(tmp);

	try
	{
		HttpReq *req = tmp.get();

		keep_alive = req->keep_alive;
		reply->header_printf("Connection: %s\r\n", keep_alive ? "keep-alive" : "close");

		if (xstr_case_equal_cstr(&req->method, "GET"))
		{
			// URL: /ifr/js?r=&r= 
			xstr_t what = req->script_name;
			if (xstr_char_equal(&what, 0, '/'))
				xstr_advance(&what, 1);

			xstr_t op;
			xstr_delimit_char(&what, '/', &op);

			if (xstr_equal_cstr(&op, "ifr"))
			{
				reply->header_puts("Content-Type: text/html\r\n");
				if (xstr_equal_cstr(&what, "js"))
					http_reply_iframe(reply, req, _ws->jsDomains());
				else
					throw XERROR_CODE_FMT(HttpError, HTTPCODE_NOT_FOUND, "%.*s", XSTR_P(&req->script_name));
			}
			else
			{
				reply->header_puts("Content-Type: application/json\r\n");

				if (!xstr_equal_cstr(&op, "k"))
					throw XERROR_CODE_FMT(HttpError, HTTPCODE_NOT_FOUND, "%.*s", XSTR_P(&req->script_name));

				reply->header_puts("Cache-Control: private, no-cache, max-age=0\r\n");
				xstr_t rand, user;
				xstr_delimit_char(&what, '/', &rand);
				xstr_delimit_char(&what, '/', &user);

				verify_cookie(req, user);

				xstr_t end;
				int64_t uid = xstr_to_long(&user, &end, 0);
				if (uid == 0 || end.len)
					throw XERROR_FMT(XError, "uid error, %.*s", XSTR_P(&req->script_name));

				xstr_t next;
				xstr_delimit_char(&what, '/', &next);
				if (what.len)
					throw XERROR_FMT(XError, "uri error, %.*s", XSTR_P(&req->script_name));

				if (xstr_equal_cstr(&op, "k"))
				{
					int wait = _web_long_poll_timeout;
					std::string category;
					xstr_t qstr = req->query_string;
					xstr_t param;
					while (xstr_delimit_char(&qstr, '&', &param))
					{
						xstr_t key;
						xstr_delimit_char(&param, '=', &key);

						if (xstr_equal_cstr(&key, "w"))
						{
							wait = xstr_atoi(&param);
						}
						else if (xstr_equal_cstr(&key, "c"))
						{
							category = make_string(param);
						}
					}
					wait = XS_CLAMP(wait, WAIT_MIN, WAIT_MAX);

					int seq = xstr_to_long(&next, &end, 10);
					StringSeq msgs;
					int rc = _ws->http_wait_user_new_msgs(uid, seq, wait, msgs, category);
					if (msgs.size())
						http_answer_inform(reply, msgs, rc);
					else
						http_answer_refresh(reply, rc > 0 ? rc : seq);
				}
				else
					throw XERROR_FMT(XError, "invalid uri, %.*s", XSTR_P(&req->script_name));
			}
		}
		else
		{
			throw XERROR_CODE_FMT(HttpError, HTTPCODE_METHOD_NOT_ALLOWED, "%.*s", XSTR_P(&req->method));
		}
	}
	catch (HttpError& ex)
	{
		dlog("WP_WEB_ERROR", "%s+%d %s", _peer_ip, _peer_port, ex.what());
		reply->httpcode(ex.code());
		reply->body_puts(ex.message().c_str());
		keep_alive = false;
	}
	catch (XError &ex)
	{
		dlog("WP_WEB_ERROR", "%s+%d %s", _peer_ip, _peer_port, ex.what());
		http_answer_error(reply, "error at %d: %s", __LINE__, ex.message().c_str());
		keep_alive = false;
	}

	reply->finish();

	exlog(3, rope_xfmt, "WP_WEB_RESP %s+%d %p{>ROPE<}", _peer_ip, _peer_port, reply->body_rope());

	int count = 0;
	struct iovec* iov = reply->get_iovec(&count);

	int rc = st_writev_resid(_sf, &iov, &count, 60*1000000);
	if (rc != 0)
	{
		throw XERROR_FMT(XSyscallError, "st_writev_resid=%d peer=%s+%d errno=%d", rc, _peer_ip, _peer_port, errno);
	}
	return keep_alive;
}


HttpListener::HttpListener(const SettingPtr& setting, const XPtr<WebServant>& ws)
    : UListener(setting->getInt("Web.Port")), _ws(ws)
{
}

void HttpListener::accepted(st_netfd_t sf)
{
	new HttpConnection(_ws, sf);
}

