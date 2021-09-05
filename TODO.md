_This todo list describes ChrisAnt996's current intended roadmap for Clink's future.  It is a living document and does not convey any guarantee about when or whether any item may be implemented._

<br/>

# IMPROVEMENTS

## High Priority
- Some way to read console input.
  - The intended scenario is a configuration wizard for a Lua script.
  - What about <kbd>Ctrl</kbd>+<kbd>Break</kbd>?

## Medium Priority

## Low Priority

## Tests

<br/>
<br/>

# INVESTIGATE

**Additional ANSI escape codes?**
- `ESC[?47h` save screen.
- `ESC[?47l` restore screen.
- `ESC[?1049h` enable alternative buffer.
- `ESC[?1049l` disable alternative buffer.

**Popup Lists**
- Ability to delete, rearrange, and edit popup list items?  _[Can't realistically rearrange or edit history, due to how the history file format works.]_
- Show the current incremental search string somewhere?
- Completions:
  - What to do about completion colors?
  - Make it owner draw and add text like "dir", "alias", etc?
  - Add stat chars when so configured?

**Installer**
- Why does it install to a versioned path?  The .nsi file says `; Install to a versioned folder to reduce interference between versions.` so use caution when making any change there.

**Miscellaneous**
- Is it a problem that `update_internal()` gets called once per char in a key sequence?  Maybe it should only happen after a key that finishes a key binding?
- Include `wildmatch()` and an `fnmatch()` wrapper for it.  But should first update it to support UTF8.

<br/>
<br/>

# LOW LIKELIHOOD

- Make scrolling key bindings work at the pager prompt.  Note that it would need to revise how the scroll routines identify the bottom line (currently they use Readline's bottom line, but the pager displays output past that point).  _[Low value; also, Windows Terminal has scrolling hotkeys that supersede Clink, and it can scroll regardless whether prompting for input.]_
- `LOG()` certain important failure information inside Detours.  _[Low value; also, I want to avoid making any changes inside Detours.]_
- Provide API to show an input box?  But make it fail if used from outside a "luafunc:" macro.  _[Questionable usage pattern; just make the "luafunc:" macro invoke a standalone program (or even standalone Lua script) that can accept input however it likes.]_
- Provide API to set Readline key binding?  _[Convenient, but also makes it very easy for third party scripts to override a user's explicit configuration choices.  In addition to that being a bit overly powerful, I want to avoid support requests caused by third party macros overriding user configuration.]_
- Classify queued input lines?  _[Low value, high cost; the module layer knows about coloring, but queued lines are handled by the host layer without ever reaching the module layer.]_
- `magic-space` () Perform history expansion on the current line and insert a space? _[Low value, low reliability, niche audience.]_

<br/>
<br/>

# MAINTENANCE

- Readline 8.1 has slight bug in `update_line`; type `c` then `l`, and it now identifies **2** chars (`cl`) as needing to be displayed; seems like the diff routine has a bug with respect to the new faces capability; it used to only identify `l` as needing to be displayed.

## Remove match type changes from Readline?
- Displaying matches was slow because Readline writes everything one byte at a time, which incurs significant processing overhead across several layers.
- Adding a new `rl_completion_display_matches_func` and `display_matches()` resolved the performance satisfactorily.
- It's now almost possible to revert the changes to feed Readline match type information.  It's only used when displaying matches, and when inserting a match to decide whether to append a path separator.  Could just add a callback for inserting.
  - The insertion hook could avoid appending a space when inserting a flag/arg that ends in `:` or `=`.
  - And address the sorting problem, and then the match_type stuff could be removed from Readline itself (though Chet may want its performance benefits).
  - And THEN individual matches could have arbitrary values associated -- color, append char, or any per-match data that's desired.
  - But the hard part is handling duplicates (especially with different match types).  Could maybe pass the index in the matches array, but that requires tighter interdependence between Readline and its host.
- But there's a hurdle:
  - Readline sorts the matches, making it difficult to translate the sorted index to the unsorted list held by `matches_impl`.  Could use a binary search, but using binary search inside a sort comparator makes the algorithmic complexity O(N * logN * logN).  Caching lookup results prior to searching yields O((N * logN) + (N * logN)), but it's still a big increase in total duration.
- **SO:** The best compromise might be to embed the original array index at the start of each match, and use a callback to retrieve host data for each match.  It's still invasive to Readline, but at least Readline doesn't need to know any new implementation details such as match types.

<br/>
<br/>

# BACKLOG

<br/>
<br/>

# APPENDICES

## Manual Test Verifications
- <kbd>Alt</kbd>+<kbd>Up</kbd> to scroll up a line.
- `git add ` in Cmder.
- `git checkout `<kbd>Alt</kbd>+<kbd>=</kbd> in Cmder.

## Known Issues
- Perturbed PROMPT envvar is visible in child processes (e.g. piped shell in various file editors).
- [#531](https://github.com/mridgers/clink/issues/531) AV detects a trojan on download _[This is likely because of the use of CreateRemoteThread and/or hooking OS APIs.  There might be a way to obfuscate the fact that clink uses those, but ultimately this is kind of an inherent problem.  Getting the binaries digitally signed might be the most effective solution, but that's financially expensive.]_
- [Terminal #10191](https://github.com/microsoft/terminal/issues/10191#issuecomment-897345862) Microsoft Terminal does not allow a console application to know about or access the scrollback history, nor to scroll the screen.  Hopefully it's an oversight.  It blocks Clink's scrolling commands, and also the `console.findline()` function and everything else that relies on access to the scrollback history.

## Mystery
- Is session history not getting reaped correctly in certain cases?  Maybe during a compact?  I think some commands disappear from the history unexpectedly sometimes, but maybe it's correct and I overlooked the catalyst?
- AutoRun, `cmd.exe`, `cmd echo hello`, `exit` => the `cmd echo hello` is not in the history. _[NOT REPRO.]_
- Windows 10.0.19042.630 seems to have problems when using WriteConsoleW with ANSI escape codes in a powerline prompt in a git repo.  But Windows 10.0.19041.630 doesn't.
- Windows Terminal crashes on exit after `clink inject`.  The current release version was crashing (1.6.10571.0).  Older versions don't crash, and a locally built version from the terminal repo's HEAD doesn't crash.  I think the crash is probably a bug in Windows Terminal, not related to Clink.  And after I built it locally, then it stopped crashing with 1.6.10571.0 as well.  Mysterious...
- Corrupted clink_history -- not sure how, when, or why -- but after having made changes to history, debugging through issues, and aborting some debugging sessions my clink_history file had a big chunk of contiguous NUL bytes. _[UPDATE: the good news is it isn't a Clink issue; the bad news is the SSD drives in my new Alienware m15 R4 keep periodically hitting a BSOD for KERNEL DATA INPAGE ERROR, which zeroes out recently written sectors.]_

## Punt
- Marking mode in-app is not realistic:
  - Windows Terminal is essentially dropping support for console APIs that read/write the screen buffer, particularly the scrollback buffer.
  - Different terminal hosts have different capabilities and limitations, so building a marking mode that behaves reasonably across all/most terminal hosts isn't feasible.
  - One of the big opportunities for terminal hosts is to provide enhancements to marking and copy/paste.
  - So it seems best to leave marking and copy/paste as something for terminal hosts to provide.
- Lua API to copy text to clipboard (plain text or HTML) is not realistic, for the same technical reasons as for marking mode.
- Would be nice to complete "%ENVVAR%\*" by internally expanding ENVVAR for collecting matches, but not expanding it in the editing line.  However, it's difficult to make that work reasonably in conjunction with path normalization.
- [ConsoleZ](https://github.com/cbucher/console) sometimes draws the prompt in the wrong color:  scroll up, then type => the prompt is drawn in the input color instead of in the default color.  It doesn't happen in conhost or ConEmu or Windows Terminal.  Debugging indicates Clink is _not_ redrawing the prompt, so it's entirely an internal issue inside ConsoleZ.
- Max input line length:
  - CMD has a max input buffer size of 8192 WCHARs including the NUL terminator.
  - ReadConsole does not allow more input than can fit in the input buffer size.
  - Readline allows infinite input size, and has no way to limit it.
  - There is a truncation problem here that does not exist without Clink.
  - However, even CMD itself silently fails to run an inputted command over 8100 characters, despite allowing 8191 characters to be input.
  - So I'm comfortable punting this for now.
- A way to disable/enable clink once injected.  _[Why?]_
- Provide API to generate HTML string from console text.  _[Too complicated; also impossible to support more than 4-bit color.]_
- [#486](https://github.com/mridgers/clink/issues/486) **Ctrl+C** doesn't always work properly _[Unrelated to Clink; the exact same behavior occurs with plain cmd.exe]_

---
Chris Antos - sparrowhawk996@gmail.com
