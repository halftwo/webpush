#include "WebUser.h"
#include "info.h"

#define MSG_QSIZE        512

WebUser::WebUser()
	: _keep_time(0), _seq(1)
{
	xatomic_set(&_waiter, 0);
	_cond = st_cond_new();
}

WebUser::~WebUser()
{
	st_cond_destroy(_cond);
}

int WebUser::get_msgs(StringSeq& msgs)
{
	msgs.reserve(_msgs.size());
	for (std::deque<std::string>::iterator iter = _msgs.begin(); iter != _msgs.end(); ++iter)
	{
		msgs.push_back(*iter);
	}
	return _seq;
}

static bool check_info_category(const std::string& category, const std::string& info)
{
	vbs_unpacker_t uk = VBS_UNPACKER_INIT((uint8_t*)info.data(), info.length(), -1);
	if (vbs_unpack_head_of_dict(&uk, NULL) != 0)
		return false;

	xstr_t key;
	if (vbs_unpack_xstr(&uk, &key) == 0 && xstr_equal_cstr(&key, "c"))
	{
		xstr_t value;
		if (vbs_unpack_xstr(&uk, &value) == 0 && xstr_equal_mem(&value, category.data(), category.length()))
			return true;
	}

	return false;
}

int WebUser::wait_for_new_msgs(int seq, int wait, StringSeq& msgs, const std::string& category)
{
	xatomic_inc(&_waiter);

	time_t start = st_time();
	time_t end = start + wait;
	while (start <= end)
	{
		int size = _msgs.size();
		if (seq < _seq && seq >= _seq - size)
		{
			int start = size - (_seq - seq);
			for (int i = start; i < size; ++i)
			{
				++seq;
				const std::string& msg = _msgs[i];
				if (!category.empty() && check_info_category(category, msg))
					msgs.push_back(msg);
			}
		}

		if (msgs.size() > 0)
		{
			time_t elapse = st_time() - start;
			if (elapse < 2)
				st_sleep(2 - elapse);
			break;
		}

		wait = end - st_time();
		if (wait > 0)
		{
			if (st_cond_timedwait(_cond, (st_utime_t)1000000 * wait) != 0)
				break;
		}
	}

	xatomic_dec(&_waiter);
	return _seq;
}

int WebUser::inform(const std::string& msg)
{
	if (_msgs.size() > MSG_QSIZE)
	{
		int reap = _msgs.size() - MSG_QSIZE;
		while (reap-- > 0)
			_msgs.pop_front();
	}

	int64_t seq = _seq++;
	_msgs.push_back(msg);

	st_cond_broadcast(_cond);
	return seq;    
}

