EXTENSION = acdat
MODULE_big = acdat
OBJS = src/acdat_pg.o src/acdat_core.o src/acdat_format.o
DATA = sql/acdat--0.1.0.sql
REGRESS = acdat_basic acdat_errors
PGFILEDESC = "acdat - compiled multi-pattern matching for PostgreSQL"

PG_CPPFLAGS = -Isrc
PG_CFLAGS = -std=c11
EXTRA_CLEAN = build

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

CORE_CC ?= cc
CORE_CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -O1 -g
CORE_SANITIZERS ?= -fsanitize=address,undefined -fno-omit-frame-pointer
CORE_TEST = build/acdat-core-test
CORE_BENCH = build/acdat-core-bench

$(CORE_TEST): src/acdat_core.c src/acdat_core.h src/acdat_format.c \
		src/acdat_format.h test/core_test.c
	mkdir -p build
	$(CORE_CC) $(CORE_CFLAGS) $(CORE_SANITIZERS) -Isrc \
		src/acdat_core.c src/acdat_format.c test/core_test.c -o $@

core-test: $(CORE_TEST)
	./$(CORE_TEST)

$(CORE_BENCH): src/acdat_core.c src/acdat_core.h src/acdat_format.c \
		src/acdat_format.h test/core_bench.c
	mkdir -p build
	$(CORE_CC) -std=c11 -Wall -Wextra -Werror -pedantic -O3 -DNDEBUG \
		-Isrc src/acdat_core.c src/acdat_format.c test/core_bench.c -o $@

core-bench: $(CORE_BENCH)
	./$(CORE_BENCH) $${PATTERNS:-10000} $${PATTERN_BYTES:-16} \
		$${INPUT_MIB:-64}

dump-restore-test:
	bash test/dump_restore.sh

cache-smoke-test:
	bash test/cache_smoke.sh

pg-matrix-test:
	bash test/pg_matrix.sh

cross-version-test:
	bash test/cross_version.sh

.PHONY: core-test core-bench dump-restore-test cache-smoke-test \
	pg-matrix-test cross-version-test
