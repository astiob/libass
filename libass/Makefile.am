AM_CFLAGS = -std=gnu99 -Wall -Wextra -Wno-sign-compare -Wno-unused-parameter \
            -Werror-implicit-function-declaration -Wstrict-prototypes        \
            -Wpointer-arith -Wredundant-decls

LIBASS_LT_CURRENT = 5
LIBASS_LT_REVISION = 0
LIBASS_LT_AGE = 0

yasm_verbose = $(yasm_verbose_$(V))
yasm_verbose_ = $(yasm_verbose_$(AM_DEFAULT_VERBOSITY))
yasm_verbose_0 = @echo "  YASM    " $@;

.asm.lo:
	$(yasm_verbose)$(LIBTOOL) $(AM_V_lt) --mode=compile $(AS) $(ASFLAGS) -o $@ $< -prefer-non-pic

SRC_X86 = x86/cpuid.asm x86/cpuid.h x86/tile_func.asm x86/tile_func.h

lib_LTLIBRARIES = libass.la
libass_la_SOURCES = ass.c ass_cache.c ass_font.c ass_fontconfig.c ass_render.c \
                    ass_utils.c ass_bitmap.c ass_library.c ass_bitmap.h \
                    ass_cache.h ass_fontconfig.h ass_font.h ass.h \
                    ass_library.h ass_types.h ass_utils.h ass_drawing.c \
                    ass_drawing.h ass_cache_template.h ass_render.h \
                    ass_parse.c ass_parse.h ass_render_api.c ass_shaper.c \
                    ass_shaper.h ass_strtod.c ass_tile.h ass_tile.c \
                    ass_tile_func.h ass_tile_func.c ass_rasterizer.h ass_rasterizer.c

libass_la_LDFLAGS = -no-undefined -version-info $(LIBASS_LT_CURRENT):$(LIBASS_LT_REVISION):$(LIBASS_LT_AGE)
libass_la_LDFLAGS += -export-symbols $(srcdir)/libass.sym

if ASM
if X86
libass_la_SOURCES += $(SRC_X86)
endif
endif

assheadersdir = $(includedir)/ass
dist_assheaders_HEADERS = ass.h ass_types.h

EXTRA_DIST = libass.sym x86/x86inc.asm
