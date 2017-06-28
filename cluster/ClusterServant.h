#ifndef ClusterServant_h_
#define ClusterServant_h_

#include "xic/ServantI.h"
#include <map>

#define CLUSTERSERVANT_CMDS	\
	CMD(renew)		\
	CMD(revision)		\
	CMD(getProxies)		\
	CMD(remove)		\
	/* END OF LIST */

class ClusterServant: public xic::ServantI
{
	static MethodTab::PairType _methodpairs[];
	static MethodTab _methodtab;

	typedef std::map<std::string, time_t> ProxyMap;
	ProxyMap _proxyMap;
	int64_t _revision;

public:
	ClusterServant();
	virtual ~ClusterServant();

	void reap_fiber();

#define CMD(X)	XIC_METHOD_DECLARE(X);
	CLUSTERSERVANT_CMDS
#undef CMD
};

typedef XPtr<ClusterServant> ClusterServantPtr;

#endif
