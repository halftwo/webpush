#ifndef ProxyChooser_h_
#define ProxyChooser_h_

#include "xic/Engine.h"
#include "xslib/CarpSequence.h"
#include <vector>

class ProxyChooser
{
	std::vector<xic::ProxyPtr> _pvec;
	CarpSequence *_carp;

public:
	ProxyChooser();
	~ProxyChooser();

	bool update(const std::vector<xic::ProxyPtr>& proxies);

	xic::ProxyPtr choose(int64_t uid);
};

#endif
