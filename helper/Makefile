##
##

OBJS = ProxyChooser.o ProxyIdSeqMap.o


LIBUTILS = libhelper.a

CFLAGS = -g -Wall -rdynamic

CXXFLAGS = -g -Wall -rdynamic

CPPFLAGS = -I. -I../../knotty -I../../knotty/include

LIBS = -pthread -L../../knotty/lib -lxic -ldlog -lxs -lst -lrt


all: $(OBJS) $(LIBUTILS) $(EXES)

$(LIBUTILS): $(OBJS)
	$(AR) -rcs $@ $(OBJS)

.c.o:
	$(CC) -c $(CFLAGS) $(CPPFLAGS) -o $@ $<

.cpp.o:
	$(CXX) -c $(CXXFLAGS) $(CPPFLAGS) -o $@ $<

.c:
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LIBS)

.cpp:
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ $^ $(LIBS)

clean:
	$(RM) $(OBJS) $(LIBUTILS) $(EXES)

