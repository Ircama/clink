// Copyright (c) 2021 Christopher Antos
// License: http://opensource.org/licenses/MIT

#pragma once

//------------------------------------------------------------------------------
class host_callbacks
{
public:
    virtual void add_history(const char* line) = 0;
    virtual void remove_history(int rl_history_index, const char* line) = 0;
    virtual void filter_prompt() = 0;
    virtual void filter_transient_prompt(bool final) = 0;
    virtual void filter_matches(char** matches) = 0;
    virtual bool call_lua_rl_global_function(const char* func_name) = 0;
    virtual const char** copy_dir_history(int* total) = 0;
    virtual void get_app_context(int& id, str_base& binaries, str_base& profile, str_base& scripts) = 0;
};
