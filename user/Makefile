CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11

COMMON_OBJS := vpipe-common.o
BINARIES := \
	vpipe-capture-mmap \
	vpipe-capture-read \
	vpipe-capture-m2m \
	vpipe-capture-dmabuf \
	vpipe-bench-fixture \
	vpipe-meta-drain \
	vpipe-fixture-feed \
	vpipe-cv-ref \
	vpipe-dump-pgm \
	vpipe-pgm-diff
TEST_BINARIES := vpipe-unit-test

.PHONY: all clean test

all: $(BINARIES) $(TEST_BINARIES)

vpipe-capture-mmap: vpipe-capture-mmap.o $(COMMON_OBJS)
vpipe-capture-read: vpipe-capture-read.o $(COMMON_OBJS)
vpipe-capture-m2m: vpipe-capture-m2m.o $(COMMON_OBJS)
vpipe-capture-dmabuf: vpipe-capture-dmabuf.o $(COMMON_OBJS)
vpipe-bench-fixture: vpipe-bench-fixture.o $(COMMON_OBJS)
vpipe-meta-drain: vpipe-meta-drain.o $(COMMON_OBJS)
vpipe-fixture-feed: vpipe-fixture-feed.o $(COMMON_OBJS)
vpipe-cv-ref: vpipe-cv-ref.o $(COMMON_OBJS)
vpipe-dump-pgm: vpipe-dump-pgm.o $(COMMON_OBJS)
vpipe-pgm-diff: vpipe-pgm-diff.o $(COMMON_OBJS)
vpipe-unit-test: vpipe-unit-test.o $(COMMON_OBJS)

test: vpipe-unit-test
	./vpipe-unit-test

clean:
	$(RM) $(BINARIES) $(TEST_BINARIES) *.o
