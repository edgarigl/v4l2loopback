all: test_dqbuf test_dmabuf test_consumer_sync consumer producer

test_consumer_sync: test_consumer_sync.c ../v4l2loopback.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Werror $< $(LDFLAGS) -o $@

consumer producer: common.h
