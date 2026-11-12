/*
   Internal file viewer for the Midnight Commander
   Line filter: show only the lines containing a string

   Copyright (C) 2026
   Free Software Foundation, Inc.

   The idea of the filter comes from the viewer of M-Commander
   (https://github.com/blue-panels/mcommander) by Ilia Maslakov.

   This file is part of the Midnight Commander.

   The Midnight Commander is free software: you can redistribute it
   and/or modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation, either version 3 of the License,
   or (at your option) any later version.

   The Midnight Commander is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * The filter is a plain string (or its lack, like grep -v), matched like the fast path of the
 * search: byte by byte, with ASCII letters case insensitively unless the search is case sensitive.
 * The data is read in big chunks and searched with mcview_fast_search_find(), so a big log file is
 * filtered at the speed of the disk.
 *
 * The start offsets of the matching lines are collected in filter_lines. They are looked for only
 * as far as needed to show the screen (or to move to the end), so the first lines are shown
 * immediately, and data from a pipe is read only as far as it is needed, like without a filter.
 */

#include <config.h>

#include <string.h>

#include "lib/global.h"
#include "lib/charsets.h"  // str_convert_to_input()
#include "lib/search.h"
#include "lib/strutil.h"
#include "lib/util.h"  // size_trunc_len()
#include "lib/widget.h"

#include "src/history.h"
#include "src/setup.h"  // verbose, panels_options

#include "internal.h"

/*** global variables ****************************************************************************/

/*** file scope macro definitions ****************************************************************/

// size of the chunks read at once (a longer line makes the chunk bigger)
#define MCVIEW_FILTER_CHUNK (1024 * 1024)

/*** file scope type declarations ****************************************************************/

typedef struct
{
    simple_status_msg_t status_msg;  // base class

    WView *view;
    gint64 label_timestamp;  // when the amount was shown last
} mcview_filter_status_msg_t;

/*** forward declarations (file scope functions) *************************************************/

/*** file scope variables ************************************************************************/

// show the lines without the filter string, like grep -v
static gboolean mcview_filter_invert = FALSE;

/* --------------------------------------------------------------------------------------------- */
/*** file scope functions ************************************************************************/
/* --------------------------------------------------------------------------------------------- */

static int
mcview_filter_status_update_cb (status_msg_t *sm)
{
    simple_status_msg_t *ssm = SIMPLE_STATUS_MSG (sm);
    mcview_filter_status_msg_t *fsm = (mcview_filter_status_msg_t *) sm;
    WView *view = fsm->view;
    int percent;

    percent = mcview_calc_percent (view, view->filter_scanned);

    if (percent >= 0)
        label_set_textv (ssm->label, _ ("Filtering %s: %3d%%"), view->filter_text, percent);
    else if (mc_time_elapsed (&fsm->label_timestamp, G_USEC_PER_SEC / 5))
    {
        // the size is unknown (data from a pipe): show how much has been read
        char buffer[BUF_TINY];

        size_trunc_len (buffer, 5, view->filter_scanned, 0, panels_options.kilobyte_si);
        label_set_textv (ssm->label, _ ("Filtering %s: %s"), view->filter_text, buffer);
    }

    return status_msg_common_update (sm);
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Whether the matching lines have to be looked for further: there are less than @count of them
 * yet, or none at or after @offset (if @offset isn't negative).
 */

static gboolean
mcview_filter_need_more (const WView *view, guint count, off_t offset)
{
    const GArray *lines = view->filter_lines;

    if (view->filter_complete || view->filter_stopped)
        return FALSE;

    if (lines->len < count)
        return TRUE;

    return offset >= 0
        && (lines->len == 0 || g_array_index (lines, off_t, lines->len - 1) < offset);
}

/* --------------------------------------------------------------------------------------------- */
/** Add the lines in [@p, @end) of @buf (read from @start) to the matching lines: an inverted
 * filter. */

static void
mcview_filter_add_lines (WView *view, off_t start, const char *buf, const char *p, const char *end)
{
    // p is at the start of a line
    while (p < end)
    {
        const off_t line = start + (p - buf);
        const char *eol;

        g_array_append_val (view->filter_lines, line);

        eol = memchr (p, '\n', (size_t) (end - p));
        if (eol == NULL)
            break;
        p = eol + 1;
    }
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Look for the matching lines in the next chunk of the data: read whole lines (more if a line
 * is longer than the chunk), and find the filter string in them (the lines without it if the
 * filter is inverted).
 *
 * @return FALSE on a read error
 */

static gboolean
mcview_filter_scan_chunk (WView *view, char **buf, size_t *size)
{
    const off_t start = view->filter_scanned;
    const char *const needle = view->filter->str;
    const gsize needle_len = view->filter->len;
    size_t len = 0;  // bytes read
    size_t used;     // bytes of whole lines
    gboolean eof = FALSE;
    const char *p, *end;

    while (TRUE)
    {
        const size_t old_len = len;
        gssize got;

        got = mcview_fast_search_read (view, start + (off_t) len, *buf + len, *size - len);
        if (got < 0)
            return FALSE;

        len += (size_t) got;
        if (len < *size)
        {
            // the end of the data (the read of a pipe waits for the data)
            eof = TRUE;
            used = len;
            break;
        }

        for (used = len; used > old_len && (*buf)[used - 1] != '\n'; used--)
            ;
        if (used > old_len)
            break;

        // no newline yet: a long line, read more of it
        *size *= 2;
        *buf = g_realloc (*buf, *size);
    }

    p = *buf;
    end = *buf + used;

    while (p < end)
    {
        const char *hit, *bol, *eol;
        off_t line;

        hit = mcview_fast_search_find (p, (gsize) (end - p), needle, needle_len,
                                       view->filter_caseless);
        if (hit == NULL)
        {
            if (view->filter_invert)
                mcview_filter_add_lines (view, start, *buf, p, end);
            break;
        }

        // p is at the start of a line
        for (bol = hit; bol > p && bol[-1] != '\n'; bol--)
            ;
        if (view->filter_invert)
            mcview_filter_add_lines (view, start, *buf, p, bol);
        else
        {
            line = start + (bol - *buf);
            g_array_append_val (view->filter_lines, line);
        }

        eol = memchr (hit, '\n', (size_t) (end - hit));
        if (eol == NULL)
            break;
        p = eol + 1;
    }

    view->filter_scanned = start + (off_t) used;
    if (eof)
        view->filter_complete = TRUE;

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Look for the matching lines until there are at least @count of them, and one at or after
 * @offset (if it isn't negative), or until the end of the data. A dialog is shown if it takes
 * long, where it can be stopped.
 */

static void
mcview_filter_scan (WView *view, guint count, off_t offset)
{
    mcview_filter_status_msg_t fsm;
    size_t size = MCVIEW_FILTER_CHUNK;
    char *buf;

    // a redraw from the dialog mustn't start it again
    if (view->filter_scanning || !mcview_filter_need_more (view, count, offset))
        return;

    view->filter_scanning = TRUE;

    fsm.view = view;
    fsm.label_timestamp = 0;
    status_msg_init (STATUS_MSG (&fsm), _ ("Filter"), 1.0, simple_status_msg_init_cb,
                     mcview_filter_status_update_cb, NULL);

    buf = g_malloc (size);

    do
    {
        if (!mcview_filter_scan_chunk (view, &buf, &size))
        {
            // a read error: show what has been found
            view->filter_complete = TRUE;
            break;
        }

        if (!view->filter_complete && STATUS_MSG (&fsm)->update (STATUS_MSG (&fsm)) == B_CANCEL)
            view->filter_stopped = TRUE;
    }
    while (mcview_filter_need_more (view, count, offset));

    g_free (buf);

    status_msg_deinit (STATUS_MSG (&fsm));

    view->filter_scanning = FALSE;
}

/* --------------------------------------------------------------------------------------------- */

static void
mcview_filter_set (WView *view, const char *text, const char *needle, gboolean case_sens,
                   gboolean invert)
{
    mcview_filter_clear (view);

    view->filter = g_string_new (needle);
    view->filter_caseless = !case_sens;
    view->filter_invert = invert;
    if (view->filter_caseless)
        g_string_ascii_down (view->filter);
    view->filter_text = g_strdup (text);
    view->filter_lines = g_array_new (FALSE, FALSE, sizeof (off_t));
    view->filter_scanned = 0;
    view->filter_complete = FALSE;
    view->filter_stopped = FALSE;

    // the filtered lines are shown unwrapped
    view->filter_saved_wrap = view->mode_flags.wrap;
    view->mode_flags.wrap = FALSE;

    view->dpy_paragraph_skip_lines = 0;
    view->dpy_wrap_dirty = TRUE;
    view->dpy_bbar_dirty = TRUE;
    view->dirty++;
}

/* --------------------------------------------------------------------------------------------- */
/*** public functions ****************************************************************************/
/* --------------------------------------------------------------------------------------------- */

/** Whether the lines are filtered now (the hex view shows all the data). */

gboolean
mcview_filter_on (const WView *view)
{
    return view->filter != NULL && !view->mode_flags.hex;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Ask for the filter string. The input line shares the history of the search, so the last
 * search string is offered, and the case sensitivity is that of the search too.
 */

void
mcview_filter_dialog (WView *view)
{
    char *text = NULL;
    int qd_result;

    if (view->datasource != DS_FILE && !view->growbuf_in_use)
    {
        message (D_ERROR, _ ("Filter"), "%s", _ ("Only a file or command output can be filtered."));
        return;
    }

    {
        quick_widget_t quick_widgets[] = {
            // clang-format off
            QUICK_LABELED_INPUT (_ ("Show only the lines containing (empty: all lines):"),
                                 input_label_above, INPUT_LAST_TEXT, MC_HISTORY_SHARED_SEARCH,
                                 &text, NULL, FALSE, FALSE, INPUT_COMPLETE_NONE),
            QUICK_CHECKBOX (_ ("Cas&e sensitive"), &mcview_search_options.case_sens, NULL),
            QUICK_CHECKBOX (_ ("Show the lines &without it"), &mcview_filter_invert, NULL),
            QUICK_BUTTONS_OK_CANCEL,
            QUICK_END,
            // clang-format on
        };

        WRect r = { -1, -1, 0, 58 };

        quick_dialog_t qdlg = {
            .rect = r,
            .title = _ ("Filter"),
            .help = "[Input Line Keys]",
            .widgets = quick_widgets,
            .callback = NULL,
            .mouse_callback = NULL,
        };

        qd_result = quick_dialog (&qdlg);
    }

    if (qd_result != B_CANCEL)
    {
        if (text[0] == '\0')
            mcview_filter_clear (view);
        else
        {
            GString *needle;

            // in the charset of the file, as the search string
            needle = str_convert_to_input (text);
            if (needle != NULL && needle->len != 0)
                mcview_filter_set (view, text, needle->str, mcview_search_options.case_sens,
                                   mcview_filter_invert);
            if (needle != NULL)
                g_string_free (needle, TRUE);
        }
    }

    g_free (text);
}

/* --------------------------------------------------------------------------------------------- */
/** Switch the filter off, and keep showing the current line. */

void
mcview_filter_clear (WView *view)
{
    if (view->filter == NULL)
        return;

    g_string_free (view->filter, TRUE);
    view->filter = NULL;
    MC_PTR_FREE (view->filter_text);
    g_array_free (view->filter_lines, TRUE);
    view->filter_lines = NULL;

    view->mode_flags.wrap = view->filter_saved_wrap;
    view->dpy_paragraph_skip_lines = 0;
    view->dpy_wrap_dirty = TRUE;
    view->dpy_bbar_dirty = TRUE;
    view->dirty++;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Find the first matching line that starts at @offset or after it.
 *
 * @return its index, or the number of the matching lines if there is none
 */

guint
mcview_filter_find (WView *view, off_t offset)
{
    const GArray *lines;
    guint lo = 0, hi;

    mcview_filter_scan (view, 0, offset);

    lines = view->filter_lines;
    hi = lines->len;

    while (lo < hi)
    {
        const guint mid = lo + (hi - lo) / 2;

        if (g_array_index (lines, off_t, mid) < offset)
            lo = mid + 1;
        else
            hi = mid;
    }

    return lo;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Look for at least @count matching lines.
 *
 * @return the number of the matching lines found: less than @count at the end of the data only
 */

guint
mcview_filter_count (WView *view, guint count)
{
    mcview_filter_scan (view, count, -1);

    return view->filter_lines->len;
}

/* --------------------------------------------------------------------------------------------- */
/** The start offset of the matching line @index (it must have been found already). */

off_t
mcview_filter_line (const WView *view, guint index)
{
    return g_array_index (view->filter_lines, off_t, index);
}

/* --------------------------------------------------------------------------------------------- */
/** Whether the filter hides the line of @offset. */

gboolean
mcview_filter_hides (WView *view, off_t offset)
{
    off_t bol;
    guint i;

    if (!mcview_filter_on (view))
        return FALSE;

    bol = mcview_bol (view, offset, 0);
    i = mcview_filter_find (view, bol);

    return i >= view->filter_lines->len || mcview_filter_line (view, i) != bol;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * The position on the status line with the filter on: the numbers of the first and the last
 * matching line on the screen, and of all the matching lines ("+" if there may be more).
 */

char *
mcview_filter_position (const WView *view)
{
    const guint count = view->filter_lines->len;
    const char *more = view->filter_complete ? "" : "+";

    if (view->filter_shown == 0)
        return g_strdup_printf ("0/%u%s", count, more);

    return g_strdup_printf ("%u-%u/%u%s", view->filter_top + 1,
                            view->filter_top + view->filter_shown, count, more);
}

/* --------------------------------------------------------------------------------------------- */
/** The percent of the matching lines up to the bottom of the screen, -1 if not known yet. */

int
mcview_filter_percent (const WView *view)
{
    const guint count = view->filter_lines->len;

    if (!view->filter_complete || count == 0)
        return -1;

    return (int) ((guint64) (view->filter_top + view->filter_shown) * 100 / count);
}

/* --------------------------------------------------------------------------------------------- */
