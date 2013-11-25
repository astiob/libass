#ifndef STRING_H_WIN32
#define STRING_H_WIN32

#if defined(_MSC_VER)
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strtoll _strtoi64
#endif

#endif