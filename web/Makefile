##
##

OBJS = info.o verify.o WebUser.o UListener.o HttpReq.o HttpConnection.o WebServant.o 


EXES = wp_webd

CFLAGS = -g -Wall -rdynamic

CXXFLAGS = -g -Wall -rdynamic

CPPFLAGS = -I. -I.. -I../../knotty -I../../knotty/include

LIBS = -pthread -Wl,-static -L../helper -lhelper -L../../knotty/lib -lxic -ldlog -lxs -lst -Wl,-call_shared -lrt


all: $(OBJS) $(EXES) 

$(EXES): $(OBJS)


.c.o:
	$(CC) -c $(CFLAGS) $(CPPFLAGS) -o $@ $<

.cpp.o:
	$(CXX) -c $(CXXFLAGS) $(CPPFLAGS) -o $@ $<

.c:
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LIBS)

.cpp:
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ $^ $(LIBS)

clean:
	$(RM) $(OBJS) $(EXES)

