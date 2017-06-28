#include "EventServant.h"
#include "dlog/dlog.h"
#include "xslib/xbuf.h"
#include <sstream>
#include <errno.h>
#include <sys/time.h>


#define WAIT_DEFAULT	5
#define WAIT_MAX 	55
#define QUEUE_SIZE 	(1024*1024)


EventServant::MethodTab::PairType EventServant::_methodpairs[] = {
#define CMD(X)    { #X, XIC_METHOD_CAST(EventServant, X) },
	EVENTSERVANT_CMDS
#undef CMD
};

EventServant::MethodTab EventServant::_methodtab(_methodpairs, XS_ARRCOUNT(_methodpairs));

EventDataPtr EventData::create(const xstr_t& type, const xstr_t& oid, const vbs_dict_t* content)
{
	size_t len = vbs_size_of_string(type.len) + vbs_size_of_string(oid.len);
	if (content)
		len += vbs_size_of_dict(content);

	uint8_t *p = (uint8_t *)malloc(sizeof(EventData) + len);
	if (!p)
		throw XERROR(XMemoryError);

	EventData *ed = new(p) EventData();
	p += sizeof(EventData);

	ed->_seq = 0;
	ed->_total = len;
	ed->_type_len = type.len;
	ed->_oid_len = oid.len;

	xbuf_t xb = XBUF_INIT(p, len);
	vbs_packer_t pk = VBS_PACKER_INIT(xbuf_xio.write, &xb, -1);

	vbs_pack_head_of_string(&pk, type.len);
	ed->_type_pos = xb.len;
	vbs_pack_raw(&pk, type.data, type.len);

	vbs_pack_head_of_string(&pk, oid.len);
	ed->_oid_pos = xb.len;
	vbs_pack_raw(&pk, oid.data, oid.len);

	if (content)
		vbs_pack_dict(&pk, content);

	return ed;
}

void EventData::xref_destroy()
{
        this->~EventData();
        free(this);
}

xstr_t EventData::type() const
{
	uint8_t *p = (uint8_t *)(void *)this;
	p += sizeof(EventData);
	xstr_t xs = XSTR_INIT(p + _type_pos, _type_len);
	return xs;
}

xstr_t EventData::oid() const
{
	uint8_t *p = (uint8_t *)(void *)this;
	p += sizeof(EventData);
	xstr_t xs = XSTR_INIT(p + _oid_pos, _oid_len);
	return xs;
}

xstr_t EventData::data() const
{
	uint8_t *p = (uint8_t *)(void *)this;
	p += sizeof(EventData);
	xstr_t xs = XSTR_INIT(p, _total);
	return xs;
}

EventServant::EventServant(const xic::AdapterPtr& adapter, const SettingPtr& setting)
	: ServantI(&_methodtab), _lastSeq(0)
{
	_cond = st_cond_new();
	xref_inc();
	_selfPrx = adapter->addServant("wp_event", this);
	xref_dec_only();
}

EventServant::~EventServant()
{
	st_cond_destroy(_cond);
}

static bool is_valid_type(const xstr_t& type)
{
	static const bset_t _valid_bset = make_bset_by_add_cstr(&alnum_bset, "_.");
	if (type.len > 255 || type.len == 0)
		return false;
	if (type.data[0] == '.' || type.data[type.len-1] == '.')
		return false;
	return (xstr_find_not_in_bset(&type, 0, &_valid_bset) < 0);
}

static bool is_valid_oid(const xstr_t& oid)
{
	static const bset_t _valid_bset = make_bset_by_add_cstr(&alnum_bset, "_");
	if (oid.len > 255 || oid.len == 0)
		return false;
	return (xstr_find_not_in_bset(&oid, 0, &_valid_bset) < 0);
}

int64_t EventServant::_gen_seq()
{
	int64_t t = time(NULL) << 20;
	int64_t seq = ++_lastSeq;
	if (seq < t)
	{
		seq = t + (seq & 0xFFFFF);
		_lastSeq = seq;
	}
	return seq;
}

XIC_METHOD(EventServant, push)
{
	xic::VDict args = quest->args();
	const xstr_t& type = args.wantXstr("type");
	const xstr_t& oid = args.wantXstr("oid");
	const vbs_dict_t* content = args.get_dict("content");

	if (!is_valid_type(type))
		throw XERROR_MSG(XError, "type empty, too long or has invalid character");

	if (!is_valid_oid(oid))
		throw XERROR_MSG(XError, "oid empty, too long or has invalid character");

	EventDataPtr ed = EventData::create(type, oid, content);

	ed->setSeq(_gen_seq());
	_events.push_back(ed);
	while (_events.size() > QUEUE_SIZE)
		_events.pop_front();
	st_cond_broadcast(_cond);
	return xic::AnswerWriter();
}

XIC_METHOD(EventServant, pull)
{
	xic::VDict args = quest->args();
	int64_t seq = args.wantInt("seq");
	ssize_t maxn = args.wantInt("maxn");
	const xstr_t& prefix = args.getXstr("prefix");
	int wait = args.getInt("wait", WAIT_DEFAULT);
	wait = XS_CLAMP(wait, 0, WAIT_MAX);

	if (seq <= 0)
		seq = _lastSeq;

	EventSeq results;
	int64_t nextseq = _wait_events(wait, seq, maxn, prefix, results);

	xic::AnswerWriter aw;
	aw.param("seq", nextseq);
	xic::VListWriter lw = aw.paramVList("events");
	for (size_t i = 0; i < results.size(); ++i)
	{
		xstr_t xs = results[i]->data();
		lw.vblob(xs);
	}

	return aw;
}

int64_t EventServant::_wait_events(int wait, int64_t seq, int maxn, const xstr_t& prefix, EventSeq& results)
{
	if (seq > _lastSeq)
		return _lastSeq;

	st_utime_t left_time = wait * 1000 * 1000;
	st_utime_t start_time = st_utime();
	if (seq == _lastSeq && left_time > 0)
	{
again:
		st_cond_timedwait(_cond, left_time);
	}

	if (seq < _lastSeq && _events.size() && seq >= _events[0]->seq())
	{
		int64_t delta = (_lastSeq - seq) & 0xFFFFF;
		if (delta > (ssize_t)_events.size())
			delta = _events.size();

		int64_t start = _events.size() - delta;
		if (maxn > delta)
			maxn = delta;

		int k = 0;
		results.reserve(maxn);
		for (int i = 0; i < delta; ++i)
		{
			const EventDataPtr& event = _events[start + i];
			if (prefix.len)
			{
				xstr_t type = event->type();
				if (!xstr_start_with(&type, &prefix))
					continue;

				if (type.len > prefix.len && type.data[prefix.len] != '.')
					continue;
			}

			results.push_back(event);

			if (++k >= maxn)
			{
				return (i < delta) ? _events[start + i]->seq() : _lastSeq;
			}
		}

		if (k == 0 && left_time > 0)
		{
			st_utime_t used_time = st_utime() - start_time;
			if (left_time > used_time)
			{
				seq = _lastSeq;
				left_time -= used_time;
				goto again;
			}
		}
	}
	return _lastSeq;
}

