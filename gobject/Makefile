CC = gcc
CFLAGS = -Wall -Iinclude -lcrypto -lssl -lpthread -ljansson `pkg-config --cflags glib-2.0 gobject-2.0`
LDFLAGS = `pkg-config --libs glib-2.0 gobject-2.0`

SRCS = src/nostr_event.c src/nostr_filter.c src/nostr_relay.c src/nostr_subscription.c src/nostr_simple_pool.c src/nostr_pointer.c src/nostr_relay_store.c src/crypto_utils_gobject.c src/nostr_async.c src/nostr_query_batcher.c
OBJS = $(SRCS:.c=.o)
TARGET = libnostr.a

all: $(TARGET) tests examples nostr-1.0.gir nostr-1.0.typelib

$(TARGET): $(OBJS)
    ar rcs $@ $^

nostr-1.0.gir: $(TARGET)
    g-ir-scanner --namespace=Nostr --nsversion=1.0 \
                 --warn-all \
                 --output=nostr-1.0.gir \
                 --cflags="$(CFLAGS)" --ldflags="$(LDFLAGS)" \
                 $(SRCS) \
                 -L. -lnostr \
                 --include=GObject-2.0

nostr-1.0.typelib: nostr-1.0.gir
    g-ir-compiler nostr-1.0.gir --output=nostr-1.0.typelib

examples: $(TARGET)
    $(CC) $(CFLAGS) examples/connect_publish.c -o connect_publish -L. -lnostr

tests: $(TARGET)
    $(CC) $(CFLAGS) tests/test_nostr_event.c -o test_nostr_event -L. -lnostr

clean:
    rm -f $(OBJS) $(TARGET) connect_publish test_nostr_event nostr-1.0.gir nostr-1.0.typelib