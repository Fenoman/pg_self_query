# contrib/pg_self_query/Makefile

MODULE_big = pg_self_query
OBJS = pg_self_query.o signal_handler.o $(WIN32RES)
EXTENSION = pg_self_query
EXTVERSION = 1.0
DATA_built = $(EXTENSION)--$(EXTVERSION).sql
PGFILEDESC = "pg_self_query - facility to get current query for logging"

EXTRA_CLEAN = ./isolation_output $(EXTENSION)--$(EXTVERSION).sql \
	Dockerfile ./tests/*.pyc

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_self_query
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

$(EXTENSION)--$(EXTVERSION).sql: init.sql
	cat $^ > $@

check: isolationcheck

ISOLATIONCHECKS=corner_cases

submake-isolation:
	$(MAKE) -C $(top_builddir)/src/test/isolation all

isolationcheck: | submake-isolation temp-install
	$(MKDIR_P) isolation_output
	$(pg_isolation_regress_check) \
	  --temp-config $(top_srcdir)/contrib/pg_self_query/test.conf \
      --outputdir=isolation_output \
	  $(ISOLATIONCHECKS)

isolationcheck-install-force: all | submake-isolation temp-install
	$(MKDIR_P) isolation_output
	$(pg_isolation_regress_installcheck) \
      --outputdir=isolation_output \
	  $(ISOLATIONCHECKS)

.PHONY: isolationcheck isolationcheck-install-force check

temp-install: EXTRA_INSTALL=contrib/pg_self_query
