#ifndef UListener_h_
#define UListener_h_

#include "xslib/XRefCount.h"
#include <st.h>
#include <string>


class UListener;
typedef XPtr<UListener> UListnerPtr;


class UListener: virtual public XRefCount
{
protected:
	unsigned short _port;
        st_netfd_t _sf;
	std::string _netloc;

public:
	UListener(unsigned short port);
	virtual ~UListener();

	void start();

	const std::string& netloc() const 		{ return _netloc; }

	// Should be override by derived class
	virtual void accepted(st_netfd_t sf);

private:
	void listen_fiber();
};


#endif
