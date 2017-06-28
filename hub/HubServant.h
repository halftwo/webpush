#include <string>

#include "dlog/dlog.h"
#include "helper/ProxyChooser.h"
#include "xslib/LruHashMap.h"
#include "xslib/Setting.h"
#include "xslib/xlog.h"
#include "xic/ServantI.h"

#define HUBSERVANT_CMDS        	\
	CMD(get_web)    	\
	CMD(web_keep_users)   	\
	CMD(web_drop_users)    	\
	CMD(web_login)        	\
	CMD(web_logout)        	\
	CMD(send_to_user)     	\
	CMD(send_to_users)     	\
	/* END OF CMDS */


typedef std::map<int64_t, int64_t> IntMap;
typedef std::set<int64_t> IntSet;

class HubServant: public xic::ServantI
{
	static MethodTab::PairType _methodpairs[];
	static MethodTab _methodtab;

	xic::EnginePtr _engine;
	xic::ProxyPtr _selfPrx;
	xic::ProxyPtr _clusterPrx;
	xic::ProxyPtr _eventPrx;

	std::set<xic::ProxyPtr> _webSet;
	ProxyChooser _hubChooser;
	int64_t _revision;

public:
	HubServant(const xic::AdapterPtr& adapter, const SettingPtr& setting);
	virtual ~HubServant();

private:
	void renew_fiber();
	void sync_webs();
	bool renew_proxy(ProxyChooser &chooser, const char* proxy);
	void reap_fiber();
	void reap_user();

	struct UserOrigin: public XRefCount
	{
		time_t create_time;
		time_t keep_time;
		xic::ProxyPtr webPrx;

		UserOrigin(time_t create)
			: create_time(create), keep_time(create)
		{
		}
	};
	typedef XPtr<UserOrigin> UserOriginPtr;
	typedef LruHashMap<int64_t, UserOriginPtr> UserMap;
	UserMap _userMap;

#define CMD(X)    XIC_METHOD_DECLARE(X);
	HUBSERVANT_CMDS
#undef CMD
};

