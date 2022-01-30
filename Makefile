
# Either linux.amd64 or win.amd64
OS:=linux
ARCH:=amd64

# Directories
VBOXSRC:=VirtualBox.src
VBOXBIN:=VirtualBox.$(OS).$(ARCH)
OBJDIR:=obj
OBJOSDIR:=$(OBJDIR)/$(OS).$(ARCH)
OUTDIR:=out
OUTOSDIR:=$(OUTDIR)/$(OS).$(ARCH)

# Files for each library
ADLIBR3OBJ:=$(OBJOSDIR)/Adlib.o $(OBJOSDIR)/opl3.o 
MPU401R3OBJ:=$(OBJOSDIR)/Mpu401.o
ADLIBR3LIBS:=
MPU401R3LIBS:=
ifeq "$(OS)" "linux"
ADLIBR3OBJ+=$(OBJOSDIR)/pcmalsa.o
MPU401R3OBJ+=$(OBJOSDIR)/midialsa.o
ADLIBR3LIBS+=-lasound
MPU401R3LIBS+=-lasound
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
VBOX_DEFINES:=-DVBOX_HAVE_VISIBILITY_HIDDEN -DRT_USE_VISIBILITY_DEFAULT -DVBOX -DVBOX_OSE -DVBOX_WITH_64_BITS_GUESTS -DVBOX_WITH_DEBUGGER -DIN_RING3 -DGC_ARCH_BITS=64 -DVBOX_WITH_DTRACE -DVBOX_WITH_DTRACE_R3 -DPIC -DVBOX_IN_EXTPACK -DVBOX_IN_EXTPACK_R3 -DHC_ARCH_BITS=64
VBOX_CFLAGS:=-pedantic -Wshadow -Wall -Wextra -Wno-missing-field-initializers -Wno-unused -Wno-trigraphs -fdiagnostics-show-option -Wno-unused-parameter  -Wlogical-op  -Wno-variadic-macros -Wno-long-long -Wunused-variable -Wunused-function -Wunused-label -Wunused-parameter  -Wno-array-bounds -Wno-ignored-qualifiers -Wno-variadic-macros -fno-omit-frame-pointer -fno-strict-aliasing -fvisibility=hidden  -fno-exceptions -I$(VBOXSRC)/include -Iinclude
VBOX_CXXFLAGS:=$(VBOX_CFLAGS) -Wno-overloaded-virtual -fvisibility-inlines-hidden -fno-rtti
VBOX_LDFLAGS:=
VBOX_LIBS:=$(VBOXBIN)/VBoxRT.$(SO) $(VBOXBIN)/VBoxVMM.$(SO)

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

build: $(OUTOSDIR)/VMusicMain.$(SO) $(OUTOSDIR)/VMusicMainVM.$(SO) $(OUTOSDIR)/AdlibR3.$(SO) $(OUTOSDIR)/Mpu401R3.$(SO)

$(OUTDIR) $(OBJDIR) $(OBJOSDIR) $(OUTOSDIR): %:
	mkdir -p $@

$(OBJOSDIR)/%.o: %.cpp | $(OBJOSDIR)
	$(CXX) -c -O2 -g -pipe -fPIC -m64 $(VBOX_CXXFLAGS) $(VBOX_DEFINES) -o $@ $<
	
$(OBJOSDIR)/%.o: %.c | $(OBJOSDIR)
	$(CC) -c -O2 -g -pipe -fPIC -m64 $(VBOX_CFLAGS) $(VBOX_DEFINES) -o $@ $<

$(OUTOSDIR)/VMusicMain.$(SO): $(OBJOSDIR)/VMusicMain.o | $(OUTOSDIR)
	$(CXX) -shared -fPIC -m64 $(VBOX_LDFLAGS) -o $@ $+ $(VBOX_LIBS)

$(OUTOSDIR)/VMusicMainVM.$(SO): $(OBJOSDIR)/VMusicMainVM.o | $(OUTOSDIR)
	$(CXX) -shared -fPIC -m64 $(VBOX_LDFLAGS) -o $@ $+ $(VBOX_LIBS)
	
$(OUTOSDIR)/AdlibR3.$(SO): $(ADLIBR3OBJ) | $(OUTOSDIR)
	$(CXX) -shared -fPIC -m64 $(VBOX_LDFLAGS) -o $@ $+ $(VBOX_LIBS) $(ADLIBR3LIBS)

$(OUTOSDIR)/Mpu401R3.$(SO): $(MPU401R3OBJ) | $(OUTOSDIR)
	$(CXX) -shared -fPIC -m64 $(VBOX_LDFLAGS) -o $@ $+ $(VBOX_LIBS) $(MPU401R3LIBS)

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
