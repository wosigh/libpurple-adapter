
SOURCES=Src/LibpurpleAdapter.c
OBJECTS=$(SOURCES:.c=.o)

CFLAGS=-g `pkg-config --cflags glib-2.0 purple` -DDEVICE -IIncs -I$(STAGING_INCDIR) -I$(STAGING_INCDIR)/cjson
LDFLAGS=-Wl,-rpath=$(STAGING_LIBDIR) -L$(STAGING_LIBDIR) `pkg-config --libs glib-2.0 purple` -llunaservice -lcjson
all: LibpurpleAdapter 

.c.o:
	$(CC) $(CFLAGS) $< -c -o $@

LibpurpleAdapter: $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@

clean:
	rm -f LibpurpleAdapter Src/*.o
