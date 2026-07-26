# ============================================================================
# HYDRA build
# ============================================================================
CC      ?= cc
CSTD    ?= -std=c99
WARN    ?= -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-align -Wstrict-prototypes
OPT     ?= -O3 -fomit-frame-pointer -funroll-loops
THREADS ?= -DHZ_THREADS -pthread
CFLAGS  ?= $(CSTD) $(WARN) $(OPT) $(THREADS)
LDFLAGS ?= -pthread

SRCDIR  := src
OBJDIR  := build
BINDIR  := bin

LIBSRC  := $(SRCDIR)/hydra.c $(SRCDIR)/hz_tables.c $(SRCDIR)/hz_model.c \
           $(SRCDIR)/hz_fast.c $(SRCDIR)/hz_mid.c $(SRCDIR)/hz_cm.c $(SRCDIR)/hz_sgi.c $(SRCDIR)/hz_filter.c
LIBOBJ  := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(LIBSRC))

BIN     := $(BINDIR)/hydra
LIB     := $(OBJDIR)/libhydra.a

.PHONY: all clean test bench asan check
all: $(BIN)

$(OBJDIR):
	@mkdir -p $(OBJDIR)
$(BINDIR):
	@mkdir -p $(BINDIR)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR) -c $< -o $@

$(LIB): $(LIBOBJ)
	ar rcs $@ $^

$(BIN): $(SRCDIR)/main.c $(LIB) | $(BINDIR)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=199309L -I$(SRCDIR) $< $(LIB) -o $@ $(LDFLAGS)

# ---- test harness ----------------------------------------------------------
$(BINDIR)/hydra_test: tests/test_hydra.c $(LIB) | $(BINDIR)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=199309L -I$(SRCDIR) $< $(LIB) -o $@ $(LDFLAGS)

test: $(BINDIR)/hydra_test
	./$(BINDIR)/hydra_test

# sanitized build: catches the memory and integer bugs a compressor is prone to
asan: CFLAGS := $(CSTD) $(WARN) $(THREADS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean $(BINDIR)/hydra_test
	ASAN_OPTIONS=detect_leaks=1 ./$(BINDIR)/hydra_test

bench: $(BIN)
	@bash bench/run_bench.sh

check: test

clean:
	rm -rf $(OBJDIR) $(BINDIR)

$(BINDIR)/bench_mem: bench/bench_mem.c $(LIB) | $(BINDIR)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=199309L -I$(SRCDIR) $< $(LIB) -o $@ $(LDFLAGS)

benchmem: $(BINDIR)/bench_mem
	@for f in $(CORPUS)/*; do ./$(BINDIR)/bench_mem $$f; done

$(BINDIR)/proof: tools/proof.c $(LIB) | $(BINDIR)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=199309L -I$(SRCDIR) $< $(LIB) -o $@ $(LDFLAGS)

proof: $(BINDIR)/proof
	./$(BINDIR)/proof 5 3

$(BINDIR)/proof2: tools/proof2.c $(LIB) | $(BINDIR)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=199309L -I$(SRCDIR) $< $(LIB) -o $@ $(LDFLAGS)

$(BINDIR)/reorder_study: tools/reorder_study.c $(LIB) | $(BINDIR)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=199309L -I$(SRCDIR) $< $(LIB) -o $@ $(LDFLAGS)

$(BINDIR)/nova: tools/nova.c $(LIB) | $(BINDIR)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=199309L -I$(SRCDIR) $< $(LIB) -o $@ $(LDFLAGS)

$(BINDIR)/seedsearch: tools/seedsearch.c $(LIB) | $(BINDIR)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=199309L -I$(SRCDIR) $< $(LIB) -o $@ $(LDFLAGS)
