# 
#	DataChan and monitor - for Josh.ai
#	Programmer: Danny Holstein
#
LOG?=Install.log	#	Use Linux "logger" command to send logging info to this file
ifeq (null,$(shell if ls -l /dev/log  2>/dev/null > /dev/null ; then echo system; else echo null; fi))
	LOGGER?={ read LOGMSG && echo "[$@: `date`]" $$LOGMSG | tee -a $(LOG); }
else
	LOGGER?=logger --tag "[$@: `date`]" -s 2>&1 | tee -a $(LOG)
endif
CXX=g++
CXXFLAGS=$(DEBUG) -std=c++17 -fpermissive -Wno-write-strings -fPIC

INCLUDES:=$(INCLUDES) -I/usr/include
INCLUDES:=$(INCLUDES) -I./
LDFLAGS=$(DEBUG)
LDLIBS:=-lpthread
# ifeq ($(STATIC),)
# else
# endif

ifeq ($(DEBUG),)
	BINDIR:=release
else
	BINDIR:=debug
endif

DATACHAN_LIB := DataChanLib.so

all: libDataChan.a $(DATACHAN_LIB)

$(DATACHAN_LIB): DataChanLib.o DataChan.o Error.o
	@if $(CXX) -shared -o $@ $^ -L../cpp-base64  $(LDLIBS);\
		then echo "--- Build \"$@\": Success ---" | $(LOGGER) ;\
		else echo "--- Build \"$@\": FAILURE! ---" | $(LOGGER) ; exit 1; fi

DataChanLib.o: DataChanLib.cpp DataChan.h Error.h
	@if $(CXX) $(CPPFLAGS) $(CXXFLAGS) $(INCLUDES) -c $< -o $@;\
		then echo "--- Build \"$@\": Success ---" | $(LOGGER) ;\
		else echo "--- Build \"$@\": FAILURE! ---" | $(LOGGER) ; exit 1; fi

monitor: monitor.o libDataChan.a
	@if $(CXX) $(CXXFLAGS) $(INCLUDES) -o $@  $^ $(LDFLAGS) -L../cpp-base64 $(LDLIBS);\
		then echo "--- Build \"monitor\": Success ---" | $(LOGGER) ;\
		else echo "--- Build \"monitor\": FAILURE! ---" | $(LOGGER) ; exit 1; fi

%.o:	%.cpp %.h
	@if $(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@;\
		then echo "--- Compile $< $*.h: Success ---" | $(LOGGER) ;\
		else echo "--- Compile $< $*.h: FAILURE! ---" | $(LOGGER) ; exit 1; fi

libDataChan.a:	DataChan.o  Error.o
	@if ar rcs $@ $^ && ranlib $@;\
		then echo "--- Archive $@: Success ---" | $(LOGGER) ;\
		else echo "--- Archive $@: FAILURE! ---" | $(LOGGER) ; exit 1; fi
	@if ln -sf $@ DataChan.a;\
		then echo "--- Link $@: Success ---" | $(LOGGER) ;\
		else echo "--- Link $@: FAILURE! ---" | $(LOGGER) ; exit 1; fi

clean:
	@if rm -fv *.a *.o *.so monitor;\
		then echo "--- $@: Success ---" | $(LOGGER) ;\
		else echo "--- $@: FAILURE! ---" | $(LOGGER) ; exit 1; fi

archive:
	@if tar -czvf DataChanArchive.tar.gz *.cpp *.h Makefile monitor README.md;\
		then echo "--- Archive $@: Success ---" | $(LOGGER) ;\
		else echo "--- Archive $@: FAILURE! ---" | $(LOGGER) ; exit 1; fi

