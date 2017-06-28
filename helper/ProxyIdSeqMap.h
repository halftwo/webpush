#ifndef PKMap_h_
#define PKMap_h_

#include "xic/Engine.h"
#include <vector>
#include <map>

class ProxyIdSeqMap
{
	typedef std::map<xic::ProxyPtr, std::vector<int64_t> > TheMap;
	TheMap _map;

public:
	void add(const xic::ProxyPtr& prx, int64_t kid);

	size_t size() const 		{ return _map.size(); }
	void swap(ProxyIdSeqMap& pm)	{ _map.swap(pm._map); }

	typedef TheMap::iterator iterator;
	iterator begin() 		{ return _map.begin(); }
	iterator end() 			{ return _map.end(); }
};

#endif
