#ifndef WebUser_h_
#define WebUser_h_

#include "xic/VData.h"
#include "xslib/XRefCount.h"
#include "xslib/xatomic.h"
#include <st.h>
#include <string>
#include <deque>
#include <vector>

typedef std::vector<std::string> StringSeq;

class WebUser: public XRefCount
{
	time_t _keep_time;
	int _seq;
	xatomic_t _waiter;
	std::deque<std::string> _msgs;
	st_cond_t _cond;

public:
	WebUser();
	~WebUser();

	time_t keep_time() const		{ return _keep_time; }
	void set_keep_time(time_t t)		{ _keep_time = t; }

	int seq() const				{ return _seq; }
	int waiter() const			{ return xatomic_get(&_waiter); }

	int inform(const std::string& msg);

	int get_msgs(StringSeq& msgs);

	int wait_for_new_msgs(int seq, int wait, StringSeq& msgs, const std::string& app);
};
typedef XPtr<WebUser> WebUserPtr;


#endif
