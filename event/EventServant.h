#ifndef EventServant_h_
#define EventServant_h_

#include "xic/ServantI.h"
#include <st.h>
#include <deque>
#include <vector>

#define EVENTSERVANT_CMDS	\
	CMD(push)		\
	CMD(pull)		\
	/* END OF LIST */

class EventData;
typedef XPtr<EventData> EventDataPtr;
typedef std::deque<EventDataPtr> EventDeque;
typedef std::vector<EventDataPtr> EventSeq;

class EventData: public XRefCount
{
	int64_t _seq;
	uint16_t _total;
	uint16_t _type_pos;
	uint16_t _oid_pos;
	uint8_t _type_len;
	uint8_t _oid_len;

        virtual void xref_destroy();
public:
        static EventDataPtr create(const xstr_t& type, const xstr_t& oid, const vbs_dict_t* content);

	void setSeq(int64_t seq)	{ _seq = seq; }
	int64_t seq() const		{ return _seq; }
	xstr_t type() const;
	xstr_t oid() const;
	xstr_t data() const;
};


class EventServant: public xic::ServantI
{
	static MethodTab::PairType _methodpairs[];
	static MethodTab _methodtab;

	xic::ProxyPtr _selfPrx;
	EventDeque _events;
	int64_t _lastSeq;

	st_cond_t _cond;
public:
	EventServant(const xic::AdapterPtr& adapter, const SettingPtr& setting);
	virtual ~EventServant();

private:
	int64_t _gen_seq();
	int64_t _wait_events(int wait, int64_t seq, int maxn, const xstr_t& filter, EventSeq& results);

#define CMD(X)    XIC_METHOD_DECLARE(X);
	EVENTSERVANT_CMDS
#undef CMD
};

typedef XPtr<EventServant> EventServantPtr;

#endif
