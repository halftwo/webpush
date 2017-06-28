#ifndef WebServant_h_
#define WebServant_h_

#include "WebUser.h"
#include "verify.h"
#include "helper/ProxyChooser.h"
#include "xslib/LruHashMap.h"
#include "xic/ServantI.h"
#include "xslib/uuid.h"
#include <string>
#include <memory>
#include <map>

class WebServant;
typedef XPtr<WebServant> WebServantPtr;


#define WEBSERVANT_CMDS		\
	CMD(login)		\
	CMD(logout)		\
	CMD(kickout)		\
	CMD(get_hub)		\
	CMD(appoint_web)	\
	CMD(get_user_msgs)	\
	CMD(send_to_user)	\
	CMD(send_to_users)	\
	/* END OF CMDS */


class WebServant: public xic::ServantI
{
	static MethodTab::PairType _methodpairs[];
	static MethodTab _methodtab;

	xic::EnginePtr _engine;
	xic::ProxyPtr _selfPrx;
	xic::ProxyPtr _clusterPrx;
	xic::ProxyPtr _eventPrx;

	std::string _httpSubDomain;
	std::vector<xstr_t> _jsDomains;

	typedef LruHashMap<int64_t, WebUserPtr> UserMap;
	UserMap _userMap;

	st_cond_t _keep_cond;
	time_t _keep_time;

	ProxyChooser _hubChooser;
	ProxyChooser _webChooser;
	int64_t _revision;

public:
	WebServant(const xic::AdapterPtr& adapter, const SettingPtr& setting);
	virtual ~WebServant();

	const std::vector<xstr_t>& jsDomains() const	{ return _jsDomains; }
	time_t time();
	WebUserPtr findUser(int64_t uid);
	WebUserPtr useUser(int64_t uid);
	WebUserPtr removeUser(int64_t uid);
	int http_wait_user_new_msgs(int64_t uid, int seq, int wait, StringSeq& msgs, const std::string& category);

private:
	void renew_fiber();
	void keep_fiber();
	void reap_fiber();

	void keep_users();
	void reap_users();
	bool renew_proxy(ProxyChooser &chooser, const char* proxy);
	xic::ProxyPtr getOnlineWeb(int64_t uid);

#define CMD(X)	XIC_METHOD_DECLARE(X);
	WEBSERVANT_CMDS
#undef CMD
};


#endif
