// Copyright (c) 2021 Christopher Antos
// License: http://opensource.org/licenses/MIT

#pragma once

#include <core/base.h>

//------------------------------------------------------------------------------
class printer;
class terminal_out;
extern printer* g_printer;

//------------------------------------------------------------------------------
extern "C" int show_cursor(int visible);
extern "C" void use_host_input_mode(void);
extern "C" void use_clink_input_mode(void);

//------------------------------------------------------------------------------
// Scoped configuration of console mode.
//
// Clear 'processed input' flag so key presses such as Ctrl-C and Ctrl-S aren't
// swallowed.  We also want events about window size changes.
class console_config
{
public:
    console_config(HANDLE handle=nullptr);
    ~console_config();

private:
    const HANDLE    m_handle;
    DWORD           m_prev_mode;
};

//------------------------------------------------------------------------------
class printer_context
{
public:
    printer_context(terminal_out* terminal, printer* printer);
    ~printer_context();

private:
    terminal_out* const m_terminal;
    rollback<printer*> m_rb_printer;
};

