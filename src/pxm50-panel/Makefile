# Buildroot übergibt CC/CFLAGS/LDFLAGS – eigene Flags daher separat
LVGL_DIR   := lvgl
LVGL_SRCS  := $(shell find $(LVGL_DIR)/src -name '*.c')
SRCS       := main.c $(LVGL_SRCS)
OBJS       := $(SRCS:.c=.o)
PANEL_CFLAGS := -O2 -I. -I$(LVGL_DIR) -DLV_CONF_INCLUDE_SIMPLE -Wall -Wno-unused-parameter
LDLIBS     := -lcurl -lcjson -ljpeg -lts -lpthread -lm

all: pxm50-panel

pxm50-panel: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c lv_conf.h
	$(CC) $(CFLAGS) $(PANEL_CFLAGS) -c $< -o $@

clean:
	rm -f pxm50-panel $(OBJS)

.PHONY: all clean
