# A WPS plug-in, built with the SDK (the wps-devel package): make, then
# make install (your own plug-ins: ~/.config/wps/plugins), and log in again.
# (wpsctl plugin new NAME makes one like this; OSD_SDK and OSD_INC say
# where the SDK is, for a checkout: make OSD_SDK=... OSD_INC=...)
NAME    ?= OsdImageFile
SO      ?= $(shell echo $(NAME) | tr A-Z a-z).so
OSD_SDK ?= /usr/share/wps/sdk
OSD_INC ?= /usr/include/wps
CC      ?= cc
PYTHON  ?= python3

all: $(SO)

gen/$(NAME).reg.c: $(NAME).osdi
	mkdir -p gen
	$(PYTHON) $(OSD_SDK)/osdc.py -I $(OSD_SDK)/classes -o gen $(NAME).osdi

$(SO): $(NAME).c gen/$(NAME).reg.c
	$(CC) -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -fPIC -shared -I$(OSD_INC) -Igen -o $@ $(NAME).c gen/$(NAME).reg.c

install: $(SO)
	install -d $(HOME)/.config/wps/plugins
	install -m 755 $(SO) $(HOME)/.config/wps/plugins/

clean:
	rm -rf gen $(SO)

.PHONY: all install clean
