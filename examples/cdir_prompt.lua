-- A simple prompt filter that discards any prompt and sets it
-- to the errorlevel code of the last executed command (enclosed
-- in curly brackets), followed by the current working directory.
-- The the errorlevel code is colored in red if not zero.
-- The used color for the working directory is lightcyan.
-- To enable the errorlevel code (disabled by default):
--     clink set prompt.errlevel true
-- To temporarily toggle the display of the errorlevel code, press
--  Ctr-Alt-E; this needs to add the following line in .inputrc:
--      M-C-e: "luafunc:errlevel_change"
-- To disable the colored prompt: "set prompt.colored false".

local err_tbl = {
    [9009] = "Not found",
    -- this allows customization by adding the map of other significant values
    }
settings.add("prompt.errlevel", true, "Boolean setting")
settings.add("prompt.colored", true, "Boolean setting")
local lightcyan = "\x1b[96m"
local red = "\x1b[31m"
local normal = "\x1b[m"
local cwd_prompt = clink.promptfilter(30)

function cwd_prompt:filter(prompt)
    local colored_active = settings.get("prompt.colored")
    local errlevel_active = settings.get("prompt.errlevel")
    if global_errlevel ~= nil and errlevel_active == old_errlevel_active then
        old_errlevel_active = errlevel_active
        errlevel_active = global_errlevel
    else
        old_errlevel_active = errlevel_active
        global_errlevel = errlevel_active
    end
    local errlevel = os.geterrorlevel()
    local errcode = err_tbl[errlevel]
    if errcode == nil then
        errcode = tostring(errlevel)
    end
    local errcolor = errlevel == 0 and normal or red
    local colored_prompt = lightcyan..os.getcwd()..">"..normal
    if not colored_active then colored_prompt = os.getcwd()..">" end
    if errlevel_active then
        return errcolor.."{"..errcode.."} "..normal..colored_prompt
    end
    return colored_prompt
end

-- Sample key binding in .inputrc:
--      M-C-e: "luafunc:errlevel_change"
function errlevel_change(rl_buffer)
    rl_buffer:beginoutput()
    if global_errlevel then
        global_errlevel = false
        print("ErrorLevel temporarily disabled. Press Enter to check.")
    else
        global_errlevel = true
        print("ErrorLevel temporarily enabled. Press Enter to check.")
    end
end
