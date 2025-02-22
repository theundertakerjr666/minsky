DEBUG=1

# root directory for ecolab include files and libraries
ifeq ($(shell ls $(HOME)/usr/ecolab/include/ecolab.h),$(HOME)/usr/ecolab/include/ecolab.h)
ECOLAB_HOME=$(HOME)/usr/ecolab
else
ECOLAB_HOME=/usr/local/ecolab
endif

include $(ECOLAB_HOME)/include/Makefile
VPATH= .. ../schema ../model ../engine ../server $(ECOLAB_HOME)/include

UNITTESTOBJS=main.o testModel.o testMinsky.o testLatexToPango.o testVariable.o testDerivative.o testDatabase.o testUnits.o testXVector.o testTensorOps.o testLockGroup.o testCSVParser.o

MINSKYOBJS=$(filter-out ../tclmain.o ../server-main.o,$(wildcard ../*.o))
FLAGS:=-I.. $(FLAGS)
FLAGS+=-std=c++11  -Wno-unused-local-typedefs -I../model -I../engine -I../schema
LIBS+=-ljson_spirit -lsoci_core -lboost_system -lboost_thread \
	-lboost_regex -lboost_date_time -lboost_filesystem \
	-lUnitTest++ -lgsl -lgslcblas  -lxml2 -ltiff

# RSVG dependencies calculated here
FLAGS+=$(shell pkg-config --cflags librsvg-2.0)
LIBS+=$(shell pkg-config --libs librsvg-2.0)

EXES=cmpFp checkSchemasAreSame
#testDatabase testGroup 

ifdef AEGIS
#EXES+=tcl-cov
endif

all: unittests $(EXES)

unittests: $(UNITTESTOBJS) $(MINSKYOBJS)
	$(CPLUSPLUS) $(FLAGS)  -o $@ $(UNITTESTOBJS) $(MINSKYOBJS) $(LIBS)

# todo - remove dependency on all MINSKYOBJS
testDatabase: main.o testDatabase.o $(MINSKYOBJS)
	$(CPLUSPLUS) $(FLAGS)  -o $@ $^ $(LIBS)

testLatexToPango: main.o testLatexToPango.o $(MINSKYOBJS)
	$(CPLUSPLUS) $(FLAGS)  -o $@ $^ $(LIBS)

testGroup: main.o testGroup.o $(MINSKYOBJS)
	$(CPLUSPLUS) $(FLAGS)  -o $@ $^ $(LIBS)

cmpFp: cmpFp.o
	$(CPLUSPLUS) $(FLAGS) -o $@ $<

checkSchemasAreSame: checkSchemasAreSame.o $(MINSKYOBJS)
	$(CPLUSPLUS) $(FLAGS) -o $@ $^ $(LIBS)

tcl-cov: tcl-cov.o $(MINSKYOBJS)
	$(CPLUSPLUS) $(FLAGS) -o $@ $^ $(LIBS)

include $(UNITTESTOBJS:.o=.d) $(EXES:=.d)

clean:
	$(BASIC_CLEAN) unittests $(EXES)  *.gcda *.gcno
	cd 00; $(BASIC_CLEAN)
	cd exampleLogs; $(BASIC_CLEAN)
	cd oldSchema; $(BASIC_CLEAN)
