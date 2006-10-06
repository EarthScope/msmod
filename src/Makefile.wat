#
# Wmake file - For Watcom's wmake
# Use 'wmake -f Makefile.wat'

.BEFORE
	@set INCLUDE=.;$(%watcom)\H;$(%watcom)\H\NT
	@set LIB=.;$(%watcom)\LIB386

cc     = wcc386
cflags = -zq
lflags = OPT quiet OPT map LIBRARY ..\libmseed\libmseed.lib
cvars  = $+$(cvars)$- -DWIN32

BIN = ..\msmod.exe

INCS = -I..\libmseed

all: $(BIN)

$(BIN):	msmod.obj dsarchive.obj
	wlink $(lflags) name $(BIN) file {msmod.obj dsarchive.obj}

# Source dependencies:
msmod.obj:	msmod.c dsarchive.h
dsarchive.obj:	dsarchive.c dsarchive.h

# How to compile sources:
.c.obj:
	$(cc) $(cflags) $(cvars) $(INCS) $[@ -fo=$@

# Clean-up directives:
clean:	.SYMBOLIC
	del *.obj *.map $(BIN)
