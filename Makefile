EXTENSION = acdat
EXTVERSION = 0.2.0
DISTVERSION = 0.2.0
MODULE_big = acdat
OBJS = src/acdat_pg.o src/acdat_core.o src/acdat_format.o
DATA = sql/acdat--0.1.0.sql sql/acdat--0.1.0--0.2.0.sql \
	   sql/acdat--$(EXTVERSION).sql
REGRESS = acdat_basic acdat_errors
PGFILEDESC = "acdat - compiled multi-pattern matching for PostgreSQL"

PG_CPPFLAGS = -Isrc
PG_CFLAGS = -std=c11
EXTRA_CLEAN = build $(EXTENSION)-$(DISTVERSION).zip

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
		$${INPUT_MIB:-64} $${PROFILE:-ascii-miss} \
		$${RUNTIME:-materialized} $${BASE_CANDIDATE_LIMIT:-0}

bench-matrix: $(CORE_BENCH)
	bash test/bench_matrix.sh ./$(CORE_BENCH)

dump-restore-test:
	bash test/dump_restore.sh

cache-smoke-test:
	bash test/cache_smoke.sh

upgrade-test:
	bash test/upgrade.sh

pg-matrix-test:
	bash test/pg_matrix.sh

cross-version-test:
	bash test/cross_version.sh

check-meta:
	@command -v jq >/dev/null 2>&1 || \
		{ echo "check-meta requires jq" >&2; exit 1; }
	jq -e --arg extension "$(EXTENSION)" --arg version "$(EXTVERSION)" '\
		.name == $$extension and \
		.version == $$version and \
		.provides[$$extension].version == $$version and \
		.provides[$$extension].file == "sql/$(EXTENSION)--$(EXTVERSION).sql" and \
		."meta-spec".version == "1.0.0" \
	' META.json >/dev/null
	test -f "sql/$(EXTENSION)--$(EXTVERSION).sql"
	test -f "sql/$(EXTENSION)--0.1.0--$(EXTVERSION).sql"
	grep -Eq "^default_version[[:space:]]*=[[:space:]]*'$(EXTVERSION)'[[:space:]]*$$" \
		$(EXTENSION).control
	# Version 0.2.0 changes C code only; its full install SQL must stay identical.
	cmp -s sql/$(EXTENSION)--0.1.0.sql \
		sql/$(EXTENSION)--$(EXTVERSION).sql

dist: check-meta
	@test -z "$$(git status --porcelain)" || \
		{ echo "dist requires a clean tree; commit first" >&2; exit 1; }
	git archive --format zip --prefix=$(EXTENSION)-$(DISTVERSION)/ \
		-o $(EXTENSION)-$(DISTVERSION).zip HEAD

.PHONY: core-test core-bench bench-matrix dump-restore-test cache-smoke-test \
	upgrade-test \
	pg-matrix-test cross-version-test check-meta dist
