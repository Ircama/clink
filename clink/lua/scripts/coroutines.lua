-- Copyright (c) 2021 Christopher Antos
-- License: http://opensource.org/licenses/MIT

--------------------------------------------------------------------------------
clink = clink or {}
local _coroutines = {}
local _coroutines_created = {}          -- Remembers creation info for each coroutine, for use by clink.addcoroutine.
local _after_coroutines = {}            -- Funcs to run after a pass resuming coroutines.
local _coroutines_resumable = false     -- When false, coroutines will no longer run.
local _coroutine_yieldguard = nil       -- Which coroutine is yielding inside popenyield.
local _coroutine_context = nil          -- Context for queuing io.popenyield calls from a same source.
local _coroutine_canceled = false       -- Becomes true if an orphaned io.popenyield cancels the coroutine.
local _coroutine_generation = 0         -- ID for current generation of coroutines.

local _dead = nil

local print = clink.print

--------------------------------------------------------------------------------
-- Scheme for entries in _coroutines:
--
--  Initialized by clink.addcoroutine:
--      coroutine:      The coroutine.
--      func:           The function the coroutine runs.
--      interval:       Interval at which to schedule the coroutine.
--      context:        The context in which the coroutine was created.
--      generation:     The generation to which this coroutine belongs.
--
--  Updated by the coroutine management system:
--      resumed:        Number of times the coroutine has been resumed.
--      firstclock:     The os.clock() from the beginning of the first resume.
--      throttleclock:  The os.clock() from the end of the most recent yieldguard.
--      lastclock:      The os.clock() from the end of the last resume.
--      infinite:       Use INFINITE wait for this coroutine; it's actively inside popenyield.
--      queued:         Use INFINITE wait for this coroutine; it's queued inside popenyield.

--------------------------------------------------------------------------------
local function clear_coroutines()
    local preserve
    if _coroutine_yieldguard then
        -- Preserve the active popenyield entry so the system can tell when to
        -- dequeue the next one.
        preserve = _coroutines[_coroutine_yieldguard.coroutine]
    end

    _coroutines = {}
    _coroutines_created = {}
    _after_coroutines = {}
    _coroutines_resumable = false
    -- Don't touch _coroutine_yieldguard; it only gets cleared when the thread finishes.
    _coroutine_context = nil
    _coroutine_canceled = false
    _coroutine_generation = _coroutine_generation + 1

    _dead = (settings.get("lua.debug") or clink.DEBUG) and {} or nil

    if preserve then
        _coroutines[preserve.coroutine] = preserve
    end
end
clink.onbeginedit(clear_coroutines)

--------------------------------------------------------------------------------
local function release_coroutine_yieldguard()
    if _coroutine_yieldguard and _coroutine_yieldguard.yieldguard:ready() then
        local entry = _coroutines[_coroutine_yieldguard.coroutine]
        if entry and entry.yieldguard == _coroutine_yieldguard.yieldguard then
            entry.throttleclock = os.clock()
            entry.yieldguard = nil
            _coroutine_yieldguard = nil
            for _,entry in pairs(_coroutines) do
                if entry.queued then
                    entry.queued = nil
                    break
                end
            end
        end
    end
end

--------------------------------------------------------------------------------
local function get_coroutine_generation()
    local t = coroutine.running()
    if t and _coroutines[t] then
        return _coroutines[t].generation
    end
end

--------------------------------------------------------------------------------
local function set_coroutine_yieldguard(yieldguard)
    local t = coroutine.running()
    if yieldguard then
        _coroutine_yieldguard = { coroutine=t, yieldguard=yieldguard }
    else
        release_coroutine_yieldguard()
    end
    if t and _coroutines[t] then
        _coroutines[t].yieldguard = yieldguard
    end
end

--------------------------------------------------------------------------------
local function set_coroutine_queued(queued)
    local t = coroutine.running()
    if t and _coroutines[t] then
        _coroutines[t].queued = queued and true or nil
    end
end

--------------------------------------------------------------------------------
local function cancel_coroutine(message)
    _coroutine_canceled = true
    error(message.."canceling popenyield; coroutine is orphaned")
end

--------------------------------------------------------------------------------
local function next_entry_target(entry, now)
    if not entry.lastclock then
        return 0
    else
        -- Multiple kinds of throttling for coroutines:
        --      - Throttle if running for 5 or more seconds and wants to run
        --        more frequently than every 5 seconds.  But reset the elapsed
        --        time every time io.popenyield() finishes.
        --      - Throttle if running for more than 30 seconds total.
        -- Throttled coroutines can only run once every 5 seconds.
        local interval = entry.interval
        local throttleclock = entry.throttleclock or entry.firstclock
        if now then
            if interval < 5 and throttleclock and now - throttleclock > 5 then
                interval = 5
            elseif entry.firstclock and now - entry.firstclock > 30 then
                interval = 5
            end
        end
        return entry.lastclock + interval
    end
end

--------------------------------------------------------------------------------
function clink._after_coroutines(func)
    if type(func) ~= "function" then
        error("bad argument #1 (function expected)")
    end
    _after_coroutines[func] = func      -- Prevent duplicates.
end

--------------------------------------------------------------------------------
function clink._has_coroutines()
    return _coroutines_resumable
end

--------------------------------------------------------------------------------
function clink._wait_duration()
    if _coroutines_resumable then
        local target
        local now = os.clock()
        release_coroutine_yieldguard()  -- Dequeue next if necessary.
        for _,entry in pairs(_coroutines) do
            local this_target = next_entry_target(entry, now)
            if entry.yieldguard or entry.queued then
                -- Yield until output is ready; don't influence the timeout.
            elseif not target or target > this_target then
                target = this_target
            end
        end
        if target then
            return target - now
        end
    end
end

--------------------------------------------------------------------------------
function clink._set_coroutine_context(context)
    _coroutine_context = context
    _coroutine_canceled = false
end

--------------------------------------------------------------------------------
function clink._resume_coroutines()
    if not _coroutines_resumable then
        return
    end

    -- Protected call to resume coroutines.
    local remove = {}
    local impl = function()
        for _,entry in pairs(_coroutines) do
            if coroutine.status(entry.coroutine) == "dead" then
                table.insert(remove, _)
            else
                _coroutines_resumable = true
                local now = os.clock()
                if next_entry_target(entry, now) <= now then
                    if not entry.firstclock then
                        entry.firstclock = now
                    end
                    entry.resumed = entry.resumed + 1
                    clink._set_coroutine_context(entry.context)
                    local ok, ret = coroutine.resume(entry.coroutine, true--[[async]])
                    if ok then
                        -- Use live clock so the interval excludes the execution
                        -- time of the coroutine.
                        entry.lastclock = os.clock()
                    else
                        if _coroutine_canceled then
                            entry.canceled = true
                        else
                            print("")
                            print("coroutine failed:")
                            print(ret)
                            entry.error = ret
                        end
                        table.insert(remove, _)
                    end
                end
            end
        end
    end

    -- Prepare.
    _coroutines_resumable = false
    clink._set_coroutine_context(nil)

    -- Protected call.
    local ok, ret = xpcall(impl, _error_handler_ret)
    if not ok then
        print("")
        print("coroutine failed:")
        print(ret)
        -- Don't return yet!  Need to do cleanup.
    end

    -- Cleanup.
    clink._set_coroutine_context(nil)
    for _,c in ipairs(remove) do
        clink.removecoroutine(c)
    end
    for _,func in pairs(_after_coroutines) do
        func()
    end
end

--------------------------------------------------------------------------------
local function str_rpad(s, width, pad)
    if width <= #s then
        return s
    end
    return s..string.rep(pad or " ", width - #s)
end

--------------------------------------------------------------------------------
local function table_has_elements(t)
    if t then
        for _ in pairs(t) do
            return true
        end
    end
end

--------------------------------------------------------------------------------
function clink._diag_coroutines()
    local bold = "\x1b[1m"          -- Bold (bright).
    local norm = "\x1b[m"           -- Normal.
    local red = "\x1b[31m"          -- Red.
    local yellow = "\x1b[33m"       -- Yellow.
    local green = "\x1b[32m"        -- Green.
    local cyan = "\x1b[36m"         -- Cyan.
    local statcolor = "\x1b[35m"    -- Magenta.
    local deadlistcolor = "\x1b[90m" -- Bright black.

    local mixed_gen = false
    local show_gen = false
    local threads = {}
    local deadthreads = {}
    local max_resumed_len = 0
    local max_freq_len = 0

    local function collect_diag(list, threads)
        for _,entry in pairs(list) do
            local resumed = tostring(entry.resumed)
            local status = coroutine.status(entry.coroutine)
            local freq = tostring(entry.interval)
            if max_resumed_len < #resumed then
                max_resumed_len = #resumed
            end
            if max_freq_len < #freq then
                max_freq_len = #freq
            end
            if entry.generation ~= _coroutine_generation then
                if list == _coroutines then
                    mixed_gen = true
                end
                show_gen = true
            end
            table.insert(threads, { entry=entry, status=status, resumed=resumed, freq=freq })
        end
    end

    local function list_diag(threads, plain)
        for _,t in ipairs(threads) do
            local key = tostring(t.entry.coroutine):gsub("thread: ", "")..":"
            local gen = (t.entry.generation == _coroutine_generation) and "" or (yellow.."gen "..t.entry.generation..plain.."  ")
            local status = (t.status == "suspended") and "" or (statcolor..t.status..plain.."  ")
            if t.entry.error then
                gen = red.."error"..plain.."  "..gen
            end
            if t.entry.yieldguard then
                status = status..yellow.."yieldguard"..plain.."  "
            end
            if t.entry.queued then
                status = status..yellow.."queued"..plain.."  "
            end
            if t.entry.canceled then
                status = status..cyan.."canceled"..plain.."  "
            end
            local res = "resumed "..str_rpad(t.resumed, max_resumed_len)
            local freq = "freq "..str_rpad(t.freq, max_freq_len)
            local src = t.entry.src
            print(plain.."  "..key.."  "..gen..status..res.."  "..freq.."  "..src..norm)
            if t.entry.error then
                print(plain.."  "..str_rpad("", #key + 2)..red..t.entry.error..norm)
            end
        end
    end

    collect_diag(_coroutines, threads)
    if _dead then
        collect_diag(_dead, deadthreads)
    end

    -- Only list coroutines if there are any, or if there's unfinished state.
    if table_has_elements(threads) or _coroutines_resumable or _coroutine_yieldguard then
        clink.print(bold.."coroutines:"..norm)
        if show_gen then
            print("  generation", (mixed_gen and yellow or norm).."gen ".._coroutine_generation..norm)
        end
        print("  resumable", _coroutines_resumable)
        print("  wait_duration", clink._wait_duration())
        if _coroutine_yieldguard then
            local yg = _coroutine_yieldguard.yieldguard
            print("  yieldguard", (yg:ready() and green.."ready"..norm or yellow.."yield"..norm))
            print("  yieldcommand", '"'..yg:command()..'"')
        end
        list_diag(threads, norm)
    end

    -- Only list dead coroutines if there are any.
    if table_has_elements(_dead) then
        clink.print(bold.."dead coroutines:"..norm)
        list_diag(deadthreads, deadlistcolor)
    end
end

--------------------------------------------------------------------------------
function clink.addcoroutine(coroutine, interval)
    if type(coroutine) ~= "thread" then
        error("bad argument #1 (coroutine expected)")
    end
    if interval ~= nil and type(interval) ~= "number" then
        error("bad argument #2 (number or nil expected)")
    end
    local created_info = _coroutines_created[coroutine] or {}
    _coroutines[coroutine] = {
        coroutine=coroutine,
        interval=interval or 0,
        resumed=0,
        func=created_info.func,
        context=created_info.context,
        generation=created_info.generation,
        src=created_info.src
    }
    _coroutines_created[coroutine] = nil
    _coroutines_resumable = true
end

--------------------------------------------------------------------------------
function clink.removecoroutine(coroutine)
    if type(coroutine) == "thread" then
        release_coroutine_yieldguard()
        if _dead then
            table.insert(_dead, _coroutines[coroutine])
        end
        _coroutines[coroutine] = nil
        _coroutines_resumable = false
        for _ in pairs(_coroutines) do
            _coroutines_resumable = true
            break
        end
    elseif coroutine ~= nil then
        error("bad argument #1 (coroutine expected)")
    end
end

--------------------------------------------------------------------------------
--- -name:  io.popenyield
--- -arg:   command:string
--- -arg:   [mode:string]
--- -ret:   file
--- -show:  local file = io.popenyield("git status")
--- -show:
--- -show:  while (true) do
--- -show:  &nbsp; local line = file:read("*line")
--- -show:  &nbsp; if not line then
--- -show:  &nbsp;   break
--- -show:  &nbsp; end
--- -show:  &nbsp; do_things_with(line)
--- -show:  end
--- -show:  file:close()
--- This is the same as
--- <code><span class="hljs-built_in">io</span>.<span class="hljs-built_in">popen</span>(<span class="arg">command</span>, <span class="arg">mode</span>)</code>
--- except that it only supports read mode and it yields until the command has
--- finished:
---
--- Runs <span class="arg">command</span> and returns a read file handle for
--- reading output from the command.  It yields until the command has finished
--- and the complete output is ready to be read without blocking.
---
--- The <span class="arg">mode</span> can contain "r" (read mode) and/or either
--- "t" for text mode (the default if omitted) or "b" for binary mode.  Write
--- mode is not supported, so it cannot contain "w".
---
--- <strong>Note:</strong> if the <code>prompt.async</code> setting is disabled,
--- or while a <a href="transientprompts">transient prompt filter</a> is
--- executing, then this behaves like
--- <code><span class="hljs-built_in">io</span>.<span class="hljs-built_in">popen</span>(<span class="arg">command</span>, <span class="arg">mode</span>)</code>
--- instead.
function io.popenyield(command, mode)
    -- This outer wrapper is implemented in Lua so that it can yield.
    if settings.get("prompt.async") and not clink.istransientpromptfilter() then
        -- Yield to ensure only one popenyield active at a time.
        if _coroutine_yieldguard then
            set_coroutine_queued(true)
            while _coroutine_yieldguard do
                coroutine.yield()
            end
            set_coroutine_queued(false)
        end
        -- Cancel if not from the current prompt filter generation.
        if get_coroutine_generation() ~= _coroutine_generation then
            local message = (type(command) == string) and command..": " or ""
            cancel_coroutine(message.."canceling popenyield; coroutine is orphaned")
        end
        -- Start the popenyield.
        local file, yieldguard = io.popenyield_internal(command, mode)
        if file and yieldguard then
            set_coroutine_yieldguard(yieldguard)
            while not yieldguard:ready() do
                coroutine.yield()
            end
            set_coroutine_yieldguard(nil)
        end
        return file
    else
        return io.popen(command, mode)
    end
end

--------------------------------------------------------------------------------
local override_coroutine_src_func = nil
function coroutine.override_src(func)
    override_coroutine_src_func = func
end

--------------------------------------------------------------------------------
local orig_coroutine_create = coroutine.create
function coroutine.create(func)
    -- Get src of func.
    local src = override_coroutine_src_func or func
    if src then
        local info = debug.getinfo(src, 'S')
        src = info.short_src..":"..info.linedefined
    else
        src = "<unknown>"
    end
    override_coroutine_src_func = nil

    -- Remember original func for diagnostic purposes later.  The table is
    -- cleared at the beginning of each input line.
    local thread = orig_coroutine_create(func)
    _coroutines_created[thread] = { func=func, context=_coroutine_context, generation=_coroutine_generation, src=src }
    return thread
end
