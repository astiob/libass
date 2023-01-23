#define CONCAT(a, b) a ## b
#define NAME(suffix) CONCAT(get_font_info_, suffix)
static bool NAME(FONT_TYPE)(FONT_TYPE *font,
#ifdef FAMILY_AS_ARG
                            IDWriteFontFamily *fontFamily,
#endif
                            ASS_FontProviderMetaData *meta)
#undef NAME
#undef CONCAT
{
    HRESULT hr;
    BOOL exists;

    meta->weight = font->lpVtbl->GetWeight(font);

    DWRITE_FONT_STYLE style = font->lpVtbl->GetStyle(font);
    meta->fsSelection = style == DWRITE_FONT_STYLE_NORMAL ? 0 : 1;

    const char* data;
    UINT32 size;
    void* context;
    hr = font->lpVtbl->TryGetFontTable(font, DWRITE_MAKE_OPENTYPE_TAG('O', 'S', '/', '2'),
                                       &data, &size, &context, &exists);
    if (!FAILED(hr)) {
        if (exists && size >= 64)
            meta->fsSelection = ntohs(*(uint16_t*)(data + 62));
        font->lpVtbl->ReleaseFontTable(font, context);
    }

    IDWriteLocalizedStrings *psNames;
    hr = font->lpVtbl->GetInformationalStrings(font,
            DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME, &psNames, &exists);
    if (FAILED(hr))
        return false;

    if (exists) {
        meta->postscript_name = get_utf8_name(psNames, 0);
        IDWriteLocalizedStrings_Release(psNames);
        if (!meta->postscript_name)
            return false;
    }

    IDWriteLocalizedStrings *fontNames;
    hr = font->lpVtbl->GetInformationalStrings(font,
            DWRITE_INFORMATIONAL_STRING_FULL_NAME, &fontNames, &exists);
    if (FAILED(hr))
        return false;

    if (exists) {
        meta->n_fullname = IDWriteLocalizedStrings_GetCount(fontNames);
        meta->fullnames = calloc(meta->n_fullname, sizeof(char *));
        if (!meta->fullnames) {
            IDWriteLocalizedStrings_Release(fontNames);
            return false;
        }
        for (int k = 0; k < meta->n_fullname; k++) {
            meta->fullnames[k] = get_utf8_name(fontNames, k);
            if (!meta->fullnames[k]) {
                IDWriteLocalizedStrings_Release(fontNames);
                return false;
            }
        }
        IDWriteLocalizedStrings_Release(fontNames);
    }

    IDWriteLocalizedStrings *familyNames;
    hr = font->lpVtbl->GetInformationalStrings(font,
            DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES, &familyNames, &exists);
    if (!FAILED(hr) && !exists) {
#ifdef FAMILY_AS_ARG
        hr = IDWriteFontFamily_GetFamilyNames(fontFamily, &familyNames);
#else
        hr = font->lpVtbl->GetFamilyNames(font, &familyNames);
#endif
    }
    if (FAILED(hr))
        return false;

    meta->n_family = IDWriteLocalizedStrings_GetCount(familyNames);
    meta->families = calloc(meta->n_family, sizeof(char *));
    if (!meta->families) {
        IDWriteLocalizedStrings_Release(familyNames);
        return false;
    }
    for (int k = 0; k < meta->n_family; k++) {
        meta->families[k] = get_utf8_name(familyNames, k);
        if (!meta->families[k]) {
            IDWriteLocalizedStrings_Release(familyNames);
            return false;
        }
    }
    IDWriteLocalizedStrings_Release(familyNames);

    return true;
}
