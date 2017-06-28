##
##

OBJS = HubServant.o

EXES = wp_hubd

CFLAGS = -g -Wall -rdynamic -Werror

CXXFLAGS = -g -Wall -rdynamic -Werror

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

