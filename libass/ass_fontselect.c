/*
 * Copyright (C) 2006 Evgeniy Stepanov <eugeni.stepanov@gmail.com>
 * Copyright (C) 2011 Grigori Goronzy <greg@chown.ath.cx>
 *
 * This file is part of libass.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"
#include "ass_compat.h"

#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <limits.h>
#include <ft2build.h>
#include <sys/types.h>
#include FT_FREETYPE_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_IDS_H
#include FT_TYPE1_TABLES_H
#include FT_TRUETYPE_TABLES_H

#ifdef CONFIG_CORETEXT
#include <CoreFoundation/CoreFoundation.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif
#ifdef CONFIG_ICONV
#include <iconv.h>
#endif

#include "ass_utils.h"
#include "ass.h"
#include "ass_library.h"
#include "ass_filesystem.h"
#include "ass_fontselect.h"
#include "ass_fontconfig.h"
#include "ass_coretext.h"
#include "ass_directwrite.h"
#include "ass_font.h"
#include "ass_string.h"

#define ABS(x) ((x) < 0 ? -(x) : (x))
#define MAX_FULLNAME 100

// internal font database element
// all strings are utf-8
struct font_info {
    int uid;            // unique font face id

    char **families;    // family name
    char **fullnames;   // list of localized fullnames (e.g. Arial Bold Italic)
    int n_family;
    int n_fullname;

    int slant;
    int weight;         // TrueType scale, 100-900
    int width;

    // how to access this face
    char *path;            // absolute path
    int index;             // font index inside font collections

    char *postscript_name; // can be used as an alternative to index to
                           // identify a font inside a collection

    char *extended_family;

    // font source
    ASS_FontProvider *provider;

    // private data for callbacks
    void *priv;

    // unused if the provider has a check_postscript function
    bool is_postscript;
};

struct font_selector {
    ASS_Library *library;
    FT_Library ftlibrary;

    // uid counter
    int uid;

    // fallbacks
    char *family_default;
    char *path_default;
    int index_default;

    // font database
    int n_font;
    int alloc_font;
    ASS_FontInfo *font_infos;

    ASS_FontProvider *default_provider;
    ASS_FontProvider *embedded_provider;
};

struct font_provider {
    ASS_FontSelector *parent;
    ASS_FontProviderFuncs funcs;
    void *priv;
};

typedef struct font_data_ft FontDataFT;
struct font_data_ft {
    ASS_Library *lib;
    FT_Face face;
    int idx;
};

static bool check_glyph_ft(void *data, uint32_t codepoint)
{
    FontDataFT *fd = (FontDataFT *)data;

    if (!codepoint)
        return true;

    return !!FT_Get_Char_Index(fd->face, codepoint);
}

static void destroy_font_ft(void *data)
{
    FontDataFT *fd = (FontDataFT *)data;

    FT_Done_Face(fd->face);
    free(fd);
}

static size_t
get_data_embedded(void *data, unsigned char *buf, size_t offset, size_t len)
{
    FontDataFT *ft = (FontDataFT *)data;
    ASS_Fontdata *fd = ft->lib->fontdata;
    int i = ft->idx;

    if (buf == NULL)
        return fd[i].size;

    if (offset >= fd[i].size)
        return 0;

    if (len > fd[i].size - offset)
        len = fd[i].size - offset;

    memcpy(buf, fd[i].data + offset, len);
    return len;
}

static ASS_FontProviderFuncs ft_funcs = {
    .get_data          = get_data_embedded,
    .check_glyph       = check_glyph_ft,
    .destroy_font      = destroy_font_ft,
};

static void load_fonts_from_dir(ASS_Library *library, const char *dir)
{
    ASS_Dir d;
    if (!ass_open_dir(&d, dir))
        return;
    while (true) {
        const char *name = ass_read_dir(&d);
        if (!name)
            break;
        if (name[0] == '.')
            continue;
        const char *path = ass_current_file_path(&d);
        if (!path)
            continue;
        ass_msg(library, MSGL_INFO, "Loading font file '%s'", path);
        size_t size = 0;
        void *data = ass_load_file(library, path, FN_DIR_LIST, &size);
        if (data) {
            ass_add_font(library, name, data, size);
            free(data);
        }
    }
    ass_close_dir(&d);
}

/**
 * \brief Create a bare font provider.
 * \param selector parent selector. The provider will be attached to it.
 * \param funcs callback/destroy functions
 * \param data private data of the provider
 * \return the font provider
 */
ASS_FontProvider *
ass_font_provider_new(ASS_FontSelector *selector, ASS_FontProviderFuncs *funcs,
                      void *data)
{
    ASS_FontProvider *provider = calloc(1, sizeof(ASS_FontProvider));
    if (provider == NULL)
        return NULL;

    provider->parent   = selector;
    provider->funcs    = *funcs;
    provider->priv     = data;

    return provider;
}

/**
 * Free all data associated with a FontInfo struct. Handles FontInfo structs
 * with incomplete allocations well.
 *
 * \param info FontInfo struct to free associated data from
 */
static void ass_font_provider_free_fontinfo(ASS_FontInfo *info)
{
    int j;

    if (info->fullnames) {
        for (j = 0; j < info->n_fullname; j++)
            free(info->fullnames[j]);
        free(info->fullnames);
    }

    if (info->families) {
        for (j = 0; j < info->n_family; j++)
            free(info->families[j]);
        free(info->families);
    }

    if (info->path)
        free(info->path);

    if (info->postscript_name)
        free(info->postscript_name);

    if (info->extended_family)
        free(info->extended_family);
}

struct name_encoding {
#ifdef CONFIG_CORETEXT
    CFStringEncoding cfencoding;
#define CFENCODING(x) encoding->cfencoding = x
#define CFENCODING_INIT .cfencoding = kCFStringEncodingInvalidId,
#else
#define CFENCODING(x)
#define CFENCODING_INIT
#endif

#ifdef _WIN32
    UINT win32_code_page;
#define WIN32_CODE_PAGE(x) encoding->win32_code_page = x
#define WIN32_CODE_PAGE_INIT .win32_code_page = -1,
#else
#define WIN32_CODE_PAGE(x)
#define WIN32_CODE_PAGE_INIT
#endif

#ifdef CONFIG_ICONV
#define MAX_ICONV_FROMCODES 4
    const char *iconv_fromcode[MAX_ICONV_FROMCODES];
    size_t n_iconv_fromcode;
#define ICONV_FROMCODE(x) \
    (assert(encoding->n_iconv_fromcode < MAX_ICONV_FROMCODES), \
     encoding->iconv_fromcode[encoding->n_iconv_fromcode++] = x)
#define ICONV_FROMCODE_INIT .iconv_fromcode = {0}, .n_iconv_fromcode = 0,
#else
#define ICONV_FROMCODE(x)
#define ICONV_FROMCODE_INIT
#endif
};

#define NAME_ENCODING_INIT { \
    CFENCODING_INIT \
    WIN32_CODE_PAGE_INIT \
    ICONV_FROMCODE_INIT \
}

static void identify_mac_encoding(struct name_encoding *encoding,
                                  int script_id, int language_id)
{
    switch (script_id) {
        case TT_MAC_ID_ROMAN:
            WIN32_CODE_PAGE(10000);
            ICONV_FROMCODE("MACINTOSH");
            switch (language_id) {
                case TT_MAC_LANGID_TURKISH:
                    CFENCODING(kCFStringEncodingMacTurkish);
                    WIN32_CODE_PAGE(10081);
                    ICONV_FROMCODE("MACTURKISH");
                    break;
                case TT_MAC_LANGID_CROATIAN:
                case TT_MAC_LANGID_SLOVENIAN:
                case TT_MAC_LANGID_SERBIAN:
                    CFENCODING(kCFStringEncodingMacCroatian);
                    WIN32_CODE_PAGE(10082);
                    ICONV_FROMCODE("MACCROATIAN");
                    break;
                case TT_MAC_LANGID_ICELANDIC:
                case TT_MAC_LANGID_FAEROESE:
                    CFENCODING(kCFStringEncodingMacIcelandic);
                    WIN32_CODE_PAGE(10079);
                    ICONV_FROMCODE("MAC-IS");
                    ICONV_FROMCODE("MACICELAND");
                    break;
                case TT_MAC_LANGID_ROMANIAN:
                    CFENCODING(kCFStringEncodingMacRomanian);
                    WIN32_CODE_PAGE(10010);
                    ICONV_FROMCODE("MACROMANIA");
                    break;
                case TT_MAC_LANGID_IRISH:
                case TT_MAC_LANGID_WELSH:
                case TT_MAC_LANGID_BRETON:
                case TT_MAC_LANGID_SCOTTISH_GAELIC:
                case TT_MAC_LANGID_MANX_GAELIC:
                    CFENCODING(kCFStringEncodingMacCeltic);
                    break;
                case TT_MAC_LANGID_IRISH_GAELIC:
                    CFENCODING(kCFStringEncodingMacGaelic);
                    break;
                case TT_MAC_LANGID_GREEK:
                    CFENCODING(kCFStringEncodingMacGreek);
                    WIN32_CODE_PAGE(10006);
                    ICONV_FROMCODE("MACGREEK");
                    break;
                default:
                    CFENCODING(kCFStringEncodingMacRoman);
            }
            break;
        case TT_MAC_ID_JAPANESE:
            CFENCODING(kCFStringEncodingMacJapanese);
            WIN32_CODE_PAGE(10001);
            ICONV_FROMCODE("SHIFT-JIS");
            break;
        case TT_MAC_ID_TRADITIONAL_CHINESE:
            CFENCODING(kCFStringEncodingMacChineseTrad);
            WIN32_CODE_PAGE(10002);
            ICONV_FROMCODE("BIG-5");
            break;
        case TT_MAC_ID_KOREAN:
            CFENCODING(kCFStringEncodingMacKorean);
            WIN32_CODE_PAGE(10003);
            ICONV_FROMCODE("EUC-KR");
            break;
        case TT_MAC_ID_ARABIC:
            WIN32_CODE_PAGE(10004);
            ICONV_FROMCODE("MACARABIC");
            switch (language_id) {
                case TT_MAC_LANGID_FARSI:
                    CFENCODING(kCFStringEncodingMacFarsi);
                    break;
                default:
                    CFENCODING(kCFStringEncodingMacArabic);
            }
            break;
        case TT_MAC_ID_HEBREW:
            CFENCODING(kCFStringEncodingMacHebrew);
            WIN32_CODE_PAGE(10005);
            ICONV_FROMCODE("MACHEBREW");
            break;
        case TT_MAC_ID_GREEK:  // not actually used (Roman is used)
            CFENCODING(kCFStringEncodingMacGreek);
            WIN32_CODE_PAGE(10006);
            ICONV_FROMCODE("MACGREEK");
            break;
        case TT_MAC_ID_RUSSIAN:
            CFENCODING(kCFStringEncodingMacCyrillic);
            WIN32_CODE_PAGE(10017);
            ICONV_FROMCODE("MAC-UK");
            ICONV_FROMCODE("MACUKRAINE");
            break;
        case TT_MAC_ID_DEVANAGARI:
            CFENCODING(kCFStringEncodingMacDevanagari);
            break;
        case TT_MAC_ID_GURMUKHI:
            CFENCODING(kCFStringEncodingMacGurmukhi);
            break;
        case TT_MAC_ID_GUJARATI:
            CFENCODING(kCFStringEncodingMacGujarati);
            break;
        case TT_MAC_ID_ORIYA:
            CFENCODING(kCFStringEncodingMacOriya);
            break;
        case TT_MAC_ID_BENGALI:
            CFENCODING(kCFStringEncodingMacBengali);
            break;
        case TT_MAC_ID_TAMIL:
            CFENCODING(kCFStringEncodingMacTamil);
            break;
        case TT_MAC_ID_TELUGU:
            CFENCODING(kCFStringEncodingMacTelugu);
            break;
        case TT_MAC_ID_KANNADA:
            CFENCODING(kCFStringEncodingMacKannada);
            break;
        case TT_MAC_ID_MALAYALAM:
            CFENCODING(kCFStringEncodingMacMalayalam);
            break;
        case TT_MAC_ID_SINHALESE:
            CFENCODING(kCFStringEncodingMacSinhalese);
            break;
        case TT_MAC_ID_BURMESE:
            CFENCODING(kCFStringEncodingMacBurmese);
            break;
        case TT_MAC_ID_KHMER:
            CFENCODING(kCFStringEncodingMacKhmer);
            break;
        case TT_MAC_ID_THAI:
            CFENCODING(kCFStringEncodingMacThai);
            WIN32_CODE_PAGE(10021);
            ICONV_FROMCODE("MACTHAI");
            break;
        case TT_MAC_ID_LAOTIAN:
            CFENCODING(kCFStringEncodingMacLaotian);
            break;
        case TT_MAC_ID_GEORGIAN:
            CFENCODING(kCFStringEncodingMacGeorgian);
            break;
        case TT_MAC_ID_ARMENIAN:
            CFENCODING(kCFStringEncodingMacArmenian);
            break;
        case TT_MAC_ID_SIMPLIFIED_CHINESE:
            CFENCODING(kCFStringEncodingMacChineseSimp);
            WIN32_CODE_PAGE(10008);
            ICONV_FROMCODE("EUC-CN");
            break;
        case TT_MAC_ID_TIBETAN:
            CFENCODING(kCFStringEncodingMacTibetan);
            break;
        case TT_MAC_ID_MONGOLIAN:
            CFENCODING(kCFStringEncodingMacMongolian);
            break;
        case TT_MAC_ID_GEEZ:
            switch (language_id) {
                case TT_MAC_LANGID_INUKTITUT:
                    CFENCODING(kCFStringEncodingMacInuit);
                    break;
                default:
                    CFENCODING(kCFStringEncodingMacEthiopic);
            }
            break;
        case TT_MAC_ID_SLAVIC:
            CFENCODING(kCFStringEncodingMacCentralEurRoman);
            WIN32_CODE_PAGE(10029);
            ICONV_FROMCODE("MAC-CENTRALEUROPE");
            ICONV_FROMCODE("MACCENTRALEUROPE");
            break;
        case TT_MAC_ID_VIETNAMESE:
            CFENCODING(kCFStringEncodingMacVietnamese);
            break;
        case TT_MAC_ID_SINDHI:
            CFENCODING(kCFStringEncodingMacExtArabic);
            break;
        case TT_MAC_ID_UNINTERP:
            CFENCODING(kCFStringEncodingMacVT100);
            break;
    }
}

static void decode_name(const struct name_encoding *encoding,
                        const unsigned char *string, size_t string_len,
                        char *buf, size_t buf_size)
{
    bool decode_attempted = false;

#ifdef CONFIG_CORETEXT
    if (!decode_attempted && encoding->cfencoding != kCFStringEncodingInvalidId) {
        decode_attempted = true;
        CFStringRef cfname =
            CFStringCreateWithBytes(
                NULL, string, string_len,
                encoding->cfencoding, false);
        if (cfname) {
            if (!CFStringGetCString(cfname, buf, buf_size,
                                    kCFStringEncodingUTF8))
                strcpy(buf, "(conversion failed)");
            CFRelease(cfname);
        }
    }
#endif

#ifdef _WIN32
    if (!decode_attempted && encoding->win32_code_page != -1u) {
        decode_attempted = true;
        int size =
            MultiByteToWideChar(encoding->win32_code_page, 0,
                                (const char *)string,
                                string_len, NULL, 0);
        if (size) {
            WCHAR *wbuf = calloc(size, sizeof(WCHAR));
            if (wbuf) {
                MultiByteToWideChar(encoding->win32_code_page, 0,
                                    (const char *)string,
                                    string_len, wbuf, size);
                memset(buf, 0, buf_size);
                WideCharToMultiByte(CP_UTF8, 0, wbuf,
                                    size, buf, buf_size - 1,
                                    NULL, NULL);
                free(wbuf);
            }
        }
    }
#endif

#ifdef CONFIG_ICONV
    size_t n_iconv_fromcode = encoding->n_iconv_fromcode;
    while (!decode_attempted && n_iconv_fromcode--) {
        iconv_t icdsc = iconv_open("UTF-8", encoding->iconv_fromcode[n_iconv_fromcode]);
        if (icdsc != (iconv_t)-1) {
            decode_attempted = true;
            char *inbuf = (char *)string;
            size_t inbytesleft = string_len;
            char *outbuf = buf;
            size_t outbytesleft = buf_size - 1;
            if (iconv(icdsc, &inbuf, &inbytesleft, &outbuf, &outbytesleft) != (size_t)-1)
                *outbuf = '\0';
            else if (errno == E2BIG)
                *outbuf = '\0';
            else
                strcpy(buf, "(conversion failed)");
            iconv_close(icdsc);
        }
    }
#endif

    if (!decode_attempted) {
        strncpy(buf, (const char *)string,
                FFMIN(buf_size, string_len));
        buf[buf_size - 1] = '\0';
    }
}

static void stringify_flag_enum(char *output, unsigned long flags, const char *const *names)
{
    strcpy(output, " (");

    for (; flags && *names; flags >>= 1, names++) {
        if (flags & 1) {
            strcat(output, *names);
            strcat(output, ",");
        }
    }

    if (flags) {
        strcat(output, "...,");
    }

    if (output[2])
        strchr(output, '\0')[-1] = ')';
    else
        output[0] = '\0';
}

/**
 * \brief Read basic metadata (names, weight, slant) from a FreeType face,
 * as required for the FontSelector for matching and sorting.
 * \param lib FreeType library
 * \param face FreeType face
 * \param fallback_family_name family name from outside source, used as last resort
 * \param info metadata, returned here
 * \return success
 */
static bool
get_font_info(ASS_Library *library, FT_Library lib, FT_Face face, const char *fallback_family_name,
              ASS_FontProviderMetaData *info)
{
    int i;
    int num_fullname = 0;
    int num_family   = 0;
    int num_names = FT_Get_Sfnt_Name_Count(face);
    int slant, weight;
    char *fullnames[MAX_FULLNAME];
    char *families[MAX_FULLNAME];
    PS_FontInfoRec postscript_info;

    // we're only interested in outlines
    if (!(face->face_flags & FT_FACE_FLAG_SCALABLE))
        return false;

    for (i = 0; i < num_names; i++) {
        FT_SfntName name;

        if (FT_Get_Sfnt_Name(face, i, &name))
            continue;

        if (name.platform_id == TT_PLATFORM_MICROSOFT &&
                (name.name_id == TT_NAME_ID_FULL_NAME ||
                 name.name_id == TT_NAME_ID_FONT_FAMILY) &&
                (name.encoding_id == TT_MS_ID_SYMBOL_CS ||
                 name.encoding_id == TT_MS_ID_UNICODE_CS ||
                 name.encoding_id == TT_MS_ID_UCS_4)) {
            char buf[1024];
            ass_utf16be_to_utf8(buf, sizeof(buf), (uint8_t *)name.string,
                                name.string_len);

            ass_msg(library, MSGL_INFO,
                "get_font_info: platform_id=Microsoft, name_id=%d, encoding_id=%d, language_id=0x%04X, name=[%s]",
                name.name_id, name.encoding_id, name.language_id, buf);

            if (name.name_id == TT_NAME_ID_FULL_NAME && num_fullname < MAX_FULLNAME) {
                fullnames[num_fullname] = strdup(buf);
                if (fullnames[num_fullname] == NULL)
                    goto error;
                num_fullname++;
            }

            if (name.name_id == TT_NAME_ID_FONT_FAMILY && num_family < MAX_FULLNAME) {
                families[num_family] = strdup(buf);
                if (families[num_family] == NULL)
                    goto error;
                num_family++;
            }
        } else if (name.name_id == TT_NAME_ID_FONT_FAMILY ||
                name.name_id == TT_NAME_ID_FONT_SUBFAMILY ||
                name.name_id == TT_NAME_ID_FULL_NAME ||
                name.name_id == TT_NAME_ID_PS_NAME ||
                name.name_id == TT_NAME_ID_PREFERRED_FAMILY ||
                name.name_id == TT_NAME_ID_PREFERRED_SUBFAMILY ||
                name.name_id == TT_NAME_ID_MAC_FULL_NAME ||
                name.name_id == TT_NAME_ID_WWS_FAMILY ||
                name.name_id == TT_NAME_ID_WWS_SUBFAMILY) {
            char buf[1024] = "(conversion failed)";
            if (name.platform_id == TT_PLATFORM_APPLE_UNICODE ||
                    name.platform_id == TT_PLATFORM_ISO ||
                    (name.platform_id == TT_PLATFORM_MICROSOFT &&
                     name.encoding_id != TT_MS_ID_GB2312 &&
                     name.encoding_id != TT_MS_ID_BIG_5 &&
                     name.encoding_id != TT_MS_ID_WANSUNG)) {
                ass_utf16be_to_utf8(buf, sizeof(buf), (uint8_t *)name.string,
                                    name.string_len);
            } else {
                struct name_encoding encoding_buffer = NAME_ENCODING_INIT, *encoding = &encoding_buffer;

                if (name.platform_id == TT_PLATFORM_MACINTOSH) {
                    identify_mac_encoding(encoding, name.encoding_id, name.language_id);
                } else if (name.platform_id == TT_PLATFORM_MICROSOFT) {
                    switch (name.encoding_id) {
                        case TT_MS_ID_GB2312:
                            CFENCODING(kCFStringEncodingDOSChineseSimplif);
                            WIN32_CODE_PAGE(936);
                            ICONV_FROMCODE("CP936");
                            break;
                        case TT_MS_ID_BIG_5:
                            CFENCODING(kCFStringEncodingDOSChineseTrad);
                            WIN32_CODE_PAGE(950);
                            ICONV_FROMCODE("CP950");
                            break;
                        case TT_MS_ID_WANSUNG:
                            CFENCODING(kCFStringEncodingDOSKorean);
                            WIN32_CODE_PAGE(949);
                            ICONV_FROMCODE("CP949");
                            break;
                    }
                    size_t r = 0, w = 0;
                    for (; r < name.string_len - 1; r += 2) {
                        if (name.string[r])
                            name.string[w++] = name.string[r];
                        name.string[w++] = name.string[r + 1];
                    }
                    // If string_len is odd, Windows either rejects the font
                    // or reads one byte past the end. In the latter case,
                    // the last output character is unpredictable (?),
                    // so chop it off; but if the last input byte is ASCII,
                    // we know it'll survive the transcoding unscathed
                    // as the penultimate output character, so include it.
                    if (r == name.string_len - 1 &&
                        name.string[r] && !(name.string[r] & 0x80))
                        name.string[w++] = name.string[r++];
                    name.string_len = w;
                }

                decode_name(encoding, name.string, name.string_len, buf, sizeof buf);
            }
            const char *platform_name = NULL;
            const char *format =
                "get_font_info: platform_id=%s, name_id=%d, encoding_id=%d, language_id=%d, name=[%s]";
            switch (name.platform_id) {
                case TT_PLATFORM_MICROSOFT:
                    platform_name = "Microsoft";
                    format = "get_font_info: platform_id=%s, name_id=%d, encoding_id=%d, language_id=0x%04X, name=[%s]";
                    break;
                case TT_PLATFORM_APPLE_UNICODE:
                    platform_name = "Unicode";
                    break;
                case TT_PLATFORM_MACINTOSH:
                    platform_name = "Macintosh";
                    break;
            }
            if (platform_name)
                ass_msg(library, MSGL_INFO, format,
                    platform_name, name.name_id, name.encoding_id, name.language_id, buf);
            else
                ass_msg(library, MSGL_INFO,
                    "get_font_info: platform_id=%d, name_id=%d, encoding_id=%d, language_id=%d, name=[%s]",
                    name.platform_id, name.name_id, name.encoding_id, name.language_id, buf);
        }

    }

    ass_msg(library, MSGL_INFO,
        "get_font_info: FreeType's family_name=[%s]",
        face->family_name ? face->family_name : "(null)");

    TT_OS2 *os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    if (os2) {
        const char *const fsSelection_names[] = {
            "ITALIC",
            "UNDERSCORE",
            "NEGATIVE",
            "OUTLINED",
            "STRIKEOUT",
            "BOLD",
            "REGULAR",
            "USE_TYPO_METRICS",
            "WWS",
            "OBLIQUE",
            NULL,
        };
        char fsSelection_explained[256];
        stringify_flag_enum(fsSelection_explained, os2->fsSelection, fsSelection_names);
        ass_msg(library, MSGL_INFO,
            "get_font_info: OS/2 table version %d, fsSelection 0x%X%s, usWeightClass %d",
            os2->version, os2->fsSelection, fsSelection_explained, os2->usWeightClass);
    } else {
        ass_msg(library, MSGL_INFO,
            "get_font_info: no OS/2 table");
    }

    for (FT_Int i = 0; i < face->num_charmaps; i++) {
        FT_CharMap cmap = face->charmaps[i];
        if (cmap->platform_id == TT_PLATFORM_MACINTOSH) {
            ass_msg(library, MSGL_INFO,
                "get_font_info: cmap for platform_id=Macintosh, encoding_id=%d, language_id+1=%lu",
                cmap->encoding_id, FT_Get_CMap_Language_ID(cmap));
        } else {
            const char *platform_name = NULL;
            switch (cmap->platform_id) {
                case TT_PLATFORM_MICROSOFT:
                    platform_name = "Microsoft";
                    break;
                case TT_PLATFORM_APPLE_UNICODE:
                    platform_name = "Unicode";
                    break;
            }
            if (platform_name)
                ass_msg(library, MSGL_INFO,
                    "get_font_info: cmap for platform_id=%s, encoding_id=%d",
                    platform_name, cmap->encoding_id);
            else
                ass_msg(library, MSGL_INFO,
                    "get_font_info: cmap for platform_id=%d, encoding_id=%d",
                    cmap->platform_id, cmap->encoding_id);
        }
    }

    // check if we got a valid family - if not, use
    // whatever the font provider or FreeType gives us
    if (num_family == 0 && (fallback_family_name || face->family_name)) {
        families[0] =
            strdup(fallback_family_name ? fallback_family_name : face->family_name);
        if (families[0] == NULL)
            goto error;
        num_family++;
    }

    // we absolutely need a name
    if (num_family == 0)
        goto error;

    // calculate sensible slant and weight from style attributes
    {
        const char *const style_flag_names[] = {
            "ITALIC",
            "BOLD",
            NULL,
        };
        char style_flags_explained[256];
        stringify_flag_enum(style_flags_explained, face->style_flags, style_flag_names);
        ass_msg(library, MSGL_INFO,
            "get_font_info: style_flags 0x%lX%s", face->style_flags, style_flags_explained);
    }
    slant  = FONT_SLANT_ITALIC * !!(face->style_flags & FT_STYLE_FLAG_ITALIC);
    weight = ass_face_get_weight(face);

    TT_Header *head = FT_Get_Sfnt_Table(face, FT_SFNT_HEAD);
    if (head) {
        const char *const style_names[] = {
            "bold",
            "italic",
            "underline",
            "outline",
            "shadow",
            "condensed",
            "extended",
            NULL,
        };
        char style_explained[256];
        stringify_flag_enum(style_explained, head->Mac_Style, style_names);
        ass_msg(library, MSGL_INFO,
            "get_font_info: head table version %g, flags 0x%X, Mac style 0x%X%s",
            d16_to_double(head->Table_Version),
            head->Flags, head->Mac_Style, style_explained);
    } else {
        ass_msg(library, MSGL_INFO,
            "get_font_info: no head table");
    }

    TT_HoriHeader *hhea = FT_Get_Sfnt_Table(face, FT_SFNT_HHEA);
    if (hhea) {
        ass_msg(library, MSGL_INFO,
            "get_font_info: hhea table version %g, caret slope rise/run %d/%d = %g",
            d16_to_double(hhea->Version),
            hhea->caret_Slope_Rise, hhea->caret_Slope_Run,
            (double) hhea->caret_Slope_Rise / hhea->caret_Slope_Run);
    } else {
        ass_msg(library, MSGL_INFO,
            "get_font_info: no hhea table");
    }

    TT_Postscript *post = FT_Get_Sfnt_Table(face, FT_SFNT_POST);
    if (head) {
        ass_msg(library, MSGL_INFO,
            "get_font_info: post table version %g, italicAngle %g",
            d16_to_double(post->FormatType),
            d16_to_double(post->italicAngle));
    } else {
        ass_msg(library, MSGL_INFO,
            "get_font_info: no post table");
    }

    TT_PCLT *pclt = FT_Get_Sfnt_Table(face, FT_SFNT_PCLT);
    if (pclt) {
        ass_msg(library, MSGL_INFO,
            "get_font_info: PCLT table version %g, style 0x%X (posture %d)",
            d16_to_double(pclt->Version),
            pclt->Style, pclt->Style & 3);
    } else {
        ass_msg(library, MSGL_INFO,
            "get_font_info: no PCLT table");
    }

    FT_ULong tag = FT_MAKE_TAG('f', 'o', 'n', 'd');
    FT_ULong length = 0;
    if (!FT_Load_Sfnt_Table(face, tag, 0, NULL, &length)) {
        unsigned char *buffer = malloc(length);
        if (buffer && !FT_Load_Sfnt_Table(face, tag, 0, buffer, &length)) {
            uint16_t fVersion = (uint16_t) buffer[0] << 8 | buffer[1];
            uint16_t fCount_fond = (uint16_t) buffer[2] << 8 | buffer[3];
            uint16_t fCount_nfnt = (uint16_t) buffer[4] << 8 | buffer[5];
            ass_msg(library, MSGL_INFO,
                "get_font_info: fond table version %d, %d FONDs, %d NFNTs",
                fVersion, fCount_fond, fCount_nfnt);

            if (fVersion == 2) {
                unsigned char *p = buffer + 8;
                for (uint16_t i = 0; i < fCount_fond; i++) {
                    int16_t fScript = p[8] << 8 | p[9];
                    int16_t fLanguage = p[10] << 8 | p[11];

                    struct name_encoding encoding_buffer = NAME_ENCODING_INIT;
                    identify_mac_encoding(&encoding_buffer, fScript, fLanguage);

                    char buf[1024] = "(conversion failed)";
                    decode_name(&encoding_buffer, p + 21, p[20], buf, sizeof buf);

                    ass_msg(library, MSGL_INFO,
                        "get_font_info: FOND [%s], style 0x%X",
                        buf,
                        p[6] << 8 | p[7]);

                    uint32_t fOffset = (uint32_t) p[12] << 24 | (uint32_t) p[13] << 16 | (uint32_t) p[14] << 8 | p[15];
                    unsigned char *q = buffer + fOffset;
                    uint32_t fNumMappings = (uint32_t) q[0] << 24 | (uint32_t) q[1] << 16 | (uint32_t) q[2] << 8 | q[3];
                    q += 4;
                    for (uint32_t j = 0; j < fNumMappings; j++) {
                        int32_t fResourceID = (int32_t) q[0] << 24 | (int32_t) q[1] << 16 | (int32_t) q[2] << 8 | q[3];
                        ass_msg(library, MSGL_INFO,
                            "get_font_info: \\-> references font [%.*s] as resource %" PRId32,
                            62, q + 4, fResourceID);
                        q += 4 + 62;
                    }

                    p += 20 + 256;
                }

                for (uint16_t i = 0; i < fCount_nfnt; i++) {
                    int16_t fScript = p[6] << 8 | p[7];
                    int16_t fLanguage = p[8] << 8 | p[9];

                    struct name_encoding encoding_buffer = NAME_ENCODING_INIT;
                    identify_mac_encoding(&encoding_buffer, fScript, fLanguage);

                    char buf[1024] = "(conversion failed)";
                    decode_name(&encoding_buffer, p + 19, p[18], buf, sizeof buf);

                    ass_msg(library, MSGL_INFO,
                        "get_font_info: NFNT [%s], style 0x%X",
                        buf,
                        p[4] << 8 | p[5]);
                    p += 18 + 256;
                }
            }
        } else {
            ass_msg(library, MSGL_INFO,
                "get_font_info: failed to load fond table");
        }
        free(buffer);
    } else {
        ass_msg(library, MSGL_INFO,
            "get_font_info: no fond table");
    }

    // fill our struct
    info->slant  = slant;
    info->weight = weight;
    info->width  = 100;     // FIXME, should probably query the OS/2 table

    info->postscript_name = (char *)FT_Get_Postscript_Name(face);
    info->is_postscript = !FT_Get_PS_Font_Info(face, &postscript_info);

    if (num_family) {
        info->families = calloc(sizeof(char *), num_family);
        if (info->families == NULL)
            goto error;
        memcpy(info->families, &families, sizeof(char *) * num_family);
        info->n_family = num_family;
    }

    if (num_fullname) {
        info->fullnames = calloc(sizeof(char *), num_fullname);
        if (info->fullnames == NULL)
            goto error;
        memcpy(info->fullnames, &fullnames, sizeof(char *) * num_fullname);
        info->n_fullname = num_fullname;
    }

    return true;

error:
    for (i = 0; i < num_family; i++)
        free(families[i]);

    for (i = 0; i < num_fullname; i++)
        free(fullnames[i]);

    free(info->families);
    free(info->fullnames);

    info->families = info->fullnames = NULL;
    info->n_family = info->n_fullname = 0;

    return false;
}

/**
 * \brief Free the dynamically allocated fields of metadata
 * created by get_font_info.
 * \param meta metadata created by get_font_info
 */
static void free_font_info(ASS_FontProviderMetaData *meta)
{
    if (meta->families) {
        for (int i = 0; i < meta->n_family; i++)
            free(meta->families[i]);
        free(meta->families);
    }

    if (meta->fullnames) {
        for (int i = 0; i < meta->n_fullname; i++)
            free(meta->fullnames[i]);
        free(meta->fullnames);
    }
}

/**
 * \brief Add a font to a font provider.
 * \param provider the font provider
 * \param meta basic metadata of the font
 * \param path path to the font file, or NULL
 * \param index face index inside the file (-1 to look up by PostScript name)
 * \param data private data for the font
 * \return success
 */
bool
ass_font_provider_add_font(ASS_FontProvider *provider,
                           ASS_FontProviderMetaData *meta, const char *path,
                           int index, void *data)
{
    int i;
    int weight, slant, width;
    ASS_FontSelector *selector = provider->parent;
    ASS_FontInfo *info = NULL;
    ASS_FontProviderMetaData implicit_meta = {0};

    if (!meta->n_family) {
        FT_Face face;
        if (provider->funcs.get_font_index)
            index = provider->funcs.get_font_index(data);
        if (!path) {
            ASS_FontStream stream = {
                .func = provider->funcs.get_data,
                .priv = data,
            };
            // This name is only used in an error message, so use
            // our best name but don't panic if we don't have any.
            // Prefer PostScript name because it is unique.
            const char *name = meta->postscript_name ?
                meta->postscript_name : meta->extended_family;
            face = ass_face_stream(selector->library, selector->ftlibrary,
                                   name, &stream, index);
        } else {
            face = ass_face_open(selector->library, selector->ftlibrary,
                                 path, meta->postscript_name, index);
        }
        if (!face)
            goto error;
        if (!get_font_info(selector->library, selector->ftlibrary,
                           face, meta->extended_family, &implicit_meta)) {
            FT_Done_Face(face);
            goto error;
        }
        if (implicit_meta.postscript_name) {
            implicit_meta.postscript_name =
                strdup(implicit_meta.postscript_name);
            if (!implicit_meta.postscript_name) {
                FT_Done_Face(face);
                goto error;
            }
        }
        FT_Done_Face(face);
        implicit_meta.extended_family = meta->extended_family;
        meta = &implicit_meta;
    }

#if 1
    int j;
    ASS_Library *lib = provider->parent->library;
    ass_msg(lib, MSGL_INFO, "new font:");
    for (j = 0; j < meta->n_family; j++)
        ass_msg(lib, MSGL_INFO, "  family: '%s'", meta->families[j]);
    for (j = 0; j < meta->n_fullname; j++)
        ass_msg(lib, MSGL_INFO, "  fullname: '%s'", meta->fullnames[j]);
    ass_msg(lib, MSGL_INFO, "  slant: %d", meta->slant);
    ass_msg(lib, MSGL_INFO, "  weight: %d", meta->weight);
    ass_msg(lib, MSGL_INFO, "  width: %d", meta->width);
    ass_msg(lib, MSGL_INFO, "  postscript_name: %s", meta->postscript_name);
    ass_msg(lib, MSGL_INFO, "  is_postscript: %d", provider->funcs.check_postscript ? provider->funcs.check_postscript(data) : meta->is_postscript);
#endif

    weight = meta->weight;
    slant  = meta->slant;
    width  = meta->width;

    // check slant/weight for validity, use defaults if they're invalid
    if (weight < 100 || weight > 900)
        weight = 400;
    if (slant < 0 || slant > 110)
        slant = 0;
    if (width < 50 || width > 200)
        width = 100;

    // check size
    if (selector->n_font >= selector->alloc_font) {
        selector->alloc_font = FFMAX(1, 2 * selector->alloc_font);
        selector->font_infos = realloc(selector->font_infos,
                selector->alloc_font * sizeof(ASS_FontInfo));
    }

    // copy over metadata
    info = selector->font_infos + selector->n_font;
    memset(info, 0, sizeof(ASS_FontInfo));

    // set uid
    info->uid = selector->uid++;

    info->slant         = slant;
    info->weight        = weight;
    info->width         = width;
    info->n_fullname    = meta->n_fullname;
    info->n_family      = meta->n_family;
    info->is_postscript = meta->is_postscript;

    info->families = calloc(meta->n_family, sizeof(char *));
    if (info->families == NULL)
        goto error;

    if (meta->n_fullname) {
        info->fullnames = calloc(meta->n_fullname, sizeof(char *));
        if (info->fullnames == NULL)
            goto error;
    }

    for (i = 0; i < info->n_family; i++) {
        info->families[i] = strdup(meta->families[i]);
        if (info->families[i] == NULL)
            goto error;
    }

    for (i = 0; i < info->n_fullname; i++) {
        info->fullnames[i] = strdup(meta->fullnames[i]);
        if (info->fullnames[i] == NULL)
            goto error;
    }

    if (meta->postscript_name) {
        info->postscript_name = strdup(meta->postscript_name);
        if (info->postscript_name == NULL)
            goto error;
    }

    if (meta->extended_family) {
        info->extended_family = strdup(meta->extended_family);
        if (info->extended_family == NULL)
            goto error;
    }

    if (path) {
        info->path = strdup(path);
        if (info->path == NULL)
            goto error;
    }

    info->index = index;
    info->priv  = data;
    info->provider = provider;

    selector->n_font++;

    free_font_info(&implicit_meta);
    free(implicit_meta.postscript_name);

    return true;

error:
    if (info)
        ass_font_provider_free_fontinfo(info);

    free_font_info(&implicit_meta);
    free(implicit_meta.postscript_name);

    if (provider->funcs.destroy_font)
        provider->funcs.destroy_font(data);

    return false;
}

/**
 * \brief Clean up font database. Deletes all fonts that have an invalid
 * font provider (NULL).
 * \param selector the font selector
 */
static void ass_fontselect_cleanup(ASS_FontSelector *selector)
{
    int i, w;

    for (i = 0, w = 0; i < selector->n_font; i++) {
        ASS_FontInfo *info = selector->font_infos + i;

        // update write pointer
        if (info->provider != NULL) {
            // rewrite, if needed
            if (w != i)
                memcpy(selector->font_infos + w, selector->font_infos + i,
                        sizeof(ASS_FontInfo));
            w++;
        }

    }

    selector->n_font = w;
}

void ass_font_provider_free(ASS_FontProvider *provider)
{
    int i;
    ASS_FontSelector *selector = provider->parent;

    // free all fonts and mark their entries
    for (i = 0; i < selector->n_font; i++) {
        ASS_FontInfo *info = selector->font_infos + i;

        if (info->provider == provider) {
            ass_font_provider_free_fontinfo(info);

            if (info->provider->funcs.destroy_font)
                info->provider->funcs.destroy_font(info->priv);

            info->provider = NULL;
        }

    }

    // delete marked entries
    ass_fontselect_cleanup(selector);

    // free private data of the provider
    if (provider->funcs.destroy_provider)
        provider->funcs.destroy_provider(provider->priv);

    free(provider);
}

static bool check_postscript(ASS_FontInfo *fi)
{
    ASS_FontProvider *provider = fi->provider;
    assert(provider);

    if (provider->funcs.check_postscript)
        return provider->funcs.check_postscript(fi->priv);
    else
        return fi->is_postscript;
}

/**
 * \brief Return whether the given font is in the given family.
 */
static bool matches_family_name(ASS_FontInfo *f, const char *family,
                                bool match_extended_family)
{
    for (int i = 0; i < f->n_family; i++) {
        if (ass_strcasecmp(f->families[i], family) == 0)
            return true;
    }
    if (match_extended_family && f->extended_family) {
        if (ass_strcasecmp(f->extended_family, family) == 0)
            return true;
    }
    return false;
}

/**
 * \brief Return whether the given font has the given fullname or
 * PostScript name depending on whether it has PostScript outlines.
 */
static bool matches_full_or_postscript_name(ASS_FontInfo *f,
                                            const char *fullname)
{
    bool matches_fullname = false;
    bool matches_postscript_name = false;

    for (int i = 0; i < f->n_fullname; i++) {
        if (ass_strcasecmp(f->fullnames[i], fullname) == 0) {
            matches_fullname = true;
            break;
        }
    }

    if (f->postscript_name != NULL &&
        ass_strcasecmp(f->postscript_name, fullname) == 0)
        matches_postscript_name = true;

    if (matches_fullname == matches_postscript_name)
        return matches_fullname;

    if (check_postscript(f))
        return matches_postscript_name;
    else
        return matches_fullname;
}

/**
 * \brief Compare attributes of font (a) against a font request (req). Returns
 * a matching score - the lower the better.
 * Ignores font names/families!
 * \param a font
 * \param b font request
 * \return matching score
 */
static unsigned font_attributes_similarity(ASS_FontInfo *a, ASS_FontInfo *req)
{
    unsigned similarity = 0;
    similarity += ABS(a->weight - req->weight);
    similarity += ABS(a->slant - req->slant);
    similarity += ABS(a->width - req->width);

    return similarity;
}

#if 0
// dump font information
static void font_info_dump(ASS_FontInfo *font_infos, size_t len)
{
    int i, j;

    // dump font infos
    for (i = 0; i < len; i++) {
        printf("font %d\n", i);
        printf("  families: ");
        for (j = 0; j < font_infos[i].n_family; j++)
            printf("'%s' ", font_infos[i].families[j]);
        printf("  fullnames: ");
        for (j = 0; j < font_infos[i].n_fullname; j++)
            printf("'%s' ", font_infos[i].fullnames[j]);
        printf("\n");
        printf("  slant: %d\n", font_infos[i].slant);
        printf("  weight: %d\n", font_infos[i].weight);
        printf("  width: %d\n", font_infos[i].width);
        printf("  path: %s\n", font_infos[i].path);
        printf("  index: %d\n", font_infos[i].index);
        printf("  score: %d\n", font_infos[i].score);

    }
}
#endif

static bool check_glyph(ASS_FontInfo *fi, uint32_t code)
{
    ASS_FontProvider *provider = fi->provider;
    assert(provider && provider->funcs.check_glyph);

    return provider->funcs.check_glyph(fi->priv, code);
}

static char *
find_font(ASS_FontSelector *priv,
          ASS_FontProviderMetaData meta, bool match_extended_family,
          unsigned bold, unsigned italic,
          int *index, char **postscript_name, int *uid, ASS_FontStream *stream,
          uint32_t code, bool *name_match)
{
    ASS_FontInfo req = {0};
    ASS_FontInfo *selected = NULL;

    // do we actually have any fonts?
    if (!priv->n_font)
        return NULL;

    // fill font request
    req.slant   = italic;
    req.weight  = bold;
    req.width   = 100;

    // Match font family name against font list
    unsigned score_min = UINT_MAX;
    for (int i = 0; i < meta.n_fullname; i++) {
        const char *fullname = meta.fullnames[i];

        for (int x = 0; x < priv->n_font; x++) {
            ASS_FontInfo *font = &priv->font_infos[x];
            unsigned score = UINT_MAX;

            if (matches_family_name(font, fullname, match_extended_family)) {
                // If there's a family match, compare font attributes
                // to determine best match in that particular family
                score = font_attributes_similarity(font, &req);
                *name_match = true;
            } else if (matches_full_or_postscript_name(font, fullname)) {
                // If we don't have any match, compare fullnames against request
                // if there is a match now, assign lowest score possible. This means
                // the font should be chosen instantly, without further search.
                score = 0;
                *name_match = true;
            }

            // Consider updating idx if score is better than current minimum
            if (score < score_min) {
                // Check if the font has the requested glyph.
                // We are doing this here, for every font face, because
                // coverage might differ between the variants of a font
                // family. In practice, it is common that the regular
                // style has the best coverage while bold/italic/etc
                // variants cover less (e.g. FreeSans family).
                // We want to be able to match even if the closest variant
                // does not have the requested glyph, but another member
                // of the family has the glyph.
                if (!check_glyph(font, code))
                    continue;

                score_min = score;
                selected = font;
            }

            // Lowest possible score instantly matches; this is typical
            // for fullname matches, but can also occur with family matches.
            if (score == 0)
                break;
        }

        // The list of names is sorted by priority. If we matched anything,
        // we can and should stop.
        if (selected != NULL)
            break;
    }

    // found anything?
    char *result = NULL;
    if (selected) {
        ASS_FontProvider *provider = selected->provider;

        // successfully matched, set up return values
        *postscript_name = selected->postscript_name;
        *uid   = selected->uid;

        // use lazy evaluation for index if applicable
        if (provider->funcs.get_font_index) {
            *index = provider->funcs.get_font_index(selected->priv);
        } else
            *index = selected->index;

        // set up memory stream if there is no path
        if (selected->path == NULL) {
            stream->func = provider->funcs.get_data;
            stream->priv = selected->priv;
            // Prefer PostScript name because it is unique. This is only
            // used for display purposes so it doesn't matter that much,
            // though.
            if (selected->postscript_name)
                result = selected->postscript_name;
            else
                result = selected->families[0];
        } else
            result = selected->path;

    }

    return result;
}

static char *select_font(ASS_FontSelector *priv,
                         const char *family, bool match_extended_family,
                         unsigned bold, unsigned italic,
                         int *index, char **postscript_name, int *uid,
                         ASS_FontStream *stream, uint32_t code)
{
    ASS_Library *library = priv->library;
    ASS_FontProvider *default_provider = priv->default_provider;
    ASS_FontProviderMetaData meta = {0};
    char *result = NULL;
    bool name_match = false;

    ass_msg(library, MSGL_INFO,
            "select_font(code=U+%04X, "
            "family=[%s], bold=%u, italic=%u)",
            code, family ? family : "(null)", bold, italic);

    if (family == NULL)
        return NULL;

    ASS_FontProviderMetaData default_meta = {
        .n_fullname = 1,
        .fullnames  = (char **)&family,
    };

    // Get a list of substitutes if applicable, and use it for matching.
    if (default_provider && default_provider->funcs.get_substitutions) {
        default_provider->funcs.get_substitutions(default_provider->priv,
                                                  family, &meta);
    }

    if (!meta.n_fullname) {
        free(meta.fullnames);
        meta = default_meta;
    }

    ass_msg(library, MSGL_INFO,
            "n_fullname=%d, fullnames:",
            meta.n_fullname);
    for (int i = 0; i < meta.n_fullname; ++i)
        ass_msg(library, MSGL_INFO,
                "    %s",
                meta.fullnames[i]);

    result = find_font(priv, meta, match_extended_family,
                       bold, italic, index, postscript_name, uid,
                       stream, code, &name_match);
    ass_msg(library, MSGL_INFO,
        "find_font returned path [%s], index %d, PostScript name [%s]",
        result ? result : "(null)",
        *index,
        *postscript_name ? *postscript_name : "(null)");

    // If no matching font was found, it might not exist in the font list
    // yet. Call the match_fonts callback to fill in the missing fonts
    // on demand, and retry the search for a match.
    if (result == NULL && name_match == false && default_provider &&
            default_provider->funcs.match_fonts) {
        ass_msg(library, MSGL_INFO, "calling match_fonts");
        // TODO: consider changing the API to make more efficient
        // implementations possible.
        for (int i = 0; i < meta.n_fullname; i++) {
            default_provider->funcs.match_fonts(default_provider->priv,
                                                priv->library, default_provider,
                                                meta.fullnames[i]);
        }
        result = find_font(priv, meta, match_extended_family,
                           bold, italic, index, postscript_name, uid,
                           stream, code, &name_match);
        ass_msg(library, MSGL_INFO,
            "post-match find_font returned path [%s], index %d, PostScript name [%s]",
            result ? result : "(null)",
            *index,
            *postscript_name ? *postscript_name : "(null)");
    }

    // cleanup
    if (meta.fullnames != default_meta.fullnames) {
        for (int i = 0; i < meta.n_fullname; i++)
            free(meta.fullnames[i]);
        free(meta.fullnames);
    }

    return result;
}


/**
 * \brief Find a font. Use default family or path if necessary.
 * \param family font family
 * \param treat_family_as_pattern treat family as fontconfig pattern
 * \param bold font weight value
 * \param italic font slant value
 * \param index out: font index inside a file
 * \param code: the character that should be present in the font, can be 0
 * \return font file path
*/
char *ass_font_select(ASS_FontSelector *priv,
                      const ASS_Font *font, int *index, char **postscript_name,
                      int *uid, ASS_FontStream *data, uint32_t code)
{
    char *res = 0;
    const char *family = font->desc.family.str;  // always zero-terminated
    unsigned bold = font->desc.bold;
    unsigned italic = font->desc.italic;
    ASS_Library *library = priv->library;
    ASS_FontProvider *default_provider = priv->default_provider;

    ass_msg(library, MSGL_INFO,
            "ass_font_select(code=U+%04X): "
            "family=[%s], bold=%u, italic=%u",
            code, family ? family : "(null)", bold, italic);

    if (family && *family) {
        res = select_font(priv, family, false, bold, italic, index,
                postscript_name, uid, data, code);
        ass_msg(library, MSGL_INFO, "first select_font returned %s", res ? res : "(null)");
    }

    if (!res && priv->family_default) {
        res = select_font(priv, priv->family_default, false, bold,
                italic, index, postscript_name, uid, data, code);
        if (res)
            ass_msg(priv->library, MSGL_WARN, "fontselect: Using default "
                    "font family: (%s, %d, %d) -> %s, %d, %s",
                    family, bold, italic, res, *index,
                    *postscript_name ? *postscript_name : "(none)");
    }

    if (!res && default_provider && default_provider->funcs.get_fallback) {
        const char *search_family = family;
        if (!search_family || !*search_family)
            search_family = "Arial";
        char *fallback_family = default_provider->funcs.get_fallback(
                default_provider->priv, priv->library, search_family, code);
        ass_msg(library, MSGL_INFO, "get_fallback returned %s", fallback_family ? fallback_family : "(null)");

        if (fallback_family) {
            res = select_font(priv, fallback_family, true, bold, italic,
                    index, postscript_name, uid, data, code);
            ass_msg(library, MSGL_INFO, "select_font returned %s", res ? res : "(null)");
            free(fallback_family);
        }
    }

    if (!res && priv->path_default) {
        res = priv->path_default;
        *index = priv->index_default;
        ass_msg(priv->library, MSGL_WARN, "fontselect: Using default font: "
                "(%s, %d, %d) -> %s, %d, %s", family, bold, italic,
                priv->path_default, *index,
                *postscript_name ? *postscript_name : "(none)");
    }

    if (res)
        ass_msg(priv->library, MSGL_INFO,
                "fontselect: (%s, %d, %d) -> %s, %d, %s", family, bold,
                italic, res, *index, *postscript_name ? *postscript_name : "(none)");
    else
        ass_msg(priv->library, MSGL_WARN,
                "fontselect: failed to find any fallback with glyph 0x%X for font: "
                "(%s, %d, %d)", code, family, bold, italic);

    return res;
}


/**
 * \brief Process memory font.
 * \param priv private data
 * \param idx index of the processed font in priv->library->fontdata
 *
 * Builds a FontInfo with FreeType and some table reading.
*/
static void process_fontdata(ASS_FontProvider *priv, int idx)
{
    ASS_FontSelector *selector = priv->parent;
    ASS_Library *library = selector->library;

    int rc;
    const char *name = library->fontdata[idx].name;
    const char *data = library->fontdata[idx].data;
    int data_size = library->fontdata[idx].size;

    FT_Face face;
    int face_index, num_faces = 1;

    for (face_index = 0; face_index < num_faces; ++face_index) {
        ASS_FontProviderMetaData info;
        FontDataFT *ft;

        rc = FT_New_Memory_Face(selector->ftlibrary, (unsigned char *) data,
                                data_size, face_index, &face);
        if (rc) {
            ass_msg(library, MSGL_WARN, "Error opening memory font '%s'",
                   name);
            continue;
        }

        num_faces = face->num_faces;

        ass_charmap_magic(library, face);

        memset(&info, 0, sizeof(ASS_FontProviderMetaData));
        if (!get_font_info(library, selector->ftlibrary, face, NULL, &info)) {
            ass_msg(library, MSGL_WARN,
                    "Error getting metadata for embedded font '%s'", name);
            FT_Done_Face(face);
            continue;
        }

        ft = calloc(1, sizeof(FontDataFT));

        if (ft == NULL) {
            free_font_info(&info);
            FT_Done_Face(face);
            continue;
        }

        ft->lib  = library;
        ft->face = face;
        ft->idx  = idx;

        if (!ass_font_provider_add_font(priv, &info, NULL, face_index, ft)) {
            ass_msg(library, MSGL_WARN, "Failed to add embedded font '%s'",
                    name);
            free(ft);
        }

        free_font_info(&info);
    }
}

/**
 * \brief Create font provider for embedded fonts. This parses the fonts known
 * to the current ASS_Library and adds them to the selector.
 * \param selector font selector
 * \return font provider
 */
static ASS_FontProvider *
ass_embedded_fonts_add_provider(ASS_FontSelector *selector, size_t *num_emfonts)
{
    ASS_FontProvider *priv = ass_font_provider_new(selector, &ft_funcs, NULL);
    if (priv == NULL)
        return NULL;

    ASS_Library *lib = selector->library;

    if (lib->fonts_dir && lib->fonts_dir[0]) {
        load_fonts_from_dir(lib, lib->fonts_dir);
    }

    for (size_t i = 0; i < lib->num_fontdata; i++)
        process_fontdata(priv, i);
    *num_emfonts = lib->num_fontdata;

    return priv;
}

struct font_constructors {
    ASS_DefaultFontProvider id;
    ASS_FontProvider *(*constructor)(ASS_Library *, ASS_FontSelector *,
                                     const char *, FT_Library);
    const char *name;
};

struct font_constructors font_constructors[] = {
#ifdef CONFIG_CORETEXT
    { ASS_FONTPROVIDER_CORETEXT,        &ass_coretext_add_provider,     "coretext"},
#endif
#ifdef CONFIG_DIRECTWRITE
    { ASS_FONTPROVIDER_DIRECTWRITE,     &ass_directwrite_add_provider,  "directwrite"
#if ASS_WINAPI_DESKTOP
        " (with GDI)"
#else
        " (without GDI)"
#endif
    },
#endif
#ifdef CONFIG_FONTCONFIG
    { ASS_FONTPROVIDER_FONTCONFIG,      &ass_fontconfig_add_provider,   "fontconfig"},
#endif
    { ASS_FONTPROVIDER_NONE, NULL, NULL },
};

/**
 * \brief Init font selector.
 * \param library libass library object
 * \param ftlibrary freetype library object
 * \param family default font family
 * \param path default font path
 * \return newly created font selector
 */
ASS_FontSelector *
ass_fontselect_init(ASS_Library *library, FT_Library ftlibrary, size_t *num_emfonts,
                    const char *family, const char *path, const char *config,
                    ASS_DefaultFontProvider dfp)
{
    ASS_FontSelector *priv = calloc(1, sizeof(ASS_FontSelector));
    if (priv == NULL)
        return NULL;

    priv->library = library;
    priv->ftlibrary = ftlibrary;
    priv->uid = 1;
    priv->family_default = family ? strdup(family) : NULL;
    priv->path_default = path ? strdup(path) : NULL;
    priv->index_default = 0;

    if (family && !priv->family_default)
        goto fail;
    if (path && !priv->path_default)
        goto fail;

    priv->embedded_provider = ass_embedded_fonts_add_provider(priv, num_emfonts);

    if (priv->embedded_provider == NULL) {
        ass_msg(library, MSGL_WARN, "failed to create embedded font provider");
        goto fail;
    }

    if (dfp >= ASS_FONTPROVIDER_AUTODETECT) {
        for (int i = 0; font_constructors[i].constructor; i++ )
            if (dfp == font_constructors[i].id ||
                dfp == ASS_FONTPROVIDER_AUTODETECT) {
                priv->default_provider =
                    font_constructors[i].constructor(library, priv,
                                                     config, ftlibrary);
                if (priv->default_provider) {
                    ass_msg(library, MSGL_INFO, "Using font provider %s",
                            font_constructors[i].name);
                    break;
                }
            }

        if (!priv->default_provider)
            ass_msg(library, MSGL_WARN, "can't find selected font provider");

    }

    return priv;

fail:
    if (priv->default_provider)
        ass_font_provider_free(priv->default_provider);
    if (priv->embedded_provider)
        ass_font_provider_free(priv->embedded_provider);

    free(priv->family_default);
    free(priv->path_default);

    free(priv);

    return NULL;
}

void ass_get_available_font_providers(ASS_Library *priv,
                                      ASS_DefaultFontProvider **providers,
                                      size_t *size)
{
    size_t offset = 2;

    *size = offset;
    for (int i = 0; font_constructors[i].constructor; i++)
        (*size)++;

    *providers = calloc(*size, sizeof(ASS_DefaultFontProvider));

    if (*providers == NULL) {
        *size = (size_t)-1;
        return;
    }

    (*providers)[0] = ASS_FONTPROVIDER_NONE;
    (*providers)[1] = ASS_FONTPROVIDER_AUTODETECT;

    for (int i = offset; i < *size; i++)
        (*providers)[i] = font_constructors[i-offset].id;
}

/**
 * \brief Free font selector and release associated data
 * \param the font selector
 */
void ass_fontselect_free(ASS_FontSelector *priv)
{
    if (priv->default_provider)
        ass_font_provider_free(priv->default_provider);
    if (priv->embedded_provider)
        ass_font_provider_free(priv->embedded_provider);

    free(priv->font_infos);
    free(priv->path_default);
    free(priv->family_default);

    free(priv);
}

void ass_map_font(const ASS_FontMapping *map, int len, const char *name,
                  ASS_FontProviderMetaData *meta)
{
    for (int i = 0; i < len; i++) {
        if (ass_strcasecmp(map[i].from, name) == 0) {
            meta->fullnames = calloc(1, sizeof(char *));
            if (meta->fullnames) {
                meta->fullnames[0] = strdup(map[i].to);
                if (meta->fullnames[0])
                    meta->n_fullname = 1;
            }
            return;
        }
    }
}

size_t ass_update_embedded_fonts(ASS_FontSelector *selector, size_t num_loaded)
{
    if (!selector->embedded_provider)
        return num_loaded;

    size_t num_fontdata = selector->library->num_fontdata;
    for (size_t i = num_loaded; i < num_fontdata; i++)
        process_fontdata(selector->embedded_provider, i);
    return num_fontdata;
}
