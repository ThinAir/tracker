noinst_PROGRAMS = \
	test-insert-or-replace \
	test-busy-handling \
	test-direct-query \
	test-bus-query \
	test-default-update \
	test-bus-update \
	test-class-signal \
	test-class-signal-performance \
	test-class-signal-performance-batch \
	test-update-array-performance

AM_VALAFLAGS = \
	--pkg gio-2.0 \
	$(BUILD_VALAFLAGS) \
	$(top_srcdir)/src/libtracker-sparql/tracker-sparql-$(TRACKER_API_VERSION).vapi \
	$(top_srcdir)/src/libtracker-bus/tracker-bus.vapi

AM_CPPFLAGS =\
	$(BUILD_VALACFLAGS) \
	-I$(top_srcdir)/src \
	-I$(top_builddir)/src \
	$(LIBTRACKER_SPARQL_CFLAGS)

LDADD =\
	$(top_builddir)/src/libtracker-data/libtracker-data.la \
	$(top_builddir)/src/libtracker-direct/libtracker-direct.la \
	$(top_builddir)/src/libtracker-bus/libtracker-bus.la \
	$(top_builddir)/src/libtracker-sparql-backend/libtracker-sparql-$(TRACKER_API_VERSION).la \
	$(top_builddir)/src/libtracker-common/libtracker-common.la \
	$(BUILD_LIBS)\
	$(LIBTRACKER_SPARQL_LIBS)

test_insert_or_replace_SOURCES = \
	test-insert-or-replace.vala

test_busy_handling_SOURCES = \
	test-busy-handling.vala

test_direct_query_SOURCES = \
	test-shared-query.vala \
	test-direct-query.vala

test_default_update_SOURCES = \
	test-shared-update.vala \
	test-default-update.vala

test_bus_query_SOURCES = \
	test-shared-query.vala \
	test-bus-query.vala

test_update_array_performance_SOURCES = \
	test-update-array-performance.c

test_bus_update_SOURCES = \
	test-shared-update.vala \
	test-bus-update.vala

test_class_signal_SOURCES = \
	test-class-signal.vala

test_class_signal_performance_SOURCES = \
	test-class-signal-performance.vala

test_class_signal_performance_batch_SOURCES = \
	test-class-signal-performance-batch.vala

EXTRA_DIST = meson.build
