# Settings
# Either linux.amd64 or win.amd64
OS:=linux
ARCH:=amd64

CFLAGS?=-O2 -g -pipe
CXXFLAGS?=-O2 -g -pipe

# 1 enables verbose logging
#LOG:=1

# Settings end here

# Directories
VBOXSRC:=VirtualBox.src
VBOXBIN:=VirtualBox.$(OS).$(ARCH)
OBJDIR:=obj
OBJOSDIR:=$(OBJDIR)/$(OS).$(ARCH)
OUTDIR:=out
OUTOSDIR:=$(OUTDIR)/$(OS).$(ARCH)

# Files for each library
ADLIBR3OBJ:=$(OBJOSDIR)/Adlib.o $(OBJOSDIR)/opl3.o
ADLIBR3LIBS:=
MPU401R3OBJ:=$(OBJOSDIR)/Mpu401.o
MPU401R3LIBS:=
EMU8000R3OBJ:=$(OBJOSDIR)/Emu8000.o $(OBJOSDIR)/emu8k.o
EMU8000R3LIBS:=

ifeq "$(OS)" "linux"
ADLIBR3OBJ+=$(OBJOSDIR)/pcmalsa.o
ADLIBR3LIBS+=-lasound
MPU401R3OBJ+=$(OBJOSDIR)/midialsa.o
MPU401R3LIBS+=-lasound
EMU8000R3OBJ+=$(OBJOSDIR)/pcmalsa.o
EMU8000R3LIBS+=-lasound
else ifeq "$(OS)" "win"
ADLIBR3OBJ+=$(OBJOSDIR)/pcmwin.o
MPU401R3OBJ+=$(OBJOSDIR)/midiwin.o
endif

# Compiler selection
ifeq "$(OS)" "win"
SO:=dll
ifeq "$(ARCH)" "amd64"
CC:=x86_64-w64-mingw32-gcc
CXX:=x86_64-w64-mingw32-g++
endif # "$(ARCH)" "amd64"
else
SO:=so
CC:=gcc
CXX=g++
endif

# Compiler flags
ifeq "$(LOG)" "1"
VMUSIC_DEFINES:=-DLOG_ENABLED=1 -DLOG_ENABLE_FLOW=1
endif

VBOX_DEFINES:=-DVBOX_HAVE_VISIBILITY_HIDDEN -DRT_USE_VISIBILITY_DEFAULT -DVBOX -DVBOX_OSE -DVBOX_WITH_64_BITS_GUESTS -DVBOX_WITH_DEBUGGER \
    -DIN_RING3 -DGC_ARCH_BITS=64 -DVBOX_WITH_DTRACE -DVBOX_WITH_DTRACE_R3 -DPIC -DVBOX_IN_EXTPACK -DVBOX_IN_EXTPACK_R3 -DHC_ARCH_BITS=64
VBOX_CFLAGS:=-fPIC -m64 -pedantic -Wshadow -Wall -Wextra \
    -Wno-missing-field-initializers -Wno-unused -Wno-trigraphs -fdiagnostics-show-option -Wno-unused-parameter -Wlogical-op  -Wno-variadic-macros -Wno-long-long -Wunused-variable -Wunused-function -Wunused-label -Wunused-parameter  -Wno-array-bounds -Wno-ignored-qualifiers -Wno-variadic-macros -fno-omit-frame-pointer -fno-strict-aliasing \
    -fvisibility=hidden -fno-exceptions \
    -I$(VBOXSRC)/include -Iinclude
VBOX_CXXFLAGS:=$(VBOX_CFLAGS) -Wno-overloaded-virtual -fvisibility-inlines-hidden -fno-rtti
VBOX_LDFLAGS:=-fPIC -m64
VBOX_LIBS:=$(VBOXBIN)/VBoxRT.$(SO)

ifeq "$(OS)" "win"
VBOX_DEFINES+=-DRT_OS_WINDOWS -D__WIN__ -D__WIN64__ 
VBOX_LDFLAGS+=-Wl,--as-needed
else ifeq "$(OS)" "linux"
VBOX_DEFINES+=-DRT_OS_LINUX -D_FILE_OFFSET_BITS=64
VBOX_LDFLAGS+=-Wl,-z,noexecstack,-z,relro -Wl,--as-needed -Wl,-z,origin
endif

ifeq "$(ARCH)" "amd64"
VBOX_DEFINES+=-DRT_ARCH_AMD64 -D__AMD64__
endif

all: build

build: $(OUTOSDIR)/VMusicMain.$(SO) $(OUTOSDIR)/VMusicMainVM.$(SO) $(OUTOSDIR)/AdlibR3.$(SO) $(OUTOSDIR)/Mpu401R3.$(SO) $(OUTOSDIR)/Emu8000R3.$(SO)

$(OUTDIR) $(OBJDIR) $(OBJOSDIR) $(OUTOSDIR): %:
	mkdir -p $@

$(OBJOSDIR)/%.o: %.cpp | $(OBJOSDIR)
	$(CXX) -c $(VBOX_CXXFLAGS) $(VBOX_DEFINES) $(VMUSIC_DEFINES) $(CXXFLAGS) -o $@ $<

$(OBJOSDIR)/%.o: %.c | $(OBJOSDIR)
	$(CC) -c $(VBOX_CFLAGS) $(VBOX_DEFINES) $(VMUSIC_DEFINES) $(CFLAGS) -o $@ $<

$(OUTOSDIR)/VMusicMain.$(SO): $(OBJOSDIR)/VMusicMain.o | $(OUTOSDIR)
	$(CXX) -shared $(VBOX_LDFLAGS) -o $@ $+ $(VBOX_LIBS)

$(OUTOSDIR)/VMusicMainVM.$(SO): $(OBJOSDIR)/VMusicMainVM.o | $(OUTOSDIR)
	$(CXX) -shared $(VBOX_LDFLAGS) -o $@ $+ $(VBOX_LIBS)
	
$(OUTOSDIR)/AdlibR3.$(SO): $(ADLIBR3OBJ) | $(OUTOSDIR)
	$(CXX) -shared $(VBOX_LDFLAGS) -o $@ $+ $(VBOX_LIBS) $(ADLIBR3LIBS)

$(OUTOSDIR)/Mpu401R3.$(SO): $(MPU401R3OBJ) | $(OUTOSDIR)
	$(CXX) -shared $(VBOX_LDFLAGS) -o $@ $+ $(VBOX_LIBS) $(MPU401R3LIBS)

$(OUTOSDIR)/Emu8000R3.$(SO): $(EMU8000R3OBJ) | $(OUTOSDIR)
	$(CXX) -shared $(VBOX_LDFLAGS) -o $@ $+ $(VBOX_LIBS) $(EMU8000R3LIBS)

$(OUTDIR)/ExtPack.xml: ExtPack.xml
	install -m 0644 $< $@

$(OUTDIR)/ExtPack.signature:
	echo "todo" > $@

$(OUTDIR)/ExtPack.manifest: $(OUTDIR) $(OUTOSDIR)
	cd $(OUTDIR) ;\
	find -type f -printf '%P\n' | xargs ../build_manifest.sh > $(@F)

pack: $(OUTDIR)/ExtPack.xml $(OUTDIR)/ExtPack.signature $(OUTDIR)/ExtPack.manifest
	tar --format=ustar --numeric-owner --owner=0 --group=0 --mode='a=rX,u+w' --sort=name -C $(OUTDIR) -f VMusic.vbox-extpack -v -z -c .

strip:
	strip $(OUTOSDIR)/*.$(SO)

clean:
	rm -rf $(OUTDIR) $(OBJDIR) VMusic.vbox-extpack

.PHONY: all build clean strip pack
