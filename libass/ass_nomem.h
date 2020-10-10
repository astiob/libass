/*
 * Copyright (C) 2020 libass contributors
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

#ifndef LIBASS_NOMEM_H
#define LIBASS_NOMEM_H

#include <stdbool.h>
#include "ass_types.h"

/**
 * Temporary solution for logging alloc failures
 * In the not-so-distant future this will be replaced by a proper and more
 * general error reporting mechanism accesible to API-users (FIXME).
 *
 * Until then, to not just silently degrade, but also not spam the log too
 * much if memory is short — which might get us OOM-killed even faster —
 * use this to report alloc-failures and unexpected NULLs.
 * Unless forced only at most one message will be printed before
 * ass_nomem_clar is called.
 */
void ass_nomem_log(ASS_Library *priv, bool force, const char *fmt, ...);

/**
 * Call to indicate new nomem messages may be logged.
 */
void ass_nomem_clear(ASS_Library *priv);

#endif /* LIBASS_NOMEM_H */
