include $(HCLIB_ROOT)/include/hclib.mak

LIKWID_INCLUDE ?= -I/usr/local/include
LIKWID_LIBDIR ?= -L/usr/local/lib
LIKWID_LIB ?= -llikwid

TARGETS := fib fib-reducer qsort

all: clean $(TARGETS) clean-obj

%: %.cpp
	$(CXX) -O3 $(PROJECT_CXXFLAGS) $(PROJECT_LDFLAGS) $(LIKWID_INCLUDE) $(LIKWID_LIBDIR) -o $@ $^  $(PROJECT_LDLIBS) $(LIKWID_LIB)

clean-obj:
	rm -rf *.o

clean:
	rm -rf *.o $(TARGETS)
