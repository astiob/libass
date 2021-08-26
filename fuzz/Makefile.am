AM_CFLAGS = -Wall

noinst_PROGRAMS = fuzzcon
fuzzcon_SOURCES = fuzzcon.c
fuzzcon_CPPFLAGS = -I$(top_srcdir)/libass
fuzzcon_LDADD = $(top_builddir)/libass/.libs/libass.a
fuzzcon_LDFLAGS = $(AM_LDFLAGS) -static
