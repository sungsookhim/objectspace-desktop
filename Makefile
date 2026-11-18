# WPS for Linux: object core.
#
#   make          build the runtime, the core classes and the tests
#   make test     run the unit tests (with AddressSanitizer and UBSan if installed)
#   make m0       build the M0 prototypes (see proto/m0)
#   make clean

CC      ?= cc
PYTHON  ?= python3
B       := build
GEN     := $(B)/gen
CFLAGS  ?= -O1 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter
# AddressSanitizer and UBSan when their runtimes are installed (Fedora:
# libasan, libubsan); otherwise a plain build. Override with SAN=.
SAN     ?= $(shell echo 'int main(void){return 0;}' | $(CC) -fsanitize=address,undefined -x c - \
             -o /dev/null 2>/dev/null && echo -fsanitize=address,undefined -fno-omit-frame-pointer)
INC     := -Iruntime/include -Iclasses -I$(GEN)

RUNTIME := runtime/src/wpo.c runtime/src/wpo_loop_posix.c runtime/src/wpo_plugin_posix.c
WPI     := $(wildcard classes/*.wpi)
CLASSES := $(wildcard classes/*.c) $(patsubst classes/%.wpi,$(GEN)/%.reg.c,$(WPI))

# Test class TBase: version 1.1 is "installed"; plug-ins are built against
# 1.0 (older, must load), 1.2 (newer, refused) and 2.0 (other major, refused).
TV      := $(B)/tests

all: $(TV)/test_runtime $(TV)/test_loop plugins

$(GEN)/.stamp: $(WPI) tools/wpoc.py
	$(PYTHON) tools/wpoc.py -I classes -o $(GEN) $(WPI)
	@touch $@

$(GEN)/%.reg.c: $(GEN)/.stamp ;

$(B)/gen-%/.stamp: tests/versions/%/TBase.wpi tests/plugins/TChild.wpi tools/wpoc.py
	$(PYTHON) tools/wpoc.py -o $(B)/gen-$* $<
	$(PYTHON) tools/wpoc.py -I tests/versions/$* -o $(B)/gen-$*/plugin tests/plugins/TChild.wpi
	@touch $@

$(TV)/test_runtime: tests/test_runtime.c $(RUNTIME) $(GEN)/.stamp $(B)/gen-v11/.stamp tests/TBase_v11.c
	@mkdir -p $(TV)
	$(CC) $(CFLAGS) $(SAN) $(INC) -I$(B)/gen-v11 -rdynamic -o $@ tests/test_runtime.c $(RUNTIME) \
	    $(CLASSES) tests/TBase_v11.c $(B)/gen-v11/TBase.reg.c -ldl -lpthread

$(TV)/test_loop: tests/test_loop.c runtime/src/wpo_loop_posix.c runtime/include/wpo_loop.h
	@mkdir -p $(TV)
	$(CC) $(CFLAGS) $(SAN) $(INC) -o $@ tests/test_loop.c runtime/src/wpo_loop_posix.c -lpthread

PLUGINS := $(TV)/plugins/tchild_v10.so $(TV)/plugins/tchild_v12.so $(TV)/plugins/tchild_v20.so \
           $(TV)/plugins/myfolder.so
plugins: $(PLUGINS)

$(TV)/plugins/tchild_%.so: tests/plugins/TChild.c $(B)/gen-%/.stamp
	@mkdir -p $(TV)/plugins
	$(CC) $(CFLAGS) $(SAN) -fPIC -shared -Iruntime/include -I$(B)/gen-$*/plugin -I$(B)/gen-$* \
	    -o $@ tests/plugins/TChild.c $(B)/gen-$*/plugin/TChild.reg.c

$(TV)/plugins/myfolder.so: tests/plugins/MyFolder.c tests/plugins/MyFolder.wpi $(GEN)/.stamp
	@mkdir -p $(TV)/plugins $(B)/gen-myfolder
	$(PYTHON) tools/wpoc.py -I classes -o $(B)/gen-myfolder tests/plugins/MyFolder.wpi
	$(CC) $(CFLAGS) $(SAN) -fPIC -shared $(INC) -I$(B)/gen-myfolder \
	    -o $@ tests/plugins/MyFolder.c $(B)/gen-myfolder/MyFolder.reg.c

test: all
	$(TV)/test_runtime $(TV)/plugins
	$(TV)/test_loop

m0:
	$(MAKE) -C proto/m0 all

clean:
	rm -rf $(GEN) $(B)/gen-* $(TV)

.PHONY: all plugins test m0 clean
