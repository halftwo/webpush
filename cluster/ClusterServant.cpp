#include "ClusterServant.h"
#include "xic/sthread.h"

#define DELIMIT_CHARS	"+:/.#"

ClusterServant::MethodTab::PairType ClusterServant::_methodpairs[] = {
#define CMD(X)	{ #X, XIC_METHOD_CAST(ClusterServant, X) },
	CLUSTERSERVANT_CMDS
#undef CMD
};

ClusterServant::MethodTab ClusterServant::_methodtab(_methodpairs, XS_ARRCOUNT(_methodpairs));


ClusterServant::ClusterServant()
	: ServantI(&_methodtab)
{
	_revision = ((int64_t)st_time()) * 1000;
	sthread_create(this, &ClusterServant::reap_fiber, 0);
}

ClusterServant::~ClusterServant()
{
}

void ClusterServant::reap_fiber()
{
	while (true)
	{
		st_sleep(5);
		time_t now = st_time();
		ProxyMap::iterator iter = _proxyMap.begin();
		while (iter != _proxyMap.end())
		{
			if (iter->second < now - 60)
			{
				_proxyMap.erase(iter++);
				++_revision;
			}
			else
				++iter;
		}
	}
}

XIC_METHOD(ClusterServant, remove)
{
	xic::VDict args = quest->args();
	xstr_t proxy = args.wantXstr("proxy");
	std::string the_proxy = make_string(proxy);

	xic::AnswerWriter aw;
	ProxyMap::iterator iter = _proxyMap.find(the_proxy);
	if (iter != _proxyMap.end())
	{
		_proxyMap.erase(iter);
		++_revision;
		aw.param("ok", true);
	}
	else
	{
		aw.param("ok", false);
	}

	return aw.take();
}

static void get_service_prefix(xstr_t* tmp, xstr_t* prefix)
{
	xstr_delimit_in_cstr(tmp, DELIMIT_CHARS, prefix);
	if (prefix->len == 0)
	{
		xstr_delimit_char(tmp, '@', prefix);
		prefix->len++;
		prefix->data--;
	}
	else
	{
		xstr_delimit_char(tmp, '@', NULL);
		prefix->len++;
	}
}

XIC_METHOD(ClusterServant, renew)
{
	xic::VDict args = quest->args();
	xstr_t proxy = args.wantXstr("proxy");

	time_t now = st_time();
	std::string the_proxy = make_string(proxy);

	ProxyMap::iterator iter = _proxyMap.find(the_proxy);
	if (iter != _proxyMap.end())
	{
		iter->second = now;
	}
	else
	{
		xstr_t tmp = proxy, tmpservname;
		xstr_t endpoint, servname;
		xstr_t netlocs[8];
		size_t num = 0;

		get_service_prefix(&tmp, &tmpservname);

		while (num < 8 && xstr_delimit_char(&tmp, '@', &endpoint))
		{
			xstr_token_cstr(&endpoint, " \t\r\n\v\f", &netlocs[num]);
			if (netlocs[num].len)
				++num;
		}

		if (num > 0)
		{
			for (iter = _proxyMap.begin(); iter != _proxyMap.end(); ++iter)
			{
				//服务名一样
				tmp = make_xstr(iter->first);
				get_service_prefix(&tmp, &servname);
				if (!xstr_equal(&servname, &tmpservname))
				{
					continue;
				}

				//服务的地址一样
				while (xstr_delimit_char(&tmp, '@', &endpoint))
				{
					xstr_t loc;
					xstr_token_cstr(&endpoint, " \t\r\n\v\f", &loc);
					for (size_t i = 0; i < num; ++i)
					{
						if (xstr_equal(&loc, &netlocs[i]))
						{
							_proxyMap.erase(iter);
							goto done;
						}
					}
				}
			}
		done:
			/* Do nothing */
			;
		}

		_proxyMap.insert(std::make_pair(the_proxy, now));
		++_revision;
	}

	xic::AnswerWriter aw;
	aw.param("revision", _revision);
	return aw.take();
}

XIC_METHOD(ClusterServant, revision)
{
	return xic::AnswerWriter()("revision", _revision);
}

XIC_METHOD(ClusterServant, getProxies)
{
	xic::VDict args = quest->args();
	xstr_t prefix = args.getXstr("prefix");

	xic::AnswerWriter aw;
	aw.param("revision", _revision);
	xic::VListWriter lw = aw.paramVList("proxies");
	for (ProxyMap::iterator iter = _proxyMap.begin(); iter != _proxyMap.end(); ++iter)
	{
		xstr_t proxy = make_xstr(iter->first);
		if (prefix.len == 0 || xstr_start_with(&proxy, &prefix))
		{
			lw.v(proxy);
		}
	}
	return aw.take();
}

