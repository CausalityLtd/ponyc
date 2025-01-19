config ?= release
arch ?= native
tune ?= generic
build_flags ?= -j2
llvm_archs ?= X86;ARM;AArch64;WebAssembly;RISCV
llvm_config ?= Release
llc_arch ?= x86-64
pic_flag ?=

ifndef version
  version := $(shell cat VERSION)
  ifneq ($(wildcard .git),)
    sha := $(shell git rev-parse --short HEAD)
    tag := $(version)-$(sha)
  else
    tag := $(version)
  endif
else
  tag := $(version)
endif

symlink := yes
ifdef DESTDIR
  prefix := $(DESTDIR)
  ponydir := $(prefix)
  symlink := no
else
  prefix ?= /usr/local
  ponydir ?= $(prefix)/lib/pony/$(tag)
endif

# Use clang by default; because CC defaults to 'cc'
# you must explicitly set CC=gcc to use gcc
ifndef CC
  ifneq (,$(shell clang --version 2>&1 | grep 'clang version'))
    CC = clang
    CXX = clang++
  endif
else ifeq ($(CC), cc)
  ifneq (,$(shell clang --version 2>&1 | grep 'clang version'))
    CC = clang
    CXX = clang++
  endif
endif

# By default, CC is cc and CXX is g++
# So if you use standard alternatives on many Linuxes
# You can get clang and g++ and then bad things will happen
ifneq (,$(shell $(CC) --version 2>&1 | grep 'clang version'))
  ifneq (,$(shell $(CXX) --version 2>&1 | grep "Free Software Foundation"))
    CXX = c++
  endif

  ifneq (,$(shell $(CXX) --version 2>&1 | grep "Free Software Foundation"))
    $(error CC is clang but CXX is g++. They must be from matching compilers.)
  endif
else ifneq (,$(shell $(CC) --version 2>&1 | grep "Free Software Foundation"))
  ifneq (,$(shell $(CXX) --version 2>&1 | grep 'clang version'))
    CXX = c++
  endif

  ifneq (,$(shell $(CXX) --version 2>&1 | grep 'clang version'))
    $(error CC is gcc but CXX is clang++. They must be from matching compilers.)
  endif
endif

# Make sure the compiler gets all relevant search paths on FreeBSD
ifeq ($(shell uname -s),FreeBSD)
  ifeq (,$(findstring /usr/local/include,$(shell echo $CPATH)))
    export CPATH = /usr/local/include:$CPATH
  endif
  ifeq (,$(findstring /usr/local/lib,$(shell echo $LIBRARY_PATH)))
    export LIBRARY_PATH = /usr/local/lib:$LIBRARY_PATH
  endif
endif

srcDir := $(shell dirname '$(subst /Volumes/Macintosh HD/,/,$(realpath $(lastword $(MAKEFILE_LIST))))')
buildDir := $(srcDir)/build/build_$(config)

# get outDir from CMake install file or fall back to default if the file doesn't exist
outDir := $(subst /libponyrt.tests,,$(shell grep -o -s '$(srcDir)\/build\/$(config).*\/libponyrt.tests' $(buildDir)/cmake_install.cmake))
ifeq ($(outDir),)
  outDir := $(srcDir)/build/$(config)
endif

libsSrcDir := $(srcDir)/lib
libsBuildDir := $(srcDir)/build/build_libs
libsOutDir := $(srcDir)/build/libs

ifndef verbose
  SILENT = @
  CMAKE_VERBOSE_FLAGS :=
else
  SILENT =
  CMAKE_VERBOSE_FLAGS := -DCMAKE_VERBOSE_MAKEFILE=ON
endif

CMAKE_FLAGS := $(CMAKE_FLAGS) $(CMAKE_VERBOSE_FLAGS)

ifeq ($(lto),yes)
  LTO_CONFIG_FLAGS = -DPONY_USE_LTO=true
else
  LTO_CONFIG_FLAGS =
endif

ifeq ($(runtime-bitcode),yes)
  ifeq (,$(shell $(CC) -v 2>&1 | grep clang))
    $(error Compiling the runtime as a bitcode file requires clang)
  endif
  BITCODE_FLAGS = -DPONY_RUNTIME_BITCODE=true
else
  BITCODE_FLAGS =
endif

PONY_USES =

comma:= ,
empty:=
space:= $(empty) $(empty)

define USE_CHECK
  $$(info Enabling use option: $1)
  ifeq ($1,valgrind)
    PONY_USES += -DPONY_USE_VALGRIND=true
  else ifeq ($1,thread_sanitizer)
    PONY_USES += -DPONY_USE_THREAD_SANITIZER=true
  else ifeq ($1,address_sanitizer)
    PONY_USES += -DPONY_USE_ADDRESS_SANITIZER=true
  else ifeq ($1,undefined_behavior_sanitizer)
    PONY_USES += -DPONY_USE_UNDEFINED_BEHAVIOR_SANITIZER=true
  else ifeq ($1,coverage)
    PONY_USES += -DPONY_USE_COVERAGE=true
  else ifeq ($1,pooltrack)
    PONY_USES += -DPONY_USE_POOLTRACK=true
  else ifeq ($1,dtrace)
    DTRACE ?= $(shell which dtrace)
    ifeq (, $$(DTRACE))
      $$(error No dtrace compatible user application static probe generation tool found)
    endif
    PONY_USES += -DPONY_USE_DTRACE=true
  else ifeq ($1,scheduler_scaling_pthreads)
    PONY_USES += -DPONY_USE_SCHEDULER_SCALING_PTHREADS=true
  else ifeq ($1,systematic_testing)
    PONY_USES += -DPONY_USE_SYSTEMATIC_TESTING=true
  else ifeq ($1,runtimestats)
    PONY_USES += -DPONY_USE_RUNTIMESTATS=true
  else ifeq ($1,runtimestats_messages)
    PONY_USES += -DPONY_USE_RUNTIMESTATS_MESSAGES=true
  else ifeq ($1,pool_memalign)
    PONY_USES += -DPONY_USE_POOL_MEMALIGN=true
  else ifeq ($1,pool_message_passing)
    PONY_USES += -DPONY_USE_POOL_MESSAGE_PASSING=true
  else
    $$(error ERROR: Unknown use option specified: $1)
  endif
endef

ifdef use
  ifneq (${MAKECMDGOALS}, configure)
    $(error You can only specify use= for 'make configure')
	else
    $(foreach useitem,$(sort $(subst $(comma),$(space),$(use))),$(eval $(call USE_CHECK,$(useitem))))
  endif
endif

ifneq ($(findstring lldb,$(usedebugger)),)
  debuggercmd := $(usedebugger) --batch --one-line "breakpoint set --name main" --one-line run --one-line "process handle SIGINT --pass true --stop false" --one-line "process handle SIGUSR2 --pass true --stop false"  --one-line "thread continue" --one-line-on-crash "frame variable" --one-line-on-crash "register read" --one-line-on-crash "bt all" --one-line-on-crash "quit 1" --
  testextras := --gtest_throw_on_failure
else ifneq ($(findstring gdb,$(usedebugger)),)
  debuggercmd := $(usedebugger) --quiet --batch --return-child-result --eval-command="set confirm off" --eval-command="set pagination off" --eval-command="handle SIGINT nostop pass" --eval-command="handle SIGUSR2 nostop pass" --eval-command=run  --eval-command="info args" --eval-command="info locals" --eval-command="info registers" --eval-command="thread apply all bt full" --eval-command=quit --args
else ifneq ($(strip $(usedebugger)),)
  $(error Unknown debugger: '$(usedebugger)')
endif

.DEFAULT_GOAL := build
.PHONY: all libs cleanlibs configure cross-configure build test test-ci test-check-version test-core test-stdlib-debug test-stdlib-release test-examples test-stress test-validate-grammar clean

libs:
	$(SILENT)mkdir -p '$(libsBuildDir)'
	$(SILENT)cd '$(libsBuildDir)' && env CC="$(CC)" CXX="$(CXX)" cmake -B '$(libsBuildDir)' -S '$(libsSrcDir)' -DPONY_PIC_FLAG=$(pic_flag) -DCMAKE_INSTALL_PREFIX="$(libsOutDir)" -DCMAKE_BUILD_TYPE="$(llvm_config)" -DLLVM_TARGETS_TO_BUILD="$(llvm_archs)" $(CMAKE_FLAGS)
	$(SILENT)cd '$(libsBuildDir)' && env CC="$(CC)" CXX="$(CXX)" cmake --build '$(libsBuildDir)' --target install --config $(llvm_config) -- $(build_flags)

cleanlibs:
	$(SILENT)rm -rf '$(libsBuildDir)'
	$(SILENT)rm -rf '$(libsOutDir)'

configure:
	$(SILENT)mkdir -p '$(buildDir)'
	$(SILENT)cd '$(buildDir)' && env CC="$(CC)" CXX="$(CXX)" cmake -B '$(buildDir)' -S '$(srcDir)' -DCMAKE_BUILD_TYPE=$(config) -DPONY_ARCH=$(arch) -DPONY_PIC_FLAG=$(pic_flag) -DPONYC_VERSION=$(tag) -DCMAKE_C_FLAGS="-march=$(arch) -mtune=$(tune)" -DCMAKE_CXX_FLAGS="-march=$(arch) -mtune=$(tune)" $(BITCODE_FLAGS) $(LTO_CONFIG_FLAGS) $(CMAKE_FLAGS) $(PONY_USES)

all: build

build:
	$(SILENT)cd '$(buildDir)' && env CC="$(CC)" CXX="$(CXX)" cmake --build '$(buildDir)' --config $(config) --target all -- $(build_flags)

ponyc:
	$(SILENT)cd '$(buildDir)' && env CC="$(CC)" CXX="$(CXX)" cmake --build '$(buildDir)' --config $(config) --target ponyc -- $(build_flags)

crossBuildDir := $(srcDir)/build/$(arch)/build_$(config)

cross-libponyrt:
	$(SILENT)mkdir -p $(crossBuildDir)
	$(SILENT)cd '$(crossBuildDir)' && env CC=$(CC) CXX=$(CXX) cmake -B '$(crossBuildDir)' -S '$(srcDir)' -DCMAKE_CROSSCOMPILING=true -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=$(arch) -DCMAKE_C_COMPILER=$(CC) -DCMAKE_CXX_COMPILER=$(CXX) -DPONY_CROSS_LIBPONYRT=true -DCMAKE_BUILD_TYPE=$(config) -DCMAKE_C_FLAGS="$(cross_cflags)" -DCMAKE_CXX_FLAGS="$(cross_cflags)" -DPONY_ARCH=$(arch) -DPONYC_VERSION=$(version) -DLL_FLAGS="$(cross_llc_flags)"  $(CMAKE_FLAGS)
	$(SILENT)cd '$(crossBuildDir)' && env CC=$(CC) CXX=$(CXX) cmake --build '$(crossBuildDir)' --config $(config) --target libponyrt -- $(build_flags)

test: all test-core test-stdlib-release test-examples

test-ci: all test-check-version test-core test-stdlib-debug test-stdlib-release test-examples test-validate-grammar

test-cross-ci: cross_args=--triple=$(cross_triple) --cpu=$(cross_cpu) --link-arch=$(cross_arch) --linker='$(cross_linker)' $(cross_ponyc_args)
test-cross-ci: debuggercmd=
test-cross-ci: test-core test-stdlib-debug test-stdlib-release test-examples

test-check-version: all
	$(SILENT)cd '$(outDir)' && ./ponyc --version

test-core: all test-libponyrt test-libponyc test-full-programs-debug test-full-programs-release

test-libponyrt: all
	$(SILENT)cd '$(outDir)' && $(debuggercmd) ./libponyrt.tests --gtest_shuffle $(testextras)

test-libponyc: all
	$(SILENT)cd '$(outDir)' && $(debuggercmd) ./libponyc.tests --gtest_shuffle $(testextras)

ifeq ($(shell uname -s),FreeBSD)
  num_cores := `sysctl -n hw.ncpu`
else ifeq ($(shell uname -s),Darwin)
  num_cores := `sysctl -n hw.ncpu`
else
  num_cores := `nproc --all`
endif

test-full-programs-release: all
	@mkdir -p $(outDir)/full-program-tests/release
	$(SILENT)cd '$(outDir)' && $(buildDir)/test/full-program-runner/full-program-runner --debugger='$(debuggercmd)' --timeout_s=120 --max_parallel=$(num_cores) --ponyc=$(outDir)/ponyc --output=$(outDir)/full-program-tests/release --test_lib=$(outDir)/test_lib $(srcDir)/test/full-program-tests

test-full-programs-debug: all
	@mkdir -p $(outDir)/full-program-tests/debug
	$(SILENT)cd '$(outDir)' && $(buildDir)/test/full-program-runner/full-program-runner --debugger='$(debuggercmd)' --timeout_s=120 --max_parallel=$(num_cores) --ponyc=$(outDir)/ponyc --debug --output=$(outDir)/full-program-tests/debug --test_lib=$(outDir)/test_lib $(srcDir)/test/full-program-tests

test-stdlib-release: all
	$(SILENT)cd '$(outDir)' && PONYPATH=.:$(PONYPATH) ./ponyc -b stdlib-release --pic --checktree $(cross_args) ../../packages/stdlib && echo Built `pwd`/stdlib-release && $(cross_runner) $(debuggercmd) ./stdlib-release --sequential

test-stdlib-debug: all
	$(SILENT)cd '$(outDir)' && PONYPATH=.:$(PONYPATH) ./ponyc -d -b stdlib-debug --pic --strip --checktree $(cross_args) ../../packages/stdlib && echo Built `pwd`/stdlib-debug && $(cross_runner) $(debuggercmd) ./stdlib-debug --sequential

test-examples: all
	$(SILENT)cd '$(outDir)' && PONYPATH=.:$(PONYPATH) find ../../examples/*/* -name '*.pony' -print | xargs -n 1 dirname | sort -u | grep -v ffi- | xargs -n 1 -I {} ./ponyc -d -s --checktree -o {} {}

test-validate-grammar: all
	$(SILENT)cd '$(outDir)' && ./ponyc --antlr >> pony.g.new && diff ../../pony.g pony.g.new

test-cross-stress-release: cross_args=--triple=$(cross_triple) --cpu=$(cross_cpu) --link-arch=$(cross_arch) --linker='$(cross_linker)' $(cross_ponyc_args)
test-cross-stress-release: debuggercmd=
test-cross-stress-release: test-stress-release
test-stress-release: all
	$(SILENT)cd '$(outDir)' && PONYPATH=.:$(PONYPATH) ./ponyc -b ubench --pic $(cross_args) ../../test/rt-stress/string-message-ubench && echo Built `pwd`/ubench && $(cross_runner) $(debuggercmd) ./ubench --pingers 320 --initial-pings 5 --report-count 40 --report-interval 300 --ponynoblock --ponynoscale

test-cross-stress-debug: cross_args=--triple=$(cross_triple) --cpu=$(cross_cpu) --link-arch=$(cross_arch) --linker='$(cross_linker)' $(cross_ponyc_args)
test-cross-stress-debug: debuggercmd=
test-cross-stress-debug: test-stress-debug
test-stress-debug: all
	$(SILENT)cd '$(outDir)' && PONYPATH=.:$(PONYPATH) ./ponyc -d -b ubench --pic $(cross_args) ../../test/rt-stress/string-message-ubench && echo Built `pwd`/ubench && $(cross_runner) $(debuggercmd) ./ubench --pingers 320 --initial-pings 5 --report-count 40 --report-interval 300 --ponynoblock --ponynoscale

test-cross-stress-with-cd-release: cross_args=--triple=$(cross_triple) --cpu=$(cross_cpu) --link-arch=$(cross_arch) --linker='$(cross_linker)' $(cross_ponyc_args)
test-cross-stress-with-cd-release: debuggercmd=
test-cross-stress-with-cd-release: test-stress-with-cd-release
test-stress-with-cd-release: all
	$(SILENT)cd '$(outDir)' && PONYPATH=.:$(PONYPATH) ./ponyc -b ubench --pic $(cross_args) ../../test/rt-stress/string-message-ubench && echo Built `pwd`/ubench && $(cross_runner) $(debuggercmd) ./ubench --pingers 320 --initial-pings 5 --report-count 40 --report-interval 300 --ponynoscale

test-cross-stress-with-cd-debug: cross_args=--triple=$(cross_triple) --cpu=$(cross_cpu) --link-arch=$(cross_arch) --linker='$(cross_linker)' $(cross_ponyc_args)
test-cross-stress-with-cd-debug: debuggercmd=
test-cross-stress-with-cd-debug: test-stress-with-cd-debug
test-stress-with-cd-debug: all
	$(SILENT)cd '$(outDir)' && PONYPATH=.:$(PONYPATH) ./ponyc -d -b ubench --pic $(cross_args) ../../test/rt-stress/string-message-ubench && echo Built `pwd`/ubench && $(cross_runner) $(debuggercmd) ./ubench --pingers 320 --initial-pings 5 --report-count 40 --report-interval 300 --ponynoscale

clean:
	$(SILENT)([ -d '$(buildDir)' ] && cd '$(buildDir)' && cmake --build '$(buildDir)' --config $(config) --target clean) || true
	$(SILENT)rm -rf $(crossBuildDir)
	$(SILENT)rm -rf $(buildDir)
	$(SILENT)rm -rf $(outDir)*

distclean:
	$(SILENT)([ -d build ] && rm -rf build) || true

install: build
	@mkdir -p $(ponydir)/bin
	@mkdir -p $(ponydir)/lib/$(arch)
	@mkdir -p $(ponydir)/include/pony/detail
	$(SILENT)cp $(buildDir)/src/libponyrt/libponyrt.a $(ponydir)/lib/$(arch)
	$(SILENT)if [ -f $(outDir)/libponyc.a ]; then cp $(outDir)/libponyc.a $(ponydir)/lib/$(arch); fi
	$(SILENT)if [ -f $(outDir)/libponyc-standalone.a ]; then cp $(outDir)/libponyc-standalone.a $(ponydir)/lib/$(arch); fi
	$(SILENT)if [ -f $(outDir)/libponyrt-pic.a ]; then cp $(outDir)/libponyrt-pic.a $(ponydir)/lib/$(arch); fi
	$(SILENT)cp $(outDir)/ponyc $(ponydir)/bin
	$(SILENT)cp src/libponyrt/pony.h $(ponydir)/include
	$(SILENT)cp src/common/pony/detail/atomics.h $(ponydir)/include/pony/detail
	$(SILENT)cp -r packages $(ponydir)/
ifeq ($(symlink),yes)
	@mkdir -p $(prefix)/bin
	@mkdir -p $(prefix)/lib
	@mkdir -p $(prefix)/include/pony/detail
	$(SILENT)ln -s -f $(ponydir)/bin/ponyc $(prefix)/bin/ponyc
	$(SILENT)if [ -f $(ponydir)/lib/$(arch)/libponyc.a ]; then ln -s -f $(ponydir)/lib/$(arch)/libponyc.a $(prefix)/lib/libponyc.a; fi
	$(SILENT)if [ -f $(ponydir)/lib/$(arch)/libponyc-standalone.a ]; then ln -s -f $(ponydir)/lib/$(arch)/libponyc-standalone.a $(prefix)/lib/libponyc-standalone.a; fi
	$(SILENT)if [ -f $(ponydir)/lib/$(arch)/libponyrt.a ]; then ln -s -f $(ponydir)/lib/$(arch)/libponyrt.a $(prefix)/lib/libponyrt.a; fi
	$(SILENT)if [ -f $(ponydir)/lib/$(arch)/libponyrt-pic.a ]; then ln -s -f $(ponydir)/lib/$(arch)/libponyrt-pic.a $(prefix)/lib/libponyrt-pic.a; fi
	$(SILENT)ln -s -f $(ponydir)/include/pony.h $(prefix)/include/pony.h
	$(SILENT)ln -s -f $(ponydir)/include/pony/detail/atomics.h $(prefix)/include/pony/detail/atomics.h
endif

uninstall:
	-$(SILENT)rm -rf $(ponydir) ||:
	-$(SILENT)rm -f $(prefix)/bin/ponyc ||:
	-$(SILENT)rm -f $(prefix)/lib/libponyc*.a ||:
	-$(SILENT)rm -f $(prefix)/lib/libponyrt*.a ||:
	-$(SILENT)rm -rf $(prefix)/lib/pony ||:
	-$(SILENT)rm -f $(prefix)/include/pony.h ||:
	-$(SILENT)rm -rf $(prefix)/include/pony ||:
