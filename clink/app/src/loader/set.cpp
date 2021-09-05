// Copyright (c) 2013 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "host/host_lua.h"
#include "utils/app_context.h"

#include <core/base.h>
#include <core/path.h>
#include <core/settings.h>
#include <core/str.h>
#include <core/str_tokeniser.h>
#include <lua/lua_script_loader.h>
#include <lua/prompt.h>

#include <getopt.h>

//------------------------------------------------------------------------------
void puts_help(const char* const* help_pairs, const char* const* other_pairs=nullptr);

//------------------------------------------------------------------------------
static void list_keys()
{
    for (auto iter = settings::first(); auto* next = iter.next();)
        puts(next->get_name());
}

//------------------------------------------------------------------------------
static void list_options(const char* key)
{
    const setting* setting = settings::find(key);
    if (setting == nullptr)
        return;

    switch (setting->get_type())
    {
    case setting::type_int:
    case setting::type_string:
        break;

    case setting::type_bool:
        puts("true");
        puts("false");
        break;

    case setting::type_enum:
        {
            const char* options = ((const setting_enum*)setting)->get_options();
            str_tokeniser tokens(options, ",");
            const char* start;
            int length;
            while (tokens.next(start, length))
                printf("%.*s\n", length, start);
        }
        break;

    case setting::type_color:
        {
            static const char* const color_keywords[] =
            {
                "bold", "nobold", "underline", "nounderline",
                "bright", "default", "normal", "on",
                "black", "red", "green", "yellow",
                "blue", "cyan", "magenta", "white",
                "sgr",
            };

            for (auto keyword : color_keywords)
                puts(keyword);
        }
        break;
    }

    puts("clear");
}

//------------------------------------------------------------------------------
static bool print_keys(const char* prefix=nullptr)
{
    size_t prefix_len = prefix ? strlen(prefix) : 0;

    int longest = 0;
    for (auto iter = settings::first(); auto* next = iter.next();)
    {
        if (!prefix || !_strnicmp(next->get_name(), prefix, prefix_len))
            longest = max(longest, int(strlen(next->get_name())));
    }

    for (auto iter = settings::first(); auto* next = iter.next();)
    {
        if (!prefix || !_strnicmp(next->get_name(), prefix, prefix_len))
        {
            str<> value;
            next->get_descriptive(value);
            const char* name = next->get_name();
            printf("%-*s  %s\n", longest, name, value.c_str());
        }
    }

    return true;
}

//------------------------------------------------------------------------------
static bool print_value(const char* key)
{
    size_t key_len = strlen(key);
    if (key_len && key[key_len - 1] == '*')
    {
        str<> prefix(key);
        prefix.truncate(prefix.length() - 1);
        return print_keys(prefix.c_str());
    }

    const setting* setting = settings::find(key);
    if (setting == nullptr)
    {
        std::vector<settings::setting_name_value> migrated_settings;
        if (migrate_setting(key, nullptr, migrated_settings))
        {
            bool ret = true;
            bool printed = false;
            for (const auto& pair : migrated_settings)
            {
                if (printed)
                    puts("");
                else
                    printed = true;
                ret = print_value(pair.name.c_str()) && ret;
            }
            return ret;
        }

        printf("ERROR: Setting '%s' not found.\n", key);
        return false;
    }

    printf("        Name: %s\n", setting->get_name());
    printf(" Description: %s\n", setting->get_short_desc());

    // Output an enum-type setting's options.
    if (setting->get_type() == setting::type_enum)
        printf("     Options: %s\n", ((setting_enum*)setting)->get_options());
    else if (setting->get_type() == setting::type_color)
        printf("      Syntax: 'sgr SGR_params' or '[underline bright] color on [bright] color'\n");


    str<> value;
    setting->get_descriptive(value);
    printf("       Value: %s\n", value.c_str());

    const char* long_desc = setting->get_long_desc();
    if (long_desc != nullptr && *long_desc)
        printf("\n%s\n", setting->get_long_desc());

    return true;
}

//------------------------------------------------------------------------------
static bool set_value_impl(const char* key, const char* value)
{
    setting* setting = settings::find(key);
    if (setting == nullptr)
    {
        std::vector<settings::setting_name_value> migrated_settings;
        if (migrate_setting(key, value, migrated_settings))
        {
            bool ret = true;
            for (const auto& pair : migrated_settings)
                ret = set_value_impl(pair.name.c_str(), pair.value.c_str()) && ret;
            return ret;
        }

        printf("ERROR: Setting '%s' not found.\n", key);
        return false;
    }

    if (!value)
    {
        setting->set();
    }
    else
    {
        if (!setting->set(value))
        {
            printf("ERROR: Failed to set value '%s'.\n", key);
            return false;
        }
    }

    str<> result;
    setting->get_descriptive(result);
    printf("Setting '%s' %sset to '%s'\n", key, value ? "" : "re", result.c_str());
    return true;
}

//------------------------------------------------------------------------------
static bool set_value(const char* key, char** argv=nullptr, int argc=0)
{
    if (!argc)
        return set_value_impl(key, nullptr);

    str<> value;
    for (int c = argc; c--;)
    {
        if (value.length())
            value << " ";
        value << *argv;
        argv++;
    }

    return set_value_impl(key, value.c_str());
}

//------------------------------------------------------------------------------
static void print_help()
{
    extern void puts_clink_header();

    static const char* const help[] = {
        "setting_name", "Name of the setting whose value is to be set.",
        "value",        "Value to set the setting to.",
        nullptr
    };

    puts_clink_header();
    puts("Usage: set [<setting_name> [clear|<value>]]\n");

    puts_help(help);

    puts("If 'settings_name' is omitted then all settings are listed.  Omit 'value'\n"
        "for more detailed info about a setting and use a value of 'clear' to reset\n"
        "the setting to its default value.\n"
        "\n"
        "If 'setting_name' ends with '*' then it is a prefix, and all settings\n"
        "matching the prefix are listed.");
}

//------------------------------------------------------------------------------
int set(int argc, char** argv)
{
    // Parse command line arguments.
    struct option options[] = {
        { "help", no_argument, nullptr, 'h' },
        { "list", no_argument, nullptr, 'l' },
        {}
    };

    bool complete = false;
    int i;
    while ((i = getopt_long(argc, argv, "+?hl", options, nullptr)) != -1)
    {
        switch (i)
        {
        default:
        case '?':
        case 'h': print_help();     return 0;
        case 'l': complete = true;  break;
        }
    }

    // Load the settings from disk.
    str<280> settings_file;
    app_context::get()->get_settings_path(settings_file);
    settings::load(settings_file.c_str());

    // Load all lua state too as there is settings declared in scripts.  The
    // load function handles deferred load for settings declared in scripts.
    host_lua lua;
    prompt_filter prompt_filter(lua);
    lua_load_script(lua, app, exec);
    lua.load_scripts();

    // List or set Clink's settings.
    if (complete)
    {
        (optind < argc) ? list_options(argv[optind]) : list_keys();
        return 0;
    }

    bool clear = false;
    switch (argc - optind)
    {
    case 0:
        return (print_keys() != true);

    case 1:
        if (!clear)
            return (print_value(argv[1]) != true);
        return print_help(), 0;

    default:
        if (_stricmp(argv[2], "clear") == 0)
        {
            if (set_value(argv[1]))
                return settings::save(settings_file.c_str()), 0;
        }
        else if (set_value(argv[1], argv + 2, argc - 2))
            return settings::save(settings_file.c_str()), 0;
    }

    return 1;
}
