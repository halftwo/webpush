#include "ProxyChooser.h"
#include "xslib/crc64.h"
#include <algorithm>


ProxyChooser::ProxyChooser()
{
	_carp = NULL;
}

ProxyChooser::~ProxyChooser()
{
	delete _carp;
}

bool ProxyChooser::update(const std::vector<xic::ProxyPtr>& prxs)
{
	size_t size = prxs.size();
	assert(size < UINT16_MAX);
	std::vector<xic::ProxyPtr> vec = prxs;
	stable_sort(vec.begin(), vec.end());

	std::vector<xic::ProxyPtr> diff(vec.size() + _pvec.size());
	std::vector<xic::ProxyPtr>::iterator iter = 
		set_symmetric_difference(vec.begin(), vec.end(), _pvec.begin(), _pvec.end(), diff.begin());

	if (iter - diff.begin())
	{
		CarpSequence *carp = NULL;
		if (size > 1)
		{
			std::vector<uint64_t> members;
			members.reserve(size);
			for (size_t i = 0; i < size; ++i)
			{
				const std::string& s = vec[i]->str();
				uint64_t m = crc64_checksum(s.data(), s.length());
				members.push_back(m);
			}
			carp = new CarpSequence(&members[0], members.size(), 0xFFFF);
			carp->enable_cache();
		}

		delete _carp;
		_carp = carp;
		_pvec.swap(vec);
		return true;
	}

	return false;
}

xic::ProxyPtr ProxyChooser::choose(int64_t uid)
{
	xic::ProxyPtr prx;
	if (_carp)
	{
		int which = _carp->which(uid);
		prx = _pvec[which];
	}
	else if (_pvec.size())
	{
		prx = _pvec[0];
	}

	return prx;
}

