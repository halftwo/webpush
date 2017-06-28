#include "ProxyIdSeqMap.h"

void ProxyIdSeqMap::add(const xic::ProxyPtr& prx, int64_t kid)
{
	if (prx)
	{
		ProxyIdSeqMap::iterator iter = _map.find(prx);
		if (iter == _map.end())
			iter = _map.insert(_map.end(), std::make_pair(prx, std::vector<int64_t>()));
		iter->second.push_back(kid);
	}
}

