// Copyright (c) 2012 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "paths.h"

#include <core/str.h>

extern "C" {
#include <readline/history.h>
}

//------------------------------------------------------------------------------
int                 get_clink_setting_int(const char*);
static int          g_new_history_count             = 0;

//------------------------------------------------------------------------------
static void get_history_file_name(str_base& buffer)
{
    get_config_dir(buffer);
    buffer << "/history";
}

//------------------------------------------------------------------------------
void load_history()
{
    str<512> buffer;
    get_history_file_name(buffer);

    // Clear existing history.
    clear_history();
    g_new_history_count = 0;

    // Read from disk.
    read_history(buffer.c_str());
    using_history();
}

//------------------------------------------------------------------------------
void save_history()
{
    str<512> buffer;
    get_history_file_name(buffer);

    // Get max history size.
    int max_history = get_clink_setting_int("history_file_lines");
    max_history = (max_history == 0) ? INT_MAX : max_history;
    if (max_history < 0)
    {
        unlink(buffer.c_str());
        return;
    }

    // Write new history to the file, and truncate to our maximum.
    int always_write = get_clink_setting_int("history_io");
    if (always_write || append_history(g_new_history_count, buffer.c_str()) != 0)
        write_history(buffer.c_str());

    if (max_history != INT_MAX)
        history_truncate_file(buffer.c_str(), max_history);

    g_new_history_count = 0;
}

//------------------------------------------------------------------------------
static int find_duplicate(const char* line)
{
    HIST_ENTRY* hist_entry;

    using_history();
    while (hist_entry = previous_history())
        if (strcmp(hist_entry->line, line) == 0)
            return where_history();

    return -1;
}

//------------------------------------------------------------------------------
void add_to_history(const char* line)
{
    int dupe_mode;
    const unsigned char* c;

    // Maybe we shouldn't add this line to the history at all?
    c = (const unsigned char*)line;
    if (isspace(*c) && get_clink_setting_int("history_ignore_space") > 0)
        return;

    // Skip leading whitespace
    while (*c)
    {
        if (!isspace(*c))
            break;

        ++c;
    }

    // Skip empty lines
    if (*c == '\0')
        return;

    // Check if the line's a duplicate of and existing history entry.
    dupe_mode = get_clink_setting_int("history_dupe_mode");
    if (dupe_mode > 0)
    {
        int where = find_duplicate((const char*)c);
        if (where >= 0)
        {
            if (dupe_mode > 1)
            {
                HIST_ENTRY* entry = remove_history(where);
                free_history_entry(entry);
            }
            else
                return;
        }
    }

    // All's well. Add the line.
    using_history();
    add_history(line);
    ++g_new_history_count;
}

//------------------------------------------------------------------------------
int expand_from_history(const char* text, char** expanded)
{
    int result;

    result = history_expand((char*)text, expanded);
    if (result < 0)
        free(*expanded);

    return result;
}

//------------------------------------------------------------------------------
int history_expand_control(char* line, int marker_pos)
{
    int setting, in_quote, i;

    setting = get_clink_setting_int("history_expand_mode");
    if (setting <= 1)
        return (setting <= 0);

    // Is marker_pos inside a quote of some kind?
    in_quote = 0;
    for (i = 0; i < marker_pos && *line; ++i, ++line)
    {
        int c = *line;
        if (c == '\'' || c == '\"')
            in_quote = (c == in_quote) ? 0 : c;
    }

    switch (setting)
    {
    case 2: return (in_quote == '\'');
    case 3: return (in_quote == '\"');
    case 4: return (in_quote == '\"' || in_quote == '\'');
    }

    return 0;
}
