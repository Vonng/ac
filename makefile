EXTENSION = acdat
MODULE_big = acdat
OBJS = src/acdat.o
DATA = sql/acdat--0.0.1.sql
PGFILEDESC = "acdat - experimental Aho-Corasick matcher"

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

