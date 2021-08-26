/*
 * Copyright (C) 2006 Evgeniy Stepanov <eugeni.stepanov@gmail.com>
 * Copyright (C) 2009 Grigori Goronzy <greg@geekmind.org>
 * Copyright (C) 2013 rcombs <rcombs@rcombs.me>
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

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "ass.h"
#include "ass_types.h"
//#include "../libass/ass.h"
//#include "../libass/ass_types.h"

enum {
    // Everything is fine
    FUZZ_OK = 0,
    /*// Default failure code of sanitisers, can be changed via env (A|UB|..)SAN_OPTIONS=exitcode=21
    SANITISER_FAIL = 1, */
    // Invalid parameters passed etc
    FUZZ_BAD_USAGE = 2,
    // Error before rendering starts
    FUZZ_INIT_ERR = 3
} ret_codes;

ASS_Library *ass_library;
ASS_Renderer *ass_renderer;

void msg_callback(int level, const char *fmt, va_list va, void *data)
{
    if (level > 6) return;
    printf("libass: ");
    vprintf(fmt, va);
    printf("\n");
}

static bool init()
{
    ass_library = ass_library_init();
    if (!ass_library) {
        printf("ass_library_init failed!\n");
        return false;
    }

    ass_set_message_cb(ass_library, msg_callback, NULL);

    ass_renderer = ass_renderer_init(ass_library);
    if (!ass_renderer) {
        printf("ass_renderer_init failed!\n");
        return false;
    }

    ass_set_fonts(ass_renderer, NULL, "sans", 1, NULL, 1);

    return true;
}

int main(int argc, char *argv[])
{
    int n;
    ASS_Track *track = NULL;
    int retval = FUZZ_OK;

    if (argc != 2) {
        // Would options for font folder or System fonts make sense?
        printf("usage: %s <subtitle file>\n", argv[0]);
        return FUZZ_BAD_USAGE;
    }

    if (init())
        track = ass_read_file(ass_library, argv[1], NULL);

    if (!track) {
        printf("track init failed!\n");
        retval = FUZZ_INIT_ERR;
        goto cleanup;
    }

    if (track->n_events == 0) {
        printf("track has no events. exiting.\n");
        goto cleanup;
    }

    ass_set_frame_size(ass_renderer, 1280, 720);
    ass_set_storage_size(ass_renderer, 1280, 720);

    for (n = 0; n < track->n_events; ++n) {
        int change;
        ASS_Event event = track->events[n];
        ass_render_frame(ass_renderer, track, event.Start, &change);
        if (event.Duration > 0) {
            ass_render_frame(ass_renderer, track, event.Start + event.Duration/2, &change);
            ass_render_frame(ass_renderer, track, event.Start + event.Duration-1, &change);
        }
    }

cleanup:
    if (track)        ass_free_track(track);
    if (ass_renderer) ass_renderer_done(ass_renderer);
    if (ass_library)  ass_library_done(ass_library);

    return retval;
}
