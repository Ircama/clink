-- Git plugin, providing colored Git status summary information displayed in
-- the first part of the prompt.  Shown data include branch (colored in green if
-- clean, yellow if status is not clean, red if conflict is present, magenta if
-- clean unpublished branch, or default color if status isn't known yet) and
-- tracking (nothing shown if the local branch is at the same commit level as
-- the remote branch, otherwise the number of commits behind (↓) or ahead (↑)
-- of the remote are reported).
-- This plugin is enabled by default and can be disabled with the command
-- "clink set prompt.git false".

local prev_dir      -- Most recent git repo visited.
local prev_info     -- Most recent info retrieved by the coroutine.
settings.add("prompt.git", true, "Boolean setting")

local function get_git_dir(dir)
    -- Check if the current directory is in a git repo.
    local child
    repeat
        if os.isdir(path.join(dir, ".git")) then
            return dir
        end
        -- Walk up one level to the parent directory.
        dir,child = path.toparent(dir)
        -- If child is empty, we've reached the top.
    until (not child or child == "")
    return nil
end

local function get_git_branch()
    -- Get the current git branch name.
    local file = io.popen("git branch --show-current 2>nul")
    local branch = file:read("*a"):match("(.+)\n")
    file:close()
    return branch
end

local function get_git_status()
    -- The io.popenyield API is like io.popen, but it yields until the output is
    -- ready to be read.
    local file = io.popenyield(
        "git -c color.status=always --no-optional-locks status -bs 2>nul")
    local branch = nil
    local unpublished_branch = nil
    local upstream = nil
    local tracking = nil
    local line_number = 0
    local editing = false
    for line in file:lines() do
        line_number = line_number + 1
        if line_number == 1 then  -- parsing the first line
            _, _, branch, upstream, tracking = string.find(
                line, "^## (.+)%.%.%.(%S+) ?%[?([^%]]*)")
            if tracking == "" then tracking = nil end
            if tracking ~= nil then  -- nil means even with remote
                tracking = tracking:gsub(",", ""):
                    gsub("ahead ", "↑"):gsub("behind ", "↓")
            end
            if branch == nil then
                _, _, unpublished_branch = string.find(line, "^## (.+)$")
                if unpublished_branch ~= nil then
                    branch = unpublished_branch
                end
            end
        end
        if branch == nil then break end  -- this means no git repository
        -- any subsequent line means unclean status
        if line_number > 1 and line and line ~= "" then
            editing = true
            break
        end
    end
    file:close()
    return branch, upstream, tracking, editing
end

local function get_git_conflict()
    -- The io.popenyield API is like io.popen, but it yields until the output is
    -- ready to be read.
    local file = io.popenyield("git diff --name-only --diff-filter=U 2>nul")
    local conflict = false
    for line in file:lines() do
        -- If there's any output, there's a conflict.
        conflict = true
        break
    end
    file:close()
    return conflict
end

local function collect_git_info()
    -- This is run inside the coroutine, which happens while idle while waiting
    -- for keyboard input.
    local info = {}
    info.branch, info.upstream, info.tracking, info.status = get_git_status()    
    info.conflict = get_git_conflict()
    -- Until this returns, the call to clink.promptcoroutine() will keep
    -- returning nil.  After this returns, subsequent calls to
    -- clink.promptcoroutine() will keep returning this return value, until a
    -- new input line begins.
    return info
end

local git_prompt = clink.promptfilter(55)
function git_prompt:filter(prompt)
    -- Do nothing if not a git repo.
    if not settings.get("prompt.git") then return end
    local dir = get_git_dir(os.getcwd())
    if not dir then
        return
    end
    -- Reset the cached status if in a different repo.
    if prev_dir ~= dir then
        prev_info = nil
        prev_dir = dir
    end
    -- Do nothing if git branch not available.  Getting the branch name is fast,
    -- so it can run outside the coroutine.  That way the branch name is visible
    -- even while the coroutine is running.
    local branch = get_git_branch()
    if not branch or branch == "" then
        return
    end
    -- Start a coroutine to collect various git info in the background.  The
    -- clink.promptcoroutine() call returns nil immediately, and the
    -- coroutine runs in the background.  After the coroutine finishes, prompt
    -- filtering is triggered again, and subsequent clink.promptcoroutine()
    -- calls from this prompt filter immediately return whatever the
    -- collect_git_info() function returned when it completed.  When a new input
    -- line begins, the coroutine results are reset to nil to allow new results.
    local info = clink.promptcoroutine(collect_git_info)
    -- If no status yet, use the status from the previous prompt.
    if info == nil then
        info = prev_info or {}
        info.status = nil  -- with prev. prompt, reset the color
    else
        prev_info = info
    end
    -- Choose color for the git branch name:  green if status is clean, yellow
    -- if status is not clean, red if conflict is present, magenta if clean
    -- unpublished branch, or default color if status isn't known yet.
    local sgr = "37;1"  -- white
    -- "\x1b[m = reset color
    local tracking = info.tracking and "\x1b[m "..info.tracking or ""
    if info.conflict then
        sgr = "31;1"  -- red
    elseif info.status ~= nil then
        sgr = info.status and "33;1" or "32;1"  -- yellow (unclean) or green
        if info.status == false and info.upstream == nil then
            sgr = "95;1"  -- magenta = clean unpublished branch
        end
    end
    -- Prefix the prompt with "[branch <tracking info>]" using the status color.
    return "\x1b["..sgr.."m["..
            branch..tracking..
            "\x1b["..sgr.."m]\x1b[m  "..
            prompt
end
