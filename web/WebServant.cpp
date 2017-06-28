#include "WebServant.h"
#include "info.h"
#include "HttpConnection.h"
#include "helper/ProxyIdSeqMap.h"
#include "xic/VData.h"
#include "xic/sthread.h"
#include "dlog/dlog.h"
#include "xslib/XError.h"
#include "xslib/path.h"
#include "xslib/hex.h"
#include "xslib/strbuf.h"
#include "xslib/ScopeGuard.h"
#include "xslib/xmalloc.h"
#include "xslib/xstr.h"
#include "xslib/xlog.h"
#include "xslib/md5.h"
#include "xslib/unixfs.h"

#define USER_ONLINE    		"user.online"
#define USER_OFFLINE    	"user.offline"

#define USER_MAX_IDLE_TIME      30
#define USER_KEEP_INTERVAL      180

WebServant::MethodTab::PairType WebServant::_methodpairs[] = {
#define CMD(X)    { #X, XIC_METHOD_CAST(WebServant, X) },
	WEBSERVANT_CMDS
#undef CMD
};
WebServant::MethodTab WebServant::_methodtab(_methodpairs, XS_ARRCOUNT(_methodpairs));


static std::string get_file_content(const std::string& filename)
{
	uint8_t *content = NULL;
	size_t size = 0;
	ON_BLOCK_EXIT(free_pptr<uint8_t>, &content);
	ssize_t len = unixfs_get_content(filename.c_str(), &content, &size);
	if (len < 0)
		return std::string();

	xstr_t xs = XSTR_INIT(content, len);
	xstr_trim(&xs);
	return make_string(xs);
}

WebServant::WebServant(const xic::AdapterPtr& adapter, const SettingPtr& setting)
	: ServantI(&_methodtab), _userMap(1024*1024)
{
	_engine = adapter->getEngine();

	std::string clusterProxy = setting->getString("cluster.proxy");
	if (!clusterProxy.empty())
		_clusterPrx = _engine->stringToProxy(clusterProxy);

	std::string eventProxy = setting->getString("event.proxy");
	if (eventProxy.empty())
		throw XERROR_MSG(XError, "event.proxy not specified in the configuration file");
	_eventPrx = _engine->stringToProxy(eventProxy);

	_keep_cond = st_cond_new();
	_keep_time = 0;

	std::string nameFile = setting->getPathname("Web.NameFile");
	if (nameFile.empty())
		throw XERROR_MSG(XError, "Web.NameFile not specified in the configuration file");

	_httpSubDomain = get_file_content(nameFile);
	if (_httpSubDomain.empty())
		throw XERROR_FMT(XError, "Can't get the content of Web.NameFile \"%s\", or it's empty", nameFile.c_str());

	std::string jsdomains = setting->getString("Web.JavascriptDomains");
	xstr_t tmp = XSTR_CXX(jsdomains);
	xstr_t domain;
	while (xstr_token_cstr(&tmp, ", \t", &domain))
	{
		xstr_strip_cstr(&domain, ".");
		if (domain.len)
			_jsDomains.push_back(domain);
	}

	if (_jsDomains.empty())
		throw XERROR_MSG(XError, "Web.JavascriptDomains not specified in the configuration file");

	wp_set_auth_cookie_name(setting->wantString("Web.Cookie").c_str());
	wp_set_web_long_poll_timeout(setting->getInt("Web.Timeout", 55));

	xref_inc();
	if (_clusterPrx)
	{
		sthread_create(this, &WebServant::renew_fiber, 0);
		sthread_create(this, &WebServant::keep_fiber, 0);
	}
	sthread_create(this, &WebServant::reap_fiber, 0);
	adapter->addServant("wp_web", this);
	_selfPrx = adapter->addServant("wp_web." + _engine->uuid(), this);
	xref_dec_only();
}

WebServant::~WebServant()
{
	st_cond_destroy(_keep_cond);
}

static void fireEvent(const xic::ProxyPtr& eventPrx, int64_t uid, const char* type)
{
	char oid[32];
	sprintf(oid, "%jd", (intmax_t)uid);

	xic::QuestWriter qw("push");
	qw.param("oid", oid);
	qw.param("type", type);
	eventPrx->emitQuest(qw.take(), xic::NULL_COMPLETION);
}

time_t WebServant::time()
{
	return st_time();
}

WebUserPtr WebServant::findUser(int64_t uid)
{
	UserMap::node_type *node = _userMap.find(uid);
	WebUserPtr user;
	if (node)
		user = node->data;
	return user;
}

WebUserPtr WebServant::useUser(int64_t uid)
{
	UserMap::node_type *node = _userMap.use(uid);
	WebUserPtr user;
	if (node)
	{
		user = node->data;
		user->set_keep_time(time());
	}
	return user;
}

WebUserPtr WebServant::removeUser(int64_t uid)
{
	UserMap::node_type *node = _userMap.find(uid);
	WebUserPtr user;
	if (node)
	{
		user = node->data;
		_userMap.remove_node(node);
	}
	return user;
}

int WebServant::http_wait_user_new_msgs(int64_t uid, int seq, int wait, StringSeq& msgs, const std::string& category)
{
	WebUserPtr user = useUser(uid);
	if (!user)
		throw XERROR_FMT(XError, "can not find user(%jd)", uid);

	int rc = user->wait_for_new_msgs(seq, wait, msgs, category);
	user->set_keep_time(time());
	return rc;
}

xic::ProxyPtr WebServant::getOnlineWeb(int64_t uid)
{
	xic::ProxyPtr webPrx;
	try
	{
		xic::ProxyPtr hubPrx = _hubChooser.choose(uid);
		if (hubPrx)
		{
			xic::QuestWriter qw("get_web");
			qw.param("uid", uid);
			xic::AnswerReader ar(hubPrx->request(qw));
			std::string webProxy = make_string(ar.wantXstr("webProxy"));
			if (!webProxy.empty())
			{
				webPrx = _engine->stringToProxy(webProxy);
			}
		}
	}
	catch (std::exception& ex)
	{
		dlog("EXCEPTION", "%s", ex.what());
	}
	return webPrx;
}

XIC_METHOD(WebServant, login)
{
	bool transfer = _clusterPrx && xstr_equal_cstr(&quest->service(), "wp_web");
	xic::VDict args = quest->args();
	int64_t uid = args.wantInt("uid");

	if (transfer)
	{
		xic::ProxyPtr webPrx = getOnlineWeb(uid);
		if (!webPrx)
			webPrx = _webChooser.choose(uid);

		if (webPrx && webPrx != _selfPrx)
		{
			try {
				return webPrx->request(quest);
			}
			catch (std::exception& ex)
			{
				dlog("EXCEPTION", "%s", ex.what());
			}
		}
	}

	time_t now = time();
	WebUserPtr user = findUser(uid);
	if (!user)
	{
		xic::ProxyPtr hubPrx = _hubChooser.choose(uid);
		if (hubPrx)
		{
			xic::QuestWriter qw("web_login");
			qw.param("uid", uid);
			qw.param("webProxy", _selfPrx->str());
			hubPrx->emitQuest(qw.take(), xic::NULL_COMPLETION);
		}

		user = new WebUser();
		_userMap.insert(uid, user);
	}
	user->set_keep_time(now);
	int seq = user->seq();

	// cookie, multi login generate diff cookie
	char buf[32];
	int len = snprintf(buf, sizeof(buf), "%jd", (intmax_t)uid);
	char cookie[64+VERIFY_MAX_SIZE];
	char *p = stpcpy(cookie, wp_auth_cookie_name());
	*p++ = '=';
	len = verify_generate(p, buf, len, now);
	if (len <= 0)
		throw XERROR_FMT(XError, "generate cookie for user(%jd) failed", uid);

	fireEvent(_eventPrx, uid, USER_ONLINE);

	xic::AnswerWriter aw;
	aw.param("web", _httpSubDomain);
	aw.param("cookie", cookie);    
	aw.param("seq", seq);
	return aw;
}

XIC_METHOD(WebServant, logout)
{
	bool transfer = _clusterPrx && xstr_equal_cstr(&quest->service(), "wp_web");
	xic::VDict args = quest->args();
	int64_t uid = args.wantInt("uid");

	if (transfer)
	{
		xic::ProxyPtr webPrx = getOnlineWeb(uid);
		if (webPrx && webPrx != _selfPrx)
		{
			return webPrx->request(quest);
		}
	}

	WebUserPtr user = findUser(uid);
	if (user)
	{
		std::string info = info_serialize(time(), "_.logout");
		user->inform(info);

		xic::ProxyPtr hubPrx = _hubChooser.choose(uid);
		if (hubPrx)
		{
			xic::QuestWriter qw("web_logout");
			qw.param("uid", uid);
			qw.param("webProxy", _selfPrx->str());
			hubPrx->emitQuest(qw.take(), xic::NULL_COMPLETION);
		}
	}

	fireEvent(_eventPrx, uid, USER_OFFLINE);
	return xic::AnswerWriter();
}

XIC_METHOD(WebServant, kickout)
{
	xic::VDict args = quest->args();
	int64_t uid = args.wantInt("uid");
	WebUserPtr user = removeUser(uid);
	if (user)
	{
		std::string info = info_serialize(time(), "_.kickout");
		user->inform(info);
	}
	return xic::ONEWAY_ANSWER;
}

XIC_METHOD(WebServant, get_hub)
{
	xic::VDict args = quest->args();
	int64_t uid = args.wantInt("uid");
	xic::ProxyPtr hubPrx = _hubChooser.choose(uid);
	if (!hubPrx)
		throw XERROR_FMT(XError, "Can't find wp_hub for uid=%jd", (intmax_t)uid);
	return xic::AnswerWriter()("hubProxy", hubPrx->str());
}

XIC_METHOD(WebServant, appoint_web)
{
	xic::VDict args = quest->args();
	int64_t uid = args.wantInt("uid");
	xic::ProxyPtr webPrx = getOnlineWeb(uid);
	bool online = webPrx;
	if (!webPrx)
	{
		online = findUser(uid);
		webPrx = _selfPrx;
	}

	return xic::AnswerWriter()("webProxy", webPrx->str())("online", online);
}

XIC_METHOD(WebServant, get_user_msgs)
{
	bool transfer = _clusterPrx && xstr_equal_cstr(&quest->service(), "wp_web");
	xic::VDict args = quest->args();
	int64_t uid = args.wantInt("uid");

	if (transfer)
	{
		xic::ProxyPtr webPrx = getOnlineWeb(uid);
		if (webPrx && webPrx != _selfPrx)
		{
			return webPrx->request(quest);
		}
	}

	WebUserPtr user = findUser(uid);
	if (!user)
		throw XERROR_FMT(XError, "can not find user(%jd)", uid);

	StringSeq msgs;
	int rc = user->get_msgs(msgs);

	xic::AnswerWriter aw;
	aw.param("web", _httpSubDomain);
	aw.param("seq", rc);
	aw.paramVList("infos", msgs);
	return aw;
}

XIC_METHOD(WebServant, send_to_user)
{
	bool transfer = _clusterPrx && xstr_equal_cstr(&quest->service(), "wp_web");
	xic::VDict args = quest->args();
	int64_t uid = args.wantInt("uid");
	const xstr_t& category = args.wantXstr("c");
	const vbs_dict_t *msg = args.want_dict("o");

	if (transfer)
	{
		xic::ProxyPtr hubPrx = _hubChooser.choose(uid);
		if (hubPrx)
			return hubPrx->request(quest);
	}

	WebUserPtr user = findUser(uid);
	if (user)
	{
		std::string info = info_serialize(time(), make_string(category), msg);
		user->inform(info);
	}
	return xic::AnswerWriter()("ok", bool(user));
}

XIC_METHOD(WebServant, send_to_users)
{
	bool transfer = _clusterPrx && xstr_equal_cstr(&quest->service(), "wp_web");
	xic::VDict args = quest->args();
	xic::VList uids = args.wantVList("uids");
	const xstr_t& category = args.wantXstr("c");
	const vbs_dict_t *msg = args.want_dict("o");

	if (transfer)
	{
		ProxyIdSeqMap pkmap;
		for (xic::VList::Node node = uids.first(); node; node = node.next())
		{
			int64_t uid = node.intValue();
			xic::ProxyPtr hubPrx = _hubChooser.choose(uid);
			if (hubPrx)
				pkmap.add(hubPrx, uid);
			else
				dlog("WIM_ERROR", "Can't find wp_hub for uid=%jd", (intmax_t)uid);
		}

		for (ProxyIdSeqMap::iterator iter = pkmap.begin(); iter != pkmap.end(); ++iter)
		{
			const xic::ProxyPtr& hubPrx = iter->first;
			std::vector<int64_t>& uids = iter->second;
			xic::QuestWriter qw("send_to_users");
			qw.param("c", category);
			qw.param("o", msg);
			qw.paramVList("uids", uids);
			hubPrx->emitQuest(qw.take(), xic::NULL_COMPLETION);
		}
	}
	else
	{
		std::string info = info_serialize(time(), make_string(category), msg);
		for (xic::VList::Node node = uids.first(); node; node = node.next())
		{
			int64_t uid = node.intValue();
			WebUserPtr user = findUser(uid);
			if (!user)
				continue;

			user->inform(info);
		}
	}
	return xic::AnswerWriter();
}

bool WebServant::renew_proxy(ProxyChooser &chooser, const char* proxy)
{
	xic::QuestWriter qw("getProxies");
	qw.param("prefix", proxy);
	xic::AnswerReader ar(_clusterPrx->request(qw.take()));
	int64_t revision = ar.wantInt("revision");
	if (revision == _revision)
		return false;

	std::vector<xstr_t> proxies;
	ar.wantXstrSeq("proxies", proxies);
	size_t size = proxies.size();
	std::vector<xic::ProxyPtr> prxs;
	prxs.reserve(size);
	for (size_t i = 0; i < size; ++i)
	{
		xic::ProxyPtr prx = _engine->stringToProxy(make_string(proxies[i]));
		prxs.push_back(prx);
	}
	return chooser.update(prxs);
}

void WebServant::renew_fiber()
{
	while (true)
	try
	{
		xic::QuestWriter qw("renew");
		qw.param("proxy", _selfPrx->str());

		xic::AnswerReader ar(_clusterPrx->request(qw.take()));
		int64_t revision = ar.wantInt("revision");
		if (revision != _revision)
		{
			if (renew_proxy(_hubChooser, "wp_hub"))
			{    
				st_cond_signal(_keep_cond);
			}
			renew_proxy(_webChooser, "wp_web");
			_revision = revision;
		}

		if (time() > _keep_time + USER_KEEP_INTERVAL)
			st_cond_signal(_keep_cond);

		if (st_sleep(3) != 0)
			break;
	}
	catch (std::exception& ex)
	{
		dlog("EXCEPTION", "%s", ex.what());
		if (st_sleep(2) != 0)
			break;
	}
}

void WebServant::keep_fiber()
{
	while (true)
	{
		if (st_cond_wait(_keep_cond) != 0)
			break;

		try
		{
			_keep_time = time();
			keep_users();
		}
		catch (std::exception& ex)
		{
			dlog("EXCEPTION", "%s", ex.what());
		}
	}
}

void WebServant::reap_fiber()
{
	while (true)
	{
		try
		{
			reap_users();
		}
		catch (std::exception& ex)
		{
			dlog("EXCEPTION", "%s", ex.what());
		}

		dlog("ONLINE", "Online User number:%jd", _userMap.count());
		if (st_sleep(20) != 0)
			break;
	}
}

void WebServant::reap_users()
{
	time_t expire = time() - USER_MAX_IDLE_TIME;
	ProxyIdSeqMap pkmap;
	UserMap::node_type *next = NULL;
	for (UserMap::node_type *node = _userMap.most_stale(); node; node = next)
	{
		next = _userMap.next_stale(node);
		WebUserPtr& user = node->data;
		if (user->waiter() == 0 && user->keep_time() < expire)
		{
			if (_clusterPrx)
			{
				int64_t uid = node->key;
				xic::ProxyPtr hubPrx = _hubChooser.choose(uid);
				if (hubPrx)
					pkmap.add(hubPrx, uid);
			}
			_userMap.remove_node(node);
		}
	}

	for (ProxyIdSeqMap::iterator iter = pkmap.begin(); iter != pkmap.end(); ++iter)
	{
		const xic::ProxyPtr& prx = iter->first;
		xic::QuestWriter qw("web_drop_users");
		qw.param("webProxy", _selfPrx->str());
		qw.paramVList("uids");
		prx->emitQuest(qw.take(), xic::NULL_COMPLETION);
	}
}

void WebServant::keep_users()
{
	ProxyIdSeqMap pkmap;
	for (UserMap::node_type *node = _userMap.most_fresh(); node; node = _userMap.next_fresh(node))
	{
		int64_t uid = node->key;
		xic::ProxyPtr prx = _hubChooser.choose(uid);
		pkmap.add(prx, uid);
	}

	for (ProxyIdSeqMap::iterator iter = pkmap.begin(); iter != pkmap.end(); ++iter)
	{
		const xic::ProxyPtr& prx = iter->first;
		xic::QuestWriter qw("web_keep_users");
		qw.param("webProxy", _selfPrx->str());
		qw.paramVList("uids", iter->second);
		prx->emitQuest(qw.take(), xic::NULL_COMPLETION);
	}
}

