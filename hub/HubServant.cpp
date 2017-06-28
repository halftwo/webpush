#include "HubServant.h"
#include "helper/ProxyIdSeqMap.h"
#include "dlog/dlog.h"
#include "xslib/Setting.h"
#include "xslib/xlog.h"
#include "xic/sthread.h"
#include <vector>
#include <set>
#include <map>

#define USER_MAX_IDLE_TIME      300
#define USER_ONLINE    		"user.online"
#define USER_OFFLINE    	"user.offline"

typedef std::map<xic::ProxyPtr, std::vector<xic::QuestPtr> > TheMap;
typedef std::map<int64_t, std::vector<int64_t> > OMap;
typedef std::map<xic::ProxyPtr, IntMap > MMap;

HubServant::MethodTab::PairType HubServant::_methodpairs[] = {
#define CMD(X)    { #X, XIC_METHOD_CAST(HubServant, X) },
	HUBSERVANT_CMDS
#undef CMD
};

HubServant::MethodTab HubServant::_methodtab(_methodpairs, XS_ARRCOUNT(_methodpairs));

HubServant::HubServant(const xic::AdapterPtr& adapter, const SettingPtr& setting)
	: ServantI(&_methodtab), _engine(adapter->getEngine()), _revision(0), _userMap(1024*1024, INT_MAX)
{
	_clusterPrx = _engine->stringToProxy(setting->getString("cluster.proxy"));
	_eventPrx = _engine->stringToProxy(setting->getString("event.proxy"));

	xref_inc();
	sthread_create(this, &HubServant::renew_fiber, 0);
	sthread_create(this, &HubServant::reap_fiber, 0);
	adapter->addServant("wp_hub", this);
	_selfPrx = adapter->addServant("wp_hub." + _engine->uuid(), this);
	xref_dec_only();
}

HubServant::~HubServant()
{
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

XIC_METHOD(HubServant, get_web)
{
	xic::VDict args = quest->args();
	int64_t uid = args.wantInt("uid");
	UserMap::node_type *node = _userMap.find(uid);
	xic::ProxyPtr webPrx;
	if (node)
	{
		webPrx = node->data->webPrx;
		if (_webSet.find(webPrx) == _webSet.end())
		{
			_userMap.remove_node(node);
			webPrx.reset();
		}
	}

	return xic::AnswerWriter()("webProxy", webPrx ? webPrx->str() : "");
}

XIC_METHOD(HubServant, web_keep_users)
{
	xic::VDict args = quest->args();
	std::string webProxy = make_string(args.wantXstr("webProxy"));
	xic::ProxyPtr webPrx = _engine->stringToProxy(webProxy);
	_webSet.insert(webPrx);

	std::vector<int64_t> uids;
	args.wantIntSeq("uids", uids);

	time_t now = st_time();
	for (size_t i = 0; i < uids.size(); ++i)
	{
		int64_t uid = uids[i];
		UserMap::node_type *user = _userMap.use(uid);
		if (!user)
		{
			user = _userMap.insert(uid, UserOriginPtr(new UserOrigin(now)));
			user->data->webPrx = webPrx;
			fireEvent(_eventPrx, uid, USER_ONLINE);
		}
		else
		{
			UserOriginPtr& uo = user->data;
			uo->keep_time = now;
			if (uo->webPrx != webPrx)
			{
				xic::QuestWriter qw("kickout", false);
				qw.param("uid", uid);
				uo->webPrx->emitQuest(qw.take(), xic::NULL_COMPLETION);
				uo->webPrx = webPrx;
			}
		}
	}

	return xic::AnswerWriter();
}

XIC_METHOD(HubServant, web_drop_users)
{
	xic::VDict args = quest->args();
	std::string webProxy = make_string(args.wantXstr("webProxy"));
	xic::ProxyPtr webPrx = _engine->stringToProxy(webProxy);

	std::vector<int64_t> uids;
	args.wantIntSeq("uids", uids);

	time_t now = st_time();
	for (size_t i = 0; i < uids.size(); ++i)
	{
		int64_t uid = uids[i];
		UserMap::node_type *user = _userMap.find(uid);
		if (user && user->data->webPrx == webPrx)
		{
			dlog("OFFLINE", "uid=%jd online_time=%d", uid, (int)(now - user->data->create_time));
			_userMap.remove_node(user);
			fireEvent(_eventPrx, uid, USER_OFFLINE);
		}
	}

	return xic::AnswerWriter();
}

XIC_METHOD(HubServant, web_login)
{
	xic::VDict args = quest->args();
	int64_t uid = args.wantInt("uid");
	std::string webProxy = make_string(args.wantXstr("webProxy"));
	xic::ProxyPtr webPrx = _engine->stringToProxy(webProxy);
	_webSet.insert(webPrx);

	time_t now = st_time();
	UserMap::node_type *node = _userMap.use(uid);
	if (!node)
	{
		node = _userMap.insert(uid, UserOriginPtr(new UserOrigin(now)));
		node->data->webPrx = webPrx;
	}
	else
	{
		UserOriginPtr& uo = node->data;
		uo->keep_time = now;
		if (uo->webPrx != webPrx)
		{
			xic::QuestWriter qw("kickout", false);
			qw.param("uid", uid);
			uo->webPrx->emitQuest(qw.take(), xic::NULL_COMPLETION);
			uo->webPrx = webPrx;
		}
	}

	return xic::AnswerWriter();
}

XIC_METHOD(HubServant, web_logout)
{
	xic::VDict args = quest->args();
	int64_t uid = args.wantInt("uid");
	xic::ProxyPtr webPrx = _engine->stringToProxy(make_string(args.wantXstr("webProxy")));

	UserMap::node_type *node = _userMap.find(uid);
	if (node)
	{
		if (node->data->webPrx == webPrx)
		{
			dlog("OFFLINE", "uid=%jd online_time=%d", uid, (int)(st_time() - node->data->create_time));
			_userMap.remove_node(node);
		}
	}

	return xic::AnswerWriter();
}

XIC_METHOD(HubServant, send_to_user)
{
	xic::VDict args = quest->args();
	int64_t uid = args.wantInt("uid");

	UserMap::node_type *node = _userMap.find(uid);
	if (node)
	{
		const xic::ProxyPtr& webPrx = node->data->webPrx;
		if (_webSet.find(webPrx) == _webSet.end())
		{
			_userMap.remove_node(node);
			node = NULL;
		}
		else
		{
			node->data->webPrx->emitQuest(quest, xic::NULL_COMPLETION);
		}
	}

	return xic::AnswerWriter()("ok", bool(node));
}

XIC_METHOD(HubServant, send_to_users)
{
	xic::VDict args = quest->args();
	const xstr_t& category = args.wantXstr("c");
	const vbs_dict_t *msg = args.want_dict("o");
	std::vector<int64_t> uids;
	args.wantIntSeq("uids", uids);

	ProxyIdSeqMap pkmap;
	for (size_t i = 0; i < uids.size(); ++i)
	{
		int64_t uid = uids[i];
		UserMap::node_type *node = _userMap.find(uid);
		if (!node)
			continue;
		pkmap.add(node->data->webPrx, uid);
	}

	for (ProxyIdSeqMap::iterator iter = pkmap.begin(); iter != pkmap.end(); ++iter)
	{
		const xic::ProxyPtr& prx = iter->first;
		std::vector<int64_t>& uids = iter->second;
		xic::QuestWriter qw("send_to_users");

		qw.param("c", category);
		qw.param("o", msg);
		qw.paramVList("uids", uids);
		prx->emitQuest(qw.take(), xic::NULL_COMPLETION);
	}
	return xic::AnswerWriter();
}


void HubServant::sync_webs()
{
	xic::QuestWriter qw("getProxies");
	qw.param("prefix", "wp_web");
	xic::AnswerReader ar(_clusterPrx->request(qw.take()));

	int64_t revision = ar.wantInt("revision");
	if (revision == _revision)
		return;

	std::vector<xstr_t> proxies;
	ar.wantXstrSeq("proxies", proxies);
	std::set<xic::ProxyPtr> webSet;
	for (size_t i = 0; i < proxies.size(); ++i)
	{
		xic::ProxyPtr prx = _engine->stringToProxy(make_string(proxies[i]));
		webSet.insert(prx);
	}
	_webSet.swap(webSet);
}

bool HubServant::renew_proxy(ProxyChooser &chooser, const char* proxy)
{
	xic::QuestWriter qw("getProxies");
	qw.param("prefix", proxy);
	xic::AnswerReader ar(_clusterPrx->request(qw.take()));

	int64_t revision = ar.wantInt("revision");
	if (revision == _revision)
		return true;

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


void HubServant::renew_fiber()
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
			renew_proxy(_hubChooser, "wp_hub");
			sync_webs();
			_revision = revision;
		}

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

void HubServant::reap_fiber()
{
	while (true)
	try
	{
		reap_user();
		if (st_sleep(20) != 0)
			break;
	}
	catch (std::exception& ex)
	{
		dlog("EXCEPTION", "%s", ex.what());
		if (st_sleep(2) != 0)
			break;
	}
}

void HubServant::reap_user()
{
	time_t now = st_time();
	time_t expire = now - USER_MAX_IDLE_TIME;
	while (true)
	{
		UserMap::node_type *node = _userMap.most_stale();
		if (!node)
			break;

		UserOriginPtr& uo = node->data;
		// If the most stale user is still alive, no need to continue.
		if (uo->keep_time > expire)
			break;

		int64_t uid = node->key;
		dlog("REAP_USER", "uid=%jd online_time=%d", uid, (int)(now - uo->create_time));
		_userMap.remove_node(node);
	}

	dlog("ONLINE", "Online User number:%jd", _userMap.count());
}

