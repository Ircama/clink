// Copyright (c) 2012 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "host/host_cmd.h"
#include "utils/app_context.h"
#include "utils/seh_scope.h"
#include "version.h"

#include <core/base.h>
#include <core/globber.h>
#include <core/log.h>
#include <core/os.h>
#include <core/path.h>
#include <core/settings.h>
#include <core/str.h>
#include <core/str_tokeniser.h>

//------------------------------------------------------------------------------
static constexpr const char* const c_clink_header =
    "Clink v" CLINK_VERSION_STR "\n"
    "Copyright (c) 2012-2018 Martin Ridgers\n"
    "Portions Copyright (c) 2020-2021 Christopher Antos\n"
    "https://github.com/chrisant996/clink\n"
    ;

static constexpr const char* const c_clink_header_abbr =
    "Clink v" CLINK_VERSION_STR " (https://github.com/chrisant996/clink)\n"
    ;

static setting_enum s_clink_logo(
    "clink.logo",
    "Controls what startup logo to show",
    "The default is 'full' which shows the full copyright logo when Clink is\n"
    "injected.  A value of 'short' shows an abbreviated startup logo with version\n"
    "information.  A value of 'none' omits the startup logo entirely.",
    "none,full,short",
    1);

void puts_clink_header()
{
    puts(c_clink_header);
}



//------------------------------------------------------------------------------
static host* g_host = nullptr;



//------------------------------------------------------------------------------
static void success()
{
    auto app = app_context::get();

    if (app->is_quiet())
        return;

    // Load settings to check if the logo should be abbreviated or omitted.
    str<288> settings_file;
    app->get_settings_path(settings_file);
    settings::load(settings_file.c_str());
    const int logo = s_clink_logo.get();
    if (!logo)
        return;

    // Add a blank line if our logo follows anything else (the goal is to
    // put a blank line after CMD's "Microsoft Windows ..." logo), but don't
    // add a blank line if our logo is at the very top of the window.
    CONSOLE_SCREEN_BUFFER_INFO csbi = { sizeof(csbi) };
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
    {
        if (csbi.dwCursorPosition.Y > 0)
            puts("");
    }

    // Using printf instead of puts ensures there's only one blank line
    // between the header and the subsequent prompt.
    printf("%s", (logo == 2) ? c_clink_header_abbr : c_clink_header);
}

//------------------------------------------------------------------------------
static void failed()
{
    auto app = app_context::get();

    str<280> buffer;
    app->get_state_dir(buffer);
    fprintf(stderr, "Failed to load Clink.\n");
    if (app->is_logging_enabled())
    {
        app->get_log_path(buffer);
        fprintf(stderr, "See log file for details (%s).\n", buffer.c_str());
    }
    else
    {
        fprintf(stderr, "Enable logging for details.\n");
    }
}

//------------------------------------------------------------------------------
static bool get_host_name(str_base& out)
{
    str<MAX_PATH> buffer;
    if (GetModuleFileName(nullptr, buffer.data(), buffer.size()) == buffer.size())
        return false;

    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
        return false;

    return path::get_name(buffer.c_str(), out);
}

//------------------------------------------------------------------------------
static void shutdown_clink()
{
    seh_scope seh;

    if (g_host != nullptr)
    {
        g_host->shutdown();
        delete g_host;
        g_host = nullptr;
    }

    if (logger* logger = logger::get())
        delete logger;

    delete app_context::get();
}

//------------------------------------------------------------------------------
void start_logger()
{
    auto* app_ctx = app_context::get();

    if (app_ctx->is_logging_enabled())
    {
        // Discard any existing logger.  This is so Cmder can be compatible with
        // Clink autorun and still override the scripts and profile paths.
        if (logger::get())
            delete logger::get();

        str<256> log_path;
        app_ctx->get_log_path(log_path);
        unlink(log_path.c_str()); // Restart the log file on every inject.
        new file_logger(log_path.c_str());

        SYSTEMTIME now;
        GetLocalTime(&now);
        LOG("---- %04u/%02u/%02u %02u:%02u:%02u.%03u -------------------------------------------------",
            now.wYear, now.wMonth, now.wDay,
            now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);

        str<64> host_name;
        if (!get_host_name(host_name))
            host_name = "<unknown>";

        LOG("Host process is '%s' (pid %d)", host_name.c_str(), app_ctx->get_id());

        str<> dll_path;
        app_ctx->get_binaries_dir(dll_path);
        LOG("DLL path is '%s'", dll_path.c_str());

        {
#pragma warning(push)
#pragma warning(disable:4996)
            OSVERSIONINFO ver = { sizeof(ver) };
            if (GetVersionEx(&ver))
            {
                SYSTEM_INFO system_info;
                GetNativeSystemInfo(&system_info);
                LOG("Windows version %u.%u.%u (%s)",
                    ver.dwMajorVersion,
                    ver.dwMinorVersion,
                    ver.dwBuildNumber,
                    (system_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) ? "x64" : "x86");
            }
#ifdef _WIN64
            LOG("Clink version %s (x64)", CLINK_VERSION_STR);
#else
            LOG("Clink version %s (x86)", CLINK_VERSION_STR);
#endif
#pragma warning(pop)
        }
    }
}

//------------------------------------------------------------------------------
INT_PTR WINAPI initialise_clink(const app_context::desc& app_desc)
{
    seh_scope seh;

    auto* app_ctx = new app_context(app_desc);

    start_logger();

    // What process is the DLL loaded into?
    str<64> host_name;
    if (app_desc.force)
        host_name = "cmd.exe";
    else if (!get_host_name(host_name))
    {
        ERR("Unable to get host name.");
        return false;
    }

    // Search for a supported host (keep in sync with inject_dll in inject.cpp).
    struct {
        const char* name;
        host*       (*creator)();
    } hosts[] = {
        { "cmd.exe", []() -> host* { return new host_cmd(); } },
    };

    for (int i = 0; i < sizeof_array(hosts); ++i)
        if (stricmp(host_name.c_str(), hosts[i].name) == 0)
            if (g_host = (hosts[i].creator)())
                break;

    // Bail out if this isn't a supported host.
    if (g_host == nullptr)
    {
        LOG("Unknown host '%s'.", host_name.c_str());
        return false;
    }

    // Validate and initialise.  Negative means an ignorable error that should
    // not be reported.
    int validate = g_host->validate();
    if (validate <= 0)
        return validate;

    if (!g_host->initialise())
    {
        failed();
        return false;
    }

    atexit(shutdown_clink);

    success();
    return true;
}
