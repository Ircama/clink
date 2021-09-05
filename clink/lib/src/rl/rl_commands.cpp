// Copyright (c) 2020 Christopher Antos
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "line_buffer.h"
#include "line_state.h"
#include "word_collector.h"
#include "popup.h"
#include "editor_module.h"
#include "rl_commands.h"
#include "doskey.h"
#include "terminal_helpers.h"

#include <core/base.h>
#include <core/log.h>
#include <core/path.h>
#include <core/settings.h>
#include <terminal/printer.h>
#include <terminal/scroll.h>

extern "C" {
#include <readline/readline.h>
#include <readline/rldefs.h>
#include <readline/rlprivate.h>
#include <readline/history.h>
#include <readline/xmalloc.h>
}

#include <list>
#include <unordered_set>

#include "../../../clink/app/src/version.h" // Ugh.



//------------------------------------------------------------------------------
// Internal ConHost system menu command IDs.
#define ID_CONSOLE_COPY         0xFFF0
#define ID_CONSOLE_PASTE        0xFFF1
#define ID_CONSOLE_MARK         0xFFF2
#define ID_CONSOLE_SCROLL       0xFFF3
#define ID_CONSOLE_FIND         0xFFF4
#define ID_CONSOLE_SELECTALL    0xFFF5
#define ID_CONSOLE_EDIT         0xFFF6
#define ID_CONSOLE_CONTROL      0xFFF7
#define ID_CONSOLE_DEFAULTS     0xFFF8



//------------------------------------------------------------------------------
enum { paste_crlf_delete, paste_crlf_space, paste_crlf_ampersand, paste_crlf_crlf };
static setting_enum g_paste_crlf(
    "clink.paste_crlf",
    "Strips CR and LF chars on paste",
    "Setting this to 'space' makes Clink strip CR and LF characters from text\n"
    "pasted into the current line.  Set this to 'delete' to strip all newline\n"
    "characters to replace them with a space.  Set this to 'ampersand' to replace\n"
    "all newline characters with an ampersand.  Or set this to 'crlf' to paste all\n"
    "newline characters as-is (executing commands that end with newline).",
    "delete,space,ampersand,crlf",
    paste_crlf_crlf);

extern setting_bool g_adjust_cursor_style;
extern setting_bool g_match_wild;

static bool s_force_reload_scripts = false;



//------------------------------------------------------------------------------
extern line_buffer* g_rl_buffer;
extern word_collector* g_word_collector;
extern editor_module::result* g_result;
extern void host_cmd_enqueue_lines(std::list<str_moveable>& lines);
extern void host_add_history(int, const char* line);
extern void host_get_app_context(int& id, str_base& binaries, str_base& profile, str_base& scripts);
extern "C" int show_cursor(int visible);

// This is implemented in the app layer, which makes it inaccessible to lower
// layers.  But Readline and History are siblings, so history_db and rl_module
// and rl_commands should be siblings.  That's a lot of reshuffling for little
// benefit, so just use a forward decl for now.
extern bool expand_history(const char* in, str_base& out);

//------------------------------------------------------------------------------
static void write_line_feed()
{
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written;
    WriteConsoleW(handle, L"\n", 1, &written, nullptr);
}

//------------------------------------------------------------------------------
static void strip_crlf(char* line, std::list<str_moveable>& overflow, int setting, bool* _done)
{
    bool has_overflow = false;
    int prev_was_crlf = 0;
    char* write = line;
    const char* read = line;
    bool done = false;
    while (*read)
    {
        char c = *read;
        if (c != '\n' && c != '\r')
        {
            prev_was_crlf = 0;
            *write = c;
            ++write;
        }
        else if (!prev_was_crlf)
        {
            switch (setting)
            {
            default:
                assert(false);
                // fall through
            case paste_crlf_delete:
                break;
            case paste_crlf_space:
                prev_was_crlf = 1;
                *write = ' ';
                ++write;
                break;
            case paste_crlf_ampersand:
                prev_was_crlf = 1;
                *write = '&';
                ++write;
                break;
            case paste_crlf_crlf:
                has_overflow = true;
                if (c == '\n')
                {
                    *write = '\n';
                    ++write;
                }
                break;
            }
        }

        ++read;
    }

    *write = '\0';

    if (has_overflow)
    {
        bool first = true;
        char* start = line;
        while (*start)
        {
            char* end = start;
            while (*end)
            {
                char c = *end;
                ++end;
                if (c == '\n')
                {
                    done = true;
                    if (first)
                        *(end - 1) = '\0';
                    break;
                }
            }

            if (first)
            {
                first = false;
            }
            else
            {
                unsigned int len = (unsigned int)(end - start);
                overflow.emplace_back();
                str_moveable& back = overflow.back();
                back.reserve(len);
                back.concat(start, len);
            }

            start = end;
        }
    }

    if (_done)
        *_done = done;
}

//------------------------------------------------------------------------------
static void get_word_bounds(const line_buffer& buffer, int* left, int* right)
{
    const char* str = buffer.get_buffer();
    unsigned int cursor = buffer.get_cursor();

    // Determine the word delimiter depending on whether the word's quoted.
    int delim = 0;
    for (unsigned int i = 0; i < cursor; ++i)
    {
        char c = str[i];
        delim += (c == '\"');
    }

    // Search outwards from the cursor for the delimiter.
    delim = (delim & 1) ? '\"' : ' ';
    *left = 0;
    for (int i = cursor - 1; i >= 0; --i)
    {
        char c = str[i];
        if (c == delim)
        {
            *left = i + 1;
            break;
        }
    }

    const char* post = strchr(str + cursor, delim);
    if (post != nullptr)
        *right = int(post - str);
    else
        *right = int(strlen(str));
}



//------------------------------------------------------------------------------
static int s_cua_anchor = -1;

//------------------------------------------------------------------------------
class cua_selection_manager
{
public:
    cua_selection_manager()
    : m_anchor(s_cua_anchor)
    , m_point(rl_point)
    {
        if (s_cua_anchor < 0)
            s_cua_anchor = rl_point;
    }

    ~cua_selection_manager()
    {
        if (g_rl_buffer && (m_anchor != s_cua_anchor || m_point != rl_point))
            g_rl_buffer->set_need_draw();
    }

private:
    int m_anchor;
    int m_point;
};

//------------------------------------------------------------------------------
static void cua_delete()
{
    if (s_cua_anchor >= 0)
    {
        if (g_rl_buffer)
        {
            // Make sure rl_point is lower so it ends up in the right place.
            if (s_cua_anchor < rl_point)
                SWAP(s_cua_anchor, rl_point);
            g_rl_buffer->remove(s_cua_anchor, rl_point);
        }
        cua_clear_selection();
    }
}



//------------------------------------------------------------------------------
int clink_reload(int count, int invoking_key)
{
    assert(g_result);
    s_force_reload_scripts = true;
    if (g_result)
        g_result->done(true); // Force a new edit line so scripts can be reloaded.
    return rl_re_read_init_file(0, 0);
}

//------------------------------------------------------------------------------
int clink_reset_line(int count, int invoking_key)
{
    using_history();
    g_rl_buffer->remove(0, rl_end);
    rl_point = 0;

    return 0;
}

//------------------------------------------------------------------------------
int clink_exit(int count, int invoking_key)
{
    clink_reset_line(1, 0);
    g_rl_buffer->insert("exit");
    rl_newline(1, invoking_key);

    return 0;
}

//------------------------------------------------------------------------------
int clink_ctrl_c(int count, int invoking_key)
{
    if (s_cua_anchor >= 0)
    {
        cua_selection_manager mgr;
        cua_copy(count, invoking_key);
        cua_clear_selection();
        return 0;
    }

    clink_reset_line(1, 0);
    write_line_feed();
    rl_newline(1, invoking_key);

    return 0;
}

//------------------------------------------------------------------------------
int clink_paste(int count, int invoking_key)
{
    if (OpenClipboard(nullptr) == FALSE)
        return 0;

    str<1024> utf8;
    HANDLE clip_data = GetClipboardData(CF_UNICODETEXT);
    if (clip_data != nullptr)
        to_utf8(utf8, (wchar_t*)clip_data);

    CloseClipboard();

    bool done = false;
    bool sel = (s_cua_anchor >= 0);
    std::list<str_moveable> overflow;
    strip_crlf(utf8.data(), overflow, g_paste_crlf.get(), &done);
    if (sel)
    {
        g_rl_buffer->begin_undo_group();
        cua_delete();
    }
    g_rl_buffer->insert(utf8.c_str());
    if (sel)
        g_rl_buffer->end_undo_group();
    host_cmd_enqueue_lines(overflow);
    if (done)
    {
        rl_redisplay();
        rl_newline(1, invoking_key);
    }

    return 0;
}

//------------------------------------------------------------------------------
static void copy_impl(const char* value, int length)
{
    int size = 0;
    if (length)
    {
        size = MultiByteToWideChar(CP_UTF8, 0, value, length, nullptr, 0) * sizeof(wchar_t);
        if (!size)
            return;
    }
    size += sizeof(wchar_t);

    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT, size);
    if (mem == nullptr)
        return;

    if (length)
    {
        wchar_t* data = (wchar_t*)GlobalLock(mem);
        MultiByteToWideChar(CP_UTF8, 0, value, length, data, size);
        GlobalUnlock(mem);
    }

    if (OpenClipboard(nullptr) == FALSE)
        return;

    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, mem); // Windows automatically dynamically converts to CF_TEXT as needed.
    CloseClipboard();
}

//------------------------------------------------------------------------------
int clink_copy_line(int count, int invoking_key)
{
    copy_impl(g_rl_buffer->get_buffer(), g_rl_buffer->get_length());

    return 0;
}

//------------------------------------------------------------------------------
int clink_copy_word(int count, int invoking_key)
{
    if (count < 0 || !g_rl_buffer || !g_word_collector)
    {
Nope:
        rl_ding();
        return 0;
    }

    std::vector<word> words;
    g_word_collector->collect_words(*g_rl_buffer, words, collect_words_mode::whole_command);

    if (words.empty())
        goto Nope;

    if (!rl_explicit_arg)
    {
        unsigned int line_cursor = g_rl_buffer->get_cursor();
        for (auto const& word : words)
        {
            if (line_cursor >= word.offset &&
                line_cursor <= word.offset + word.length)
            {
                copy_impl(g_rl_buffer->get_buffer() + word.offset, word.length);
                return 0;
            }
        }
    }
    else
    {
        for (auto const& word : words)
        {
            if (count-- == 0)
            {
                copy_impl(g_rl_buffer->get_buffer() + word.offset, word.length);
                return 0;
            }
        }
    }

    goto Nope;
}

//------------------------------------------------------------------------------
int clink_copy_cwd(int count, int invoking_key)
{
    wstr<270> cwd;
    unsigned int length = GetCurrentDirectoryW(cwd.size(), cwd.data());
    if (length < cwd.size())
    {
        str<> tmp;
        to_utf8(tmp, cwd.c_str());
        tmp << PATH_SEP;
        path::normalise(tmp);
        copy_impl(tmp.c_str(), tmp.length());
    }

    return 0;
}

//------------------------------------------------------------------------------
int clink_expand_env_var(int count, int invoking_key)
{
    // Extract the word under the cursor.
    int word_left, word_right;
    get_word_bounds(*g_rl_buffer, &word_left, &word_right);

    str<1024> in;
    in.concat(g_rl_buffer->get_buffer() + word_left, word_right - word_left);

    str<> out;
    os::expand_env(in.c_str(), in.length(), out);

    // Update Readline with the resulting expansion.
    g_rl_buffer->begin_undo_group();
    g_rl_buffer->remove(word_left, word_right);
    g_rl_buffer->set_cursor(word_left);
    g_rl_buffer->insert(out.c_str());
    g_rl_buffer->end_undo_group();

    return 0;
}

//------------------------------------------------------------------------------
enum { el_alias = 1, el_envvar = 2, el_history = 4 };
static int do_expand_line(int flags)
{
    bool expanded = false;
    str<> in;
    str<> out;
    int point = rl_point;

    in = g_rl_buffer->get_buffer();

    if (flags & el_history)
    {
        if (expand_history(in.c_str(), out))
        {
            in = out.c_str();
            point = -1;
            expanded = true;
        }
    }

    if (flags & el_alias)
    {
        doskey_alias alias;
        doskey doskey("cmd.exe");
        doskey.resolve(in.c_str(), alias, point < 0 ? nullptr : &point);
        if (alias)
        {
            alias.next(out);
            in = out.c_str();
            expanded = true;
        }
    }

    if (flags & el_envvar)
    {
        if (os::expand_env(in.c_str(), in.length(), out, point < 0 ? nullptr : &point))
        {
            in = out.c_str();
            expanded = true;
        }
    }

    if (!expanded)
    {
        rl_ding();
        return 0;
    }

    g_rl_buffer->begin_undo_group();
    g_rl_buffer->remove(0, rl_end);
    rl_point = 0;
    if (!out.empty())
        g_rl_buffer->insert(out.c_str());
    if (point >= 0 && point <= rl_end)
        g_rl_buffer->set_cursor(point);
    g_rl_buffer->end_undo_group();

    return 0;
}

//------------------------------------------------------------------------------
// Expands a doskey alias (but only the first line, if $T is present).
int clink_expand_doskey_alias(int count, int invoking_key)
{
    return do_expand_line(el_alias);
}

//------------------------------------------------------------------------------
// Performs history expansion.
int clink_expand_history(int count, int invoking_key)
{
    return do_expand_line(el_history);
}

//------------------------------------------------------------------------------
// Performs history and doskey alias expansion.
int clink_expand_history_and_alias(int count, int invoking_key)
{
    return do_expand_line(el_history|el_alias);
}

//------------------------------------------------------------------------------
// Performs history, doskey alias, and environment variable expansion.
int clink_expand_line(int count, int invoking_key)
{
    return do_expand_line(el_history|el_alias|el_envvar);
}

//------------------------------------------------------------------------------
int clink_up_directory(int count, int invoking_key)
{
    g_rl_buffer->begin_undo_group();
    g_rl_buffer->remove(0, ~0u);
    g_rl_buffer->insert(" cd ..");
    g_rl_buffer->end_undo_group();
    rl_newline(1, invoking_key);

    return 0;
}

//------------------------------------------------------------------------------
int clink_insert_dot_dot(int count, int invoking_key)
{
    str<> str;

    if (unsigned int cursor = g_rl_buffer->get_cursor())
    {
        char last_char = g_rl_buffer->get_buffer()[cursor - 1];
        if (last_char != ' ' && !path::is_separator(last_char))
            str << PATH_SEP;
    }

    str << ".." << PATH_SEP;

    g_rl_buffer->insert(str.c_str());

    return 0;
}



//------------------------------------------------------------------------------
int clink_scroll_line_up(int count, int invoking_key)
{
    ScrollConsoleRelative(GetStdHandle(STD_OUTPUT_HANDLE), -1, SCR_BYLINE);
    return 0;
}

//------------------------------------------------------------------------------
int clink_scroll_line_down(int count, int invoking_key)
{
    ScrollConsoleRelative(GetStdHandle(STD_OUTPUT_HANDLE), 1, SCR_BYLINE);
    return 0;
}

//------------------------------------------------------------------------------
int clink_scroll_page_up(int count, int invoking_key)
{
    ScrollConsoleRelative(GetStdHandle(STD_OUTPUT_HANDLE), -1, SCR_BYPAGE);
    return 0;
}

//------------------------------------------------------------------------------
int clink_scroll_page_down(int count, int invoking_key)
{
    ScrollConsoleRelative(GetStdHandle(STD_OUTPUT_HANDLE), 1, SCR_BYPAGE);
    return 0;
}

//------------------------------------------------------------------------------
int clink_scroll_top(int count, int invoking_key)
{
    ScrollConsoleRelative(GetStdHandle(STD_OUTPUT_HANDLE), -1, SCR_TOEND);
    return 0;
}

//------------------------------------------------------------------------------
int clink_scroll_bottom(int count, int invoking_key)
{
    ScrollConsoleRelative(GetStdHandle(STD_OUTPUT_HANDLE), 1, SCR_TOEND);
    return 0;
}



//------------------------------------------------------------------------------
int clink_find_conhost(int count, int invoking_key)
{
    HWND hwndConsole = GetConsoleWindow();
    if (!hwndConsole)
    {
        rl_ding();
        return 0;
    }

    // Invoke conhost's Find command via the system menu.
    SendMessage(hwndConsole, WM_SYSCOMMAND, ID_CONSOLE_FIND, 0);
    return 0;
}

//------------------------------------------------------------------------------
int clink_mark_conhost(int count, int invoking_key)
{
    HWND hwndConsole = GetConsoleWindow();
    if (!hwndConsole)
    {
        rl_ding();
        return 0;
    }

    // Conhost's Mark command is asynchronous and saves/restores the cursor info
    // and position.  So we need to trick the cursor into being visible, so that
    // it gets restored as visible since that's the state Readline will be in
    // after the Mark command finishes.
    if (g_adjust_cursor_style.get())
    {
        HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_CURSOR_INFO info;
        GetConsoleCursorInfo(handle, &info);
        info.bVisible = true;
        SetConsoleCursorInfo(handle, &info);
    }

    // Invoke conhost's Mark command via the system menu.
    SendMessage(hwndConsole, WM_SYSCOMMAND, ID_CONSOLE_MARK, 0);
    return 0;
}



//------------------------------------------------------------------------------
extern const char** host_copy_dir_history(int* total);
int clink_popup_directories(int count, int invoking_key)
{
    // Copy the directory list (just a shallow copy of the dir pointers).
    int total = 0;
    const char** history = host_copy_dir_history(&total);
    if (!history || !total)
    {
        free(history);
        rl_ding();
        return 0;
    }

    // Popup list.
    str<> choice;
    int current = total - 1;
    popup_list_result result = do_popup_list("Directories",
        (const char **)history, total, 0, 0,
        false/*completing*/, false/*auto_complete*/, false/*reverse_find*/,
        current, choice);
    switch (result)
    {
    case popup_list_result::cancel:
        break;
    case popup_list_result::error:
        rl_ding();
        break;
    case popup_list_result::select:
    case popup_list_result::use:
        {
            bool end_sep = (history[current][0] &&
                            path::is_separator(history[current][strlen(history[current]) - 1]));

            char qs[2] = {};
            if (rl_basic_quote_characters &&
                rl_basic_quote_characters[0] &&
                rl_filename_quote_characters &&
                _rl_strpbrk(history[current], rl_filename_quote_characters) != 0)
            {
                qs[0] = rl_basic_quote_characters[0];
            }

            str<> dir;
            dir.format("%s%s%s", qs, history[current], qs);

            bool use = (result == popup_list_result::use);
            rl_begin_undo_group();
            if (use)
            {
                if (!end_sep)
                    dir.concat(PATH_SEP);
                rl_replace_line(dir.c_str(), 0);
                rl_point = rl_end;
            }
            else
            {
                rl_insert_text(dir.c_str());
            }
            rl_end_undo_group();
            rl_redisplay();
            if (use)
                rl_newline(1, invoking_key);
        }
        break;
    }

    free(history);

    return 0;
}



//------------------------------------------------------------------------------
extern bool host_call_lua_rl_global_function(const char* func_name);

//------------------------------------------------------------------------------
int clink_complete_numbers(int count, int invoking_key)
{
    if (!host_call_lua_rl_global_function("clink._complete_numbers"))
        rl_ding();
    return 0;
}

//------------------------------------------------------------------------------
int clink_menu_complete_numbers(int count, int invoking_key)
{
    if (!host_call_lua_rl_global_function("clink._menu_complete_numbers"))
        rl_ding();
    return 0;
}

//------------------------------------------------------------------------------
int clink_menu_complete_numbers_backward(int count, int invoking_key)
{
    if (!host_call_lua_rl_global_function("clink._menu_complete_numbers_backward"))
        rl_ding();
    return 0;
}

//------------------------------------------------------------------------------
int clink_old_menu_complete_numbers(int count, int invoking_key)
{
    if (!host_call_lua_rl_global_function("clink._old_menu_complete_numbers"))
        rl_ding();
    return 0;
}

//------------------------------------------------------------------------------
int clink_old_menu_complete_numbers_backward(int count, int invoking_key)
{
    if (!host_call_lua_rl_global_function("clink._old_menu_complete_numbers_backward"))
        rl_ding();
    return 0;
}

//------------------------------------------------------------------------------
int clink_popup_complete_numbers(int count, int invoking_key)
{
    if (!host_call_lua_rl_global_function("clink._popup_complete_numbers"))
        rl_ding();
    return 0;
}

//------------------------------------------------------------------------------
int clink_popup_show_help(int count, int invoking_key)
{
    if (!host_call_lua_rl_global_function("clink._popup_show_help"))
        rl_ding();
    return 0;
}



//------------------------------------------------------------------------------
int clink_select_complete(int count, int invoking_key)
{
    extern bool activate_select_complete(editor_module::result& result, bool reactivate);
    if (!g_result || !activate_select_complete(*g_result, rl_last_func == clink_select_complete))
        rl_ding();
    return 0;
}



//------------------------------------------------------------------------------
void cua_clear_selection()
{
    s_cua_anchor = -1;
}

//------------------------------------------------------------------------------
bool cua_point_in_selection(int in)
{
    if (s_cua_anchor < 0)
        return false;
    if (s_cua_anchor < rl_point)
        return (s_cua_anchor <= in && in < rl_point);
    else
        return (rl_point <= in && in < s_cua_anchor);
}

//------------------------------------------------------------------------------
int cua_selection_event_hook(int event)
{
    if (!g_rl_buffer)
        return 0;

    static bool s_cleanup = false;

    switch (event)
    {
    case SEL_BEFORE_INSERTCHAR:
        assert(!s_cleanup);
        if (s_cua_anchor >= 0)
        {
            s_cleanup = true;
            g_rl_buffer->begin_undo_group();
            cua_delete();
        }
        break;
    case SEL_AFTER_INSERTCHAR:
        if (s_cleanup)
        {
            g_rl_buffer->end_undo_group();
            s_cleanup = false;
        }
        break;
    case SEL_BEFORE_DELETE:
        if (s_cua_anchor < 0 || s_cua_anchor == rl_point)
            break;
        cua_delete();
        return 1;
    }

    return 0;
}

//------------------------------------------------------------------------------
void cua_after_command(bool force_clear)
{
    static std::unordered_set<void*> s_map;

    if (s_map.empty())
    {
        // No action after a cua command.
        s_map.emplace(cua_backward_char);
        s_map.emplace(cua_forward_char);
        s_map.emplace(cua_backward_word);
        s_map.emplace(cua_forward_word);
        s_map.emplace(cua_beg_of_line);
        s_map.emplace(cua_end_of_line);
        s_map.emplace(cua_select_all);
        s_map.emplace(cua_copy);
        s_map.emplace(cua_cut);

        // No action after scroll commands.
        s_map.emplace(clink_scroll_line_up);
        s_map.emplace(clink_scroll_line_down);
        s_map.emplace(clink_scroll_page_up);
        s_map.emplace(clink_scroll_page_down);
        s_map.emplace(clink_scroll_top);
        s_map.emplace(clink_scroll_bottom);

        // No action after some special commands.
        s_map.emplace(show_rl_help);
        s_map.emplace(show_rl_help_raw);
    }

    // If not a recognized command, clear the cua selection.
    if (s_map.find((void*)rl_last_func) == s_map.end())
        cua_clear_selection();
}

//------------------------------------------------------------------------------
int cua_backward_char(int count, int invoking_key)
{
    cua_selection_manager mgr;
    return rl_backward_char(count, invoking_key);
}

//------------------------------------------------------------------------------
int cua_forward_char(int count, int invoking_key)
{
    cua_selection_manager mgr;
    return rl_forward_char(count, invoking_key);
}

//------------------------------------------------------------------------------
int cua_backward_word(int count, int invoking_key)
{
    cua_selection_manager mgr;
    return rl_backward_word(count, invoking_key);
}

//------------------------------------------------------------------------------
int cua_forward_word(int count, int invoking_key)
{
    cua_selection_manager mgr;
    return rl_forward_word(count, invoking_key);
}

//------------------------------------------------------------------------------
int cua_beg_of_line(int count, int invoking_key)
{
    cua_selection_manager mgr;
    return rl_beg_of_line(count, invoking_key);
}

//------------------------------------------------------------------------------
int cua_end_of_line(int count, int invoking_key)
{
    cua_selection_manager mgr;
    return rl_end_of_line(count, invoking_key);
}

//------------------------------------------------------------------------------
int cua_select_all(int count, int invoking_key)
{
    cua_selection_manager mgr;
    s_cua_anchor = 0;
    rl_point = rl_end;
    return 0;
}

//------------------------------------------------------------------------------
int cua_copy(int count, int invoking_key)
{
    if (g_rl_buffer)
    {
        bool has_sel = (s_cua_anchor >= 0);
        unsigned int len = g_rl_buffer->get_length();
        unsigned int beg = has_sel ? min<unsigned int>(len, s_cua_anchor) : 0;
        unsigned int end = has_sel ? min<unsigned int>(len, rl_point) : len;
        if (beg > end)
            SWAP(beg, end);
        if (beg < end)
            copy_impl(g_rl_buffer->get_buffer() + beg, end - beg);
    }
    return 0;
}

//------------------------------------------------------------------------------
int cua_cut(int count, int invoking_key)
{
    cua_copy(0, 0);
    cua_delete();
    return 0;
}



//------------------------------------------------------------------------------
static bool s_globbing_wild = false;
static bool s_literal_wild = false;
bool is_globbing_wild() { return s_globbing_wild; }
bool is_literal_wild() { return s_literal_wild; }

//------------------------------------------------------------------------------
static int glob_completion_internal(int what_to_do)
{
    s_globbing_wild = true;
    if (!rl_explicit_arg)
        s_literal_wild = true;

    return rl_complete_internal(what_to_do);
}

//------------------------------------------------------------------------------
int glob_complete_word(int count, int invoking_key)
{
    if (rl_editing_mode == emacs_mode)
        rl_explicit_arg = 1; /* force `*' append */

    return glob_completion_internal(rl_completion_mode(glob_complete_word));
}

//------------------------------------------------------------------------------
int glob_expand_word(int count, int invoking_key)
{
    return glob_completion_internal('*');
}

//------------------------------------------------------------------------------
int glob_list_expansions(int count, int invoking_key)
{
    return glob_completion_internal('?');
}



//------------------------------------------------------------------------------
int edit_and_execute_command(int count, int invoking_key)
{
    str<> line;
    if (rl_explicit_arg)
    {
        HIST_ENTRY* h = history_get(count);
        if (!h)
        {
            rl_ding();
            return 0;
        }
        line = h->line;
    }
    else
    {
        line.concat(rl_line_buffer, rl_end);
        host_add_history(0, line.c_str());
    }

    str_moveable tmp_file;
    FILE* file = os::create_temp_file(&tmp_file);
    if (!file)
    {
LDing:
        rl_ding();
        return 0;
    }

    if (fputs(line.c_str(), file) < 0)
    {
        fclose(file);
LUnlinkFile:
        unlink(tmp_file.c_str());
        goto LDing;
    }
    fclose(file);
    file = nullptr;

    // Save and reset console state.
    HANDLE std_handles[2] = { GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE) };
    DWORD prev_mode[2];
    static_assert(_countof(std_handles) == _countof(prev_mode), "array sizes much match");
    for (size_t i = 0; i < _countof(std_handles); ++i)
        GetConsoleMode(std_handles[i], &prev_mode[i]);
    SetConsoleMode(std_handles[0], (prev_mode[0] | ENABLE_PROCESSED_INPUT) & ~(ENABLE_WINDOW_INPUT|ENABLE_MOUSE_INPUT));
    bool was_visible = show_cursor(true);
    rl_clear_signals();

    // Build editor command.
    str<> editor;
    str_moveable command;
    const char* const qs = (strpbrk(tmp_file.c_str(), rl_filename_quote_characters)) ? "\"" : "";
    if ((!os::get_env("VISUAL", editor) && !os::get_env("EDITOR", editor)) || editor.empty())
        editor = "%systemroot%\\system32\\notepad.exe";
    command.format("%s %s%s%s", editor.c_str(), qs, tmp_file.c_str(), qs);

    // Execute editor command.
    wstr_moveable wcommand(command.c_str());
    const int exit_code = _wsystem(wcommand.c_str());

    // Restore console state.
    show_cursor(was_visible);
    for (size_t i = 0; i < _countof(std_handles); ++i)
        SetConsoleMode(std_handles[i], prev_mode[i]);
    rl_set_signals();

    // Was the editor launched successfully?
    if (exit_code < 0)
        goto LUnlinkFile;

    // Read command(s) from temp file.
    line.clear();
    wstr_moveable wtmp_file(tmp_file.c_str());
    file = _wfopen(wtmp_file.c_str(), L"rt");
    if (!file)
        goto LUnlinkFile;
    char buffer[4096];
    while (true)
    {
        const int len = fread(buffer, 1, sizeof(buffer), file);
        if (len <= 0)
            break;
        line.concat(buffer, len);
    }
    fclose(file);

    // Trim trailing newlines to avoid redundant blank commands.  Ensure a final
    // newline so all lines get executed (otherwise it will go into edit mode).
    while (line.length() && line.c_str()[line.length() - 1] == '\n')
        line.truncate(line.length() - 1);
    line.concat("\n");

    // Split into multiple lines.
    std::list<str_moveable> overflow;
    strip_crlf(line.data(), overflow, paste_crlf_crlf, nullptr);

    // Replace the input line with the content from the temp file.
    g_rl_buffer->begin_undo_group();
    g_rl_buffer->remove(0, rl_end);
    rl_point = 0;
    if (!line.empty())
        g_rl_buffer->insert(line.c_str());
    g_rl_buffer->end_undo_group();

    // Queue any additional lines.
    host_cmd_enqueue_lines(overflow);

    // Accept the input and execute it.
    rl_redisplay();
    rl_newline(1, invoking_key);

    return 0;
}



//------------------------------------------------------------------------------
int clink_diagnostics(int count, int invoking_key)
{
    end_prompt(true/*crlf*/);

    static char bold[] = "\x1b[1m";
    static char norm[] = "\x1b[m";
    static char lf[] = "\n";

    str<> s;
    const int spacing = 12;

    int id = 0;
    str<> binaries;
    str<> profile;
    str<> scripts;
    host_get_app_context(id, binaries, profile, scripts);

    // Version and binaries dir.

    s.clear();
    s << bold << "version:" << norm << lf;
    g_printer->print(s.c_str(), s.length());

    printf("  %-*s  %s\n", spacing, "version", CLINK_VERSION_STR);
    printf("  %-*s  %s\n", spacing, "binaries", binaries.c_str());

    // Session info.

    s.clear();
    s <<bold << "session:" << norm << lf;
    g_printer->print(s.c_str(), s.length());

    printf("  %-*s  %d\n", spacing, "session", id);

    printf("  %-*s  %s\n", spacing, "profile", profile.c_str());
    if (scripts.length())
        printf("  %-*s  %s\n", spacing, "scripts", scripts.c_str());

    host_call_lua_rl_global_function("clink._diagnostics");

    puts("");

    rl_forced_update_display();
    return 0;
}



//------------------------------------------------------------------------------
int macro_hook_func(const char* macro)
{
    bool is_luafunc = (macro && strnicmp(macro, "luafunc:", 8) == 0);

    if (is_luafunc)
    {
        str<> func_name;
        func_name = macro + 8;
        func_name.trim();

        // TODO: Ideally optimize this so that it only resets match generation if
        // the Lua function triggers completion.
        extern void reset_generate_matches();
        reset_generate_matches();

        HANDLE std_handles[2] = { GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE) };
        DWORD prev_mode[2];
        static_assert(_countof(std_handles) == _countof(prev_mode), "array sizes much match");
        for (size_t i = 0; i < _countof(std_handles); ++i)
            GetConsoleMode(std_handles[i], &prev_mode[i]);

        if (!host_call_lua_rl_global_function(func_name.c_str()))
            rl_ding();

        for (size_t i = 0; i < _countof(std_handles); ++i)
            SetConsoleMode(std_handles[i], prev_mode[i]);
    }

    cua_after_command(true/*force_clear*/);

    return is_luafunc;
}

//------------------------------------------------------------------------------
void reset_command_states()
{
    s_globbing_wild = false;
    s_literal_wild = false;
}

//------------------------------------------------------------------------------
bool is_force_reload_scripts()
{
    return s_force_reload_scripts;
}

//------------------------------------------------------------------------------
void clear_force_reload_scripts()
{
    s_force_reload_scripts = false;
}
