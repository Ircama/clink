// Copyright (c) 2016 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "host.h"
#include "host_lua.h"
#include "version.h"

#include <core/globber.h>
#include <core/os.h>
#include <core/path.h>
#include <core/settings.h>
#include <core/str.h>
#include <core/str_compare.h>
#include <core/str_tokeniser.h>
#include <core/str_transform.h>
#include <lib/doskey.h>
#include <lib/match_generator.h>
#include <lib/line_editor.h>
#include <lib/terminal_helpers.h>
#include <lua/lua_script_loader.h>
#include <lua/lua_state.h>
#include <lua/lua_match_generator.h>
#include <lua/prompt.h>
#include <terminal/printer.h>
#include <terminal/terminal_out.h>
#include <utils/app_context.h>

#include <list>
#include <memory>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

//------------------------------------------------------------------------------
static setting_enum g_ignore_case(
    "match.ignore_case",
    "Case insensitive matching",
    "Toggles whether case is ignored when selecting matches.  The 'relaxed'\n"
    "option will also consider -/_ as equal.",
    "off,on,relaxed",
    2);

static setting_bool g_fuzzy_accent(
    "match.ignore_accent",
    "Accent insensitive matching",
    "Toggles whether accents on characters are ignored when selecting matches.",
    true);

static setting_bool g_filter_prompt(
    "clink.promptfilter",
    "Enable prompt filtering by Lua scripts",
    true);

static setting_enum s_prompt_transient(
    "prompt.transient",
    "Controls when past prompts are collapsed",
    "The default is 'off' which never collapses past prompts.  Set to 'always' to\n"
    "always collapse past prompts.  Set to 'same_dir' to only collapse past prompts\n"
    "when the current working directory hasn't changed since the last prompt.",
    "off,always,same_dir",
    0);

setting_bool g_save_history(
    "history.save",
    "Save history between sessions",
    "Changing this setting only takes effect for new instances.",
    true);

static setting_str g_exclude_from_history_cmds(
    "history.dont_add_to_history_cmds",
    "Commands not automatically added to the history",
    "List of commands that aren't automatically added to the history.\n"
    "Commands are separated by spaces, commas, or semicolons.  Default is\n"
    "\"exit history\", to exclude both of those commands.",
    "exit history");

static setting_bool g_reload_scripts(
    "lua.reload_scripts",
    "Reload scripts on every prompt",
    "When true, Lua scripts are reloaded on every prompt.  When false, Lua scripts\n"
    "are loaded once.  This setting can be changed while Clink is running and takes\n"
    "effect at the next prompt.",
    false);

static setting_bool g_get_errorlevel(
    "cmd.get_errorlevel",
    "Retrieve last exit code",
    "When this is enabled, Clink runs a hidden 'echo %errorlevel%' command before\n"
    "each interactive input prompt to retrieve the last exit code for use by Lua\n"
    "scripts.  If you experience problems, try turning this off.  This is on by\n"
    "default.",
    true);

extern setting_bool g_classify_words;
extern setting_color g_color_prompt;
extern setting_bool g_prompt_async;

extern void start_logger();

extern bool get_sticky_search_history();
extern bool has_sticky_search_position();
extern bool get_sticky_search_add_history(const char* line);
extern void clear_sticky_search_position();
extern void reset_keyseq_to_name_map();
extern void set_prompt(const char* prompt, const char* rprompt, bool redisplay);



//------------------------------------------------------------------------------
extern str<> g_last_prompt;



//------------------------------------------------------------------------------
static void get_errorlevel_tmp_name(str_base& out, bool wild=false)
{
    app_context::get()->get_log_path(out);
    path::to_parent(out, nullptr);
    path::append(out, "clink_errorlevel");

    if (wild)
    {
        // "clink_errorlevel*.txt" catches the obsolete clink_errorlevel.txt
        // file as well.
        out << "*.txt";
    }
    else
    {
        str<> name;
        name.format("_%X.txt", GetCurrentProcessId());
        out.concat(name.c_str(), name.length());
    }
}



//------------------------------------------------------------------------------
class dir_history_entry : public no_copy
{
public:
                    dir_history_entry(const char* s);
                    dir_history_entry(dir_history_entry&& d) { dir = d.dir; d.dir = nullptr; }
                    ~dir_history_entry() { free(dir); }

    const char*     get() const { return dir; }

private:
    char* dir;
};

//------------------------------------------------------------------------------
dir_history_entry::dir_history_entry(const char* s)
{
    size_t alloc = strlen(s) + 1;
    dir = (char *)malloc(alloc);
    memcpy(dir, s, alloc);
}

//------------------------------------------------------------------------------
const int c_max_dir_history = 100;
static std::list<dir_history_entry> s_dir_history;

//------------------------------------------------------------------------------
static void update_dir_history()
{
    str<> cwd;
    os::get_current_dir(cwd);

    // Add cwd to tail.
    if (!s_dir_history.size() || _stricmp(s_dir_history.back().get(), cwd.c_str()) != 0)
        s_dir_history.push_back(cwd.c_str());

    // Trim overflow from head.
    while (s_dir_history.size() > c_max_dir_history)
        s_dir_history.pop_front();
}

//------------------------------------------------------------------------------
static void prev_dir_history(str_base& inout)
{
    inout.clear();

    if (s_dir_history.size() < 2)
        return;

    auto a = s_dir_history.rbegin();
    a++;

    inout.format(" cd /d \"%s\"", a->get());
}



//------------------------------------------------------------------------------
static void write_line_feed()
{
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written;
    WriteConsoleW(handle, L"\n", 1, &written, nullptr);
}

//------------------------------------------------------------------------------
static bool parse_line_token(str_base& out, const char* line)
{
    out.clear();

    // Skip leading whitespace.
    while (*line == ' ' || *line == '\t')
        line++;

    // Parse the line text.
    bool first_component = true;
    for (bool quoted = false; true; line++)
    {
        // Spaces are acceptable when quoted, otherwise it's the end of the
        // token and any subsequent text defeats the directory shortcut feature.
        if (*line == ' ' || *line == '\t')
        {
            if (!quoted)
            {
                // Skip trailing whitespace.
                while (*line == ' ' || *line == '\t')
                    line++;
                // Parse fails if input is more than a single token.
                if (*line)
                    return false;
            }
        }

        // Parse succeeds if input is one token.
        if (!*line)
            return out.length();

        switch (*line)
        {
            // These characters defeat the directory shortcut feature.
        case '^':
        case '<':
        case '|':
        case '>':
        case '%':
            return false;

            // These characters are acceptable when quoted.
        case '@':
        case '(':
        case ')':
        case '&':
        case '+':
        case '=':
        case ';':
        case ',':
            if (!quoted)
                return false;
            break;

            // Quotes toggle quote mode.
        case '"':
            first_component = false;
            quoted = !quoted;
            continue;

            // These characters end a component.
        case '.':
        case '/':
        case '\\':
            if (first_component)
            {
                // Some commands are special and defeat the directory shortcut
                // feature even if they're legitimately part of an actual path,
                // unless they are quoted.
                static const char* const c_commands[] = { "call", "cd", "chdir", "dir", "echo", "md", "mkdir", "popd", "pushd" };
                for (const char* name : c_commands)
                    if (out.iequals(name))
                        return false;
                first_component = false;
            }
            break;
        }

        out.concat(line, 1);
    }
}

//------------------------------------------------------------------------------
static bool intercept_directory(str_base& inout)
{
    const char* line = inout.c_str();

    // Check for '-' (etc) to change to previous directory.
    if (strcmp(line, "-") == 0 ||
        _strcmpi(line, "cd -") == 0 ||
        _strcmpi(line, "chdir -") == 0)
    {
        prev_dir_history(inout);
        return true;
    }

    // Parse the input for a single token.
    str<> tmp;
    if (!parse_line_token(tmp, line))
        return false;

    // If all dots, convert into valid path syntax moving N-1 levels.
    // Examples:
    //  - "..." becomes "..\..\"
    //  - "...." becomes "..\..\..\"
    int num_dots = 0;
    for (const char* p = tmp.c_str(); *p; ++p, ++num_dots)
    {
        if (*p != '.')
        {
            if (!path::is_separator(p[0]) || p[1]) // Allow "...\"
                num_dots = -1;
            break;
        }
    }
    if (num_dots >= 2)
    {
        tmp.clear();
        while (num_dots > 1)
        {
            tmp.concat("..\\");
            --num_dots;
        }
    }

    // If the input doesn't end with a separator, don't handle it.  Otherwise
    // it would interfere with launching something found on the PATH but with
    // the same name as a subdirectory of the current working directory.
    if (!path::is_separator(tmp.c_str()[tmp.length() - 1]))
    {
        // But allow a special case for "..\.." and "..\..\..", etc.
        const char* p = tmp.c_str();
        while (true)
        {
            if (p[0] != '.' || p[1] != '.')
                return false;
            if (p[2] == '\0')
            {
                tmp.concat("\\");
                break;
            }
            if (!path::is_separator(p[2]))
                return false;
            p += 3;
        }
    }

    // Tilde expansion.
    if (tmp.first_of('~') >= 0)
    {
        char* expanded_path = tilde_expand(tmp.c_str());
        if (expanded_path)
        {
            if (!tmp.equals(expanded_path))
                tmp = expanded_path;
            free(expanded_path);
        }
    }

    if (os::get_path_type(tmp.c_str()) != os::path_type_dir)
        return false;

    // Normalize to system path separator, since `cd /d "/foo/"` fails because
    // the `/d` flag disables `cd` accepting forward slashes in paths.
    path::normalise_separators(tmp);

    inout.format(" cd /d \"%s\"", tmp.c_str());
    return true;
}



//------------------------------------------------------------------------------
struct cwd_restorer
{
    cwd_restorer() { os::get_current_dir(m_path); }
    ~cwd_restorer() { os::set_current_dir(m_path.c_str()); }
    str<288> m_path;
};



//------------------------------------------------------------------------------
struct autostart_display
{
    void save();
    void restore();

    COORD m_pos = {};
    COORD m_saved = {};
    std::unique_ptr<CHAR_INFO[]> m_screen_content;
};

//------------------------------------------------------------------------------
void autostart_display::save()
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleScreenBufferInfo(h, &csbi))
        return;
    if (csbi.dwCursorPosition.X != 0)
        return;

    m_pos = csbi.dwCursorPosition;
    m_saved.X = csbi.dwSize.X;
    m_saved.Y = 1;
    m_screen_content = std::unique_ptr<CHAR_INFO[]>(new CHAR_INFO[csbi.dwSize.X]);

    SMALL_RECT sr { 0, m_pos.Y, static_cast<SHORT>(csbi.dwSize.X - 1), m_pos.Y };
    if (!ReadConsoleOutput(h, m_screen_content.get(), m_saved, COORD {}, &sr))
        m_screen_content.reset();
}

//------------------------------------------------------------------------------
void autostart_display::restore()
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleScreenBufferInfo(h, &csbi))
        return;
    if (csbi.dwCursorPosition.X != 0)
        return;

    if (m_pos.Y + 1 != csbi.dwCursorPosition.Y)
        return;
    if (m_saved.X != csbi.dwSize.X)
        return;

    SMALL_RECT sr { 0, m_pos.Y, static_cast<SHORT>(csbi.dwSize.X - 1), m_pos.Y };
    std::unique_ptr<CHAR_INFO[]> screen_content = std::unique_ptr<CHAR_INFO[]>(new CHAR_INFO[csbi.dwSize.X]);
    if (!ReadConsoleOutput(h, screen_content.get(), m_saved, COORD {}, &sr))
        return;

    if (memcmp(screen_content.get(), m_screen_content.get(), sizeof(CHAR_INFO) * m_saved.Y * m_saved.X) != 0)
        return;

    SetConsoleCursorPosition(h, m_pos);
}

//------------------------------------------------------------------------------
static void move_cursor_up_one_line()
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleScreenBufferInfo(h, &csbi))
    {
        if (csbi.dwCursorPosition.Y > 0)
            csbi.dwCursorPosition.Y--;
        SetConsoleCursorPosition(h, csbi.dwCursorPosition);
    }
}



//------------------------------------------------------------------------------
host::host(const char* name)
: m_name(name)
, m_doskey(os::get_shellname())
{
    m_terminal = terminal_create();
    m_printer = new printer(*m_terminal.out);
}

//------------------------------------------------------------------------------
host::~host()
{
    purge_old_files();

    delete m_prompt_filter;
    delete m_lua;
    delete m_history;
    delete m_printer;
    terminal_destroy(m_terminal);
}

//------------------------------------------------------------------------------
void host::enqueue_lines(std::list<str_moveable>& lines)
{
    for (auto& line : lines)
        m_queued_lines.emplace_back(std::move(line));
}

//------------------------------------------------------------------------------
bool host::dequeue_line(wstr_base& out)
{
    if (m_queued_lines.empty())
        return false;

    out = m_queued_lines.front().c_str();
    m_queued_lines.pop_front();
    return true;
}

//------------------------------------------------------------------------------
void host::add_history(const char* line)
{
    m_history->add(line);
}

//------------------------------------------------------------------------------
void host::remove_history(int rl_history_index, const char* line)
{
    m_history->remove(rl_history_index, line);
}

//------------------------------------------------------------------------------
void host::filter_prompt()
{
    if (!g_prompt_async.get())
        return;

    const char* rprompt = nullptr;
    const char* prompt = filter_prompt(&rprompt, false/*transient*/);
    set_prompt(prompt, rprompt, true/*redisplay*/);
}

//------------------------------------------------------------------------------
void host::filter_transient_prompt(bool final)
{
    if (!m_can_transient)
        return;

    const char* rprompt;
    const char* prompt;

    // Replace old prompt with transient prompt.
    rprompt = nullptr;
    prompt = filter_prompt(&rprompt, true/*transient*/);
    set_prompt(prompt, rprompt, true/*redisplay*/);

    if (final)
        return;

    // Refilter new prompt, but don't redisplay (which would replace the prompt
    // again; not what is needed here).  Instead let the prompt get displayed
    // again naturally in due time.
    rprompt = nullptr;
    prompt = filter_prompt(&rprompt, false/*transient*/);
    set_prompt(prompt, rprompt, false/*redisplay*/);
}

//------------------------------------------------------------------------------
void host::filter_matches(char** matches)
{
    if (m_lua)
        m_lua->call_lua_filter_matches(matches, rl_completion_type, rl_filename_completion_desired);
}

//------------------------------------------------------------------------------
bool host::call_lua_rl_global_function(const char* func_name)
{
    return m_lua && m_lua->call_lua_rl_global_function(func_name);
}

//------------------------------------------------------------------------------
const char** host::copy_dir_history(int* total)
{
    if (!s_dir_history.size())
        return nullptr;

    // Copy the directory list (just a shallow copy of the dir pointers).
    const char** history = (const char**)malloc(sizeof(*history) * s_dir_history.size());
    int i = 0;
    for (auto const& it : s_dir_history)
        history[i++] = it.get();

    *total = i;
    return history;
}

//------------------------------------------------------------------------------
void host::get_app_context(int& id, str_base& binaries, str_base& profile, str_base& scripts)
{
    const auto* context = app_context::get();

    id = context->get_id();
    context->get_binaries_dir(binaries);
    context->get_state_dir(profile);
    context->get_script_path_readable(scripts);
}

//------------------------------------------------------------------------------
bool host::edit_line(const char* prompt, const char* rprompt, str_base& out)
{
    assert(!m_prompt); // Reentrancy not supported!

    const app_context* app = app_context::get();
    bool reset = app->update_env();

    path::refresh_pathext();

    cwd_restorer cwd;
    printer_context prt(m_terminal.out, m_printer);

    // Load Clink's settings.  The load function handles deferred load for
    // settings declared in scripts.
    str<288> settings_file;
    str<288> state_dir;
    app->get_settings_path(settings_file);
    app->get_state_dir(state_dir);
    settings::load(settings_file.c_str());
    reset_keyseq_to_name_map();

    // Set up the string comparison mode.
    static_assert(str_compare_scope::exact == 0, "g_ignore_case values must match str_compare_scope values");
    static_assert(str_compare_scope::caseless == 1, "g_ignore_case values must match str_compare_scope values");
    static_assert(str_compare_scope::relaxed == 2, "g_ignore_case values must match str_compare_scope values");
    str_compare_scope compare(g_ignore_case.get(), g_fuzzy_accent.get());

    // Run clinkstart.cmd on inject, if present.
    static bool s_autostart = true;
    static std::unique_ptr<autostart_display> s_autostart_display;
    str_moveable autostart;
    bool interactive = !m_doskey_alias && ((m_queued_lines.size() == 0) ||
                                           (m_queued_lines.size() == 1 &&
                                            (m_queued_lines.front().length() == 0 ||
                                             m_queued_lines.front().c_str()[m_queued_lines.front().length() - 1] != '\n')));
    if (interactive && s_autostart)
    {
        s_autostart = false;
        app->get_autostart_command(autostart);
        interactive = autostart.empty();
    }

    // Run " echo %ERRORLEVEL% >tmpfile 2>nul" before every interactive prompt.
    static bool s_inspect_errorlevel = true;
    bool inspect_errorlevel = false;
    if (g_get_errorlevel.get())
    {
        if (interactive)
        {
            if (s_inspect_errorlevel)
            {
                inspect_errorlevel = true;
                interactive = false;
            }
            else
            {
                str<> tmp_errfile;
                get_errorlevel_tmp_name(tmp_errfile);

                FILE* f = fopen(tmp_errfile.c_str(), "r");
                if (f)
                {
                    char buffer[32];
                    memset(buffer, sizeof(buffer), 0);
                    fgets(buffer, _countof(buffer) - 1, f);
                    fclose(f);
                    _unlink(tmp_errfile.c_str());
                    os::set_errorlevel(atoi(buffer));
                }
                else
                {
                    os::set_errorlevel(0);
                }
            }
            s_inspect_errorlevel = !s_inspect_errorlevel;
        }
    }
    else
    {
        s_inspect_errorlevel = true;
    }

    // Improve performance while replaying doskey macros by not loading scripts
    // or history, since they aren't used.
    bool init_scripts = reset || interactive;
    bool send_event = interactive;
    bool init_prompt = interactive;
    bool init_editor = interactive;
    bool init_history = reset || (interactive && !rl_has_saved_history());

    // Update last cwd and whether transient prompt can be applied later.
    if (init_editor)
        update_last_cwd();

    // Set up Lua.
    bool local_lua = g_reload_scripts.get();
    bool reload_lua = local_lua || (m_lua && m_lua->is_script_path_changed());
    std::unique_ptr<host_lua> tmp_lua;
    std::unique_ptr<prompt_filter> tmp_prompt_filter;
    if (reload_lua || local_lua)
    {
        delete m_prompt_filter;
        delete m_lua;
        m_prompt_filter = nullptr;
        m_lua = nullptr;
    }
    if (!local_lua)
        init_scripts = !m_lua;
    if (!m_lua)
        m_lua = new host_lua;
    if (!m_prompt_filter)
        m_prompt_filter = new prompt_filter(*m_lua);
    host_lua& lua = *m_lua;
    prompt_filter& prompt_filter = *m_prompt_filter;

    // Load scripts.
    if (init_scripts)
    {
        initialise_lua(lua);
        lua.load_scripts();
    }

    // Send oninject event; one time only.
    static bool s_injected = false;
    if (!s_injected)
    {
        s_injected = true;
        lua.send_event("oninject");
    }

    // Send onbeginedit event.
    if (send_event)
        lua.send_event("onbeginedit");

    // Reset input idle.  Must happen before filtering the prompt, so that the
    // wake event is available.
    if (init_editor || init_prompt)
        static_cast<input_idle*>(lua)->reset();

    line_editor::desc desc(m_terminal.in, m_terminal.out, m_printer, this);
    initialise_editor_desc(desc);
    desc.state_dir = state_dir.c_str();

    // Filter the prompt.  Unless processing a multiline doskey macro.
    if (init_prompt)
    {
        m_prompt = prompt ? prompt : "";
        m_rprompt = rprompt ? rprompt : "";
        desc.prompt = filter_prompt(&desc.rprompt);
    }

    // Create the editor and add components to it.
    line_editor* editor = nullptr;

    if (init_editor)
    {
        editor = line_editor_create(desc);
        editor->add_generator(lua);
        editor->add_generator(file_match_generator());
        if (g_classify_words.get())
            editor->set_classifier(lua);
        editor->set_input_idle(lua);
    }

    if (init_history)
    {
        if (m_history &&
            ((g_save_history.get() != m_history->has_bank(bank_master)) ||
             m_history->is_stale_name()))
        {
            delete m_history;
            m_history = 0;
        }

        if (!m_history)
            m_history = new history_db(g_save_history.get());

        if (m_history)
        {
            m_history->initialise();
            m_history->load_rl_history();
        }
    }

    bool resolved = false;
    bool ret = false;
    while (1)
    {
        // Auto-run clinkstart.cmd the first time the edit prompt is invoked.
        if (autostart.length())
        {
            // Remember the original position to be able to restore the cursor
            // there if the autostart command doesn't output anything.
            s_autostart_display = std::make_unique<autostart_display>();
            s_autostart_display->save();

            m_terminal.out->begin();
            m_terminal.out->end();
            out = autostart.c_str();
            resolved = true;
            ret = true;
            break;
        }

        // Adjust the cursor position if possible, to make the initial prompt
        // appear on the same line it would have if no autostart script ran.
        if (s_autostart_display)
        {
            s_autostart_display->restore();
            s_autostart_display.reset();
        }

        // Before each interactive prompt, run an echo command to interrogate
        // CMD's internal %ERRORLEVEL% variable.
        if (inspect_errorlevel)
        {
            str<> tmp_errfile;
            get_errorlevel_tmp_name(tmp_errfile);

            m_terminal.out->begin();
            m_terminal.out->end();
            out.format(" echo %%errorlevel%% 2>nul >\"%s\"", tmp_errfile.c_str());
            resolved = true;
            ret = true;
            move_cursor_up_one_line();
            break;
        }

        // Give the directory history queue a crack at the current directory.
        update_dir_history();

        // Doskey is implemented on the server side of a ReadConsoleW() call
        // (i.e. in conhost.exe). Commands separated by a "$T" are returned one
        // command at a time through successive calls to ReadConsoleW().
        resolved = false;
        if (m_doskey_alias.next(out))
        {
            m_terminal.out->begin();
            m_terminal.out->end();
            resolved = true;
            ret = true;
        }
        else
        {
            bool edit = true;
            if (!m_queued_lines.empty())
            {
                out.concat(m_queued_lines.front().c_str());
                m_queued_lines.pop_front();

                unsigned int len = out.length();
                while (len && out.c_str()[len - 1] == '\n')
                {
                    out.truncate(--len);
                    edit = false;
                }
            }

            if (!edit)
            {
                char const* read = g_last_prompt.c_str();
                char* write = g_last_prompt.data();
                while (*read)
                {
                    if (*read != 0x01 && *read != 0x02)
                    {
                        *write = *read;
                        ++write;
                    }
                    ++read;
                }
                *write = '\0';

                m_printer->print(g_last_prompt.c_str(), g_last_prompt.length());
                m_printer->print(out.c_str(), out.length());
                m_printer->print("\n");
                ret = true;
            }
            else
            {
                ret = editor && editor->edit(out);
                if (!ret)
                    break;
            }

            // Determine whether to add the line to history.  Must happen before
            // calling expand() because that resets the history position.
            bool add_history = true;
            if (rl_has_saved_history())
            {
                // Don't add to history when operate-and-get-next was used, as
                // that would defeat the command.
                add_history = false;
            }
            else if (!out.empty() && get_sticky_search_history() && has_sticky_search_position())
            {
                // Query whether the sticky search position should be added
                // (i.e. the input line matches the history entry corresponding
                // to the sticky search history position).
                add_history = get_sticky_search_add_history(out.c_str());
            }
            if (add_history)
                clear_sticky_search_position();

            // Handle history event expansion.  expand() is a static method,
            // so can call it even when m_history is nullptr.
            if (m_history->expand(out.c_str(), out) == history_db::expand_print)
            {
                puts(out.c_str());
                continue;
            }

            // Should we skip adding certain commands?
            if (g_exclude_from_history_cmds.get() &&
                *g_exclude_from_history_cmds.get())
            {
                const char* c = out.c_str();
                while (*c == ' ' || *c == '\t')
                    ++c;

                bool exclude = false;
                str<> token;
                str_tokeniser tokens(g_exclude_from_history_cmds.get(), " ,;");
                while (tokens.next(token))
                {
                    if (token.length() &&
                        _strnicmp(c, token.c_str(), token.length()) == 0 &&
                        !isalnum((unsigned char)c[token.length()]) &&
                        !path::is_separator(c[token.length()]))
                    {
                        exclude = true;
                        break;
                    }
                }

                if (exclude)
                    break;
            }

            // Add the line to the history.
            if (add_history)
                m_history->add(out.c_str());
        }
        break;
    }

    if (send_event)
    {
        lua_state& state = lua;
        lua_pushlstring(state.get_state(), out.c_str(), out.length());
        lua.send_event("onendedit", 1);
    }

    if (send_event)
        lua.send_event_cancelable_string_inout("onfilterinput", out.c_str(), out);

    if (!resolved)
    {
        m_doskey.resolve(out.c_str(), m_doskey_alias);
        m_doskey_alias.next(out);
    }

    if (ret && autostart.empty())
    {
        // If the line is a directory, rewrite the line to invoke the CD command
        // to change to the directory.
        intercept_directory(out);
    }

    line_editor_destroy(editor);

    if (local_lua)
    {
        delete m_prompt_filter;
        delete m_lua;
        m_prompt_filter = nullptr;
        m_lua = nullptr;
    }

    m_prompt = nullptr;
    m_rprompt = nullptr;

    return ret;
}

//------------------------------------------------------------------------------
const char* host::filter_prompt(const char** rprompt, bool transient)
{
    m_filtered_prompt.clear();
    m_filtered_rprompt.clear();
    if (g_filter_prompt.get() && m_prompt_filter)
    {
        str_moveable tmp;
        str_moveable rtmp;
        const char* p;
        const char* rp;
        if (transient)
        {
            prompt_utils::get_transient_prompt(tmp);
            prompt_utils::get_transient_rprompt(rtmp);
            p = tmp.c_str();
            rp = rtmp.c_str();
        }
        else
        {
            p = m_prompt ? m_prompt : "";
            rp = m_rprompt ? m_rprompt : "";
        }
        m_prompt_filter->filter(p,
                                rp,
                                m_filtered_prompt,
                                m_filtered_rprompt,
                                transient);
    }
    else
    {
        m_filtered_prompt = m_prompt;
        m_filtered_rprompt = m_rprompt;
    }
    if (rprompt)
        *rprompt = m_filtered_rprompt.length() ? m_filtered_rprompt.c_str() : nullptr;
    return m_filtered_prompt.c_str();
}

//------------------------------------------------------------------------------
void host::purge_old_files()
{
    str<> tmp;
    get_errorlevel_tmp_name(tmp, true/*wild*/);

    // Purge orphaned clink_errorlevel temporary files older than 30 minutes.
    const int seconds = 30 * 60/*seconds per minute*/;

    globber i(tmp.c_str());
    i.older_than(seconds);
    while (i.next(tmp))
        _unlink(tmp.c_str());
}

//------------------------------------------------------------------------------
void host::update_last_cwd()
{
    int when = s_prompt_transient.get();

    str<> cwd;
    os::get_current_dir(cwd);

    wstr_moveable wcwd(cwd.c_str());
    if (wcwd.iequals(m_last_cwd.c_str()))
    {
        m_can_transient = (when != 0);  // Same dir collapses if not 'off'.
    }
    else
    {
        m_can_transient = (when == 1);  // Otherwise only collapse if 'always'.
        m_last_cwd = std::move(wcwd);
    }
}
