The [examples](examples) folder includes a number of LUA extensions to customize the [clink](https://github.com/chrisant996/clink) behaviour.

The following notes help to configure and install these scripts.

_________________

## [ex_async_prompt.lua](examples/ex_async_prompt.lua): Git Status extension

- Status: `git status` plugin, ready to install.
- Description: If the local directory is inside a GIT repository, this script shows the branch name closed in square brakets and colored in green if up to date, or yellow if there are changes.
- Installation: This script is a working plugin and can be installed by simply moving it to one of the allowed directories to run LUA scripts, like `C:\Users\%USERNAME%\AppData\Local\clink` (check with `clink info`).
- Prerequisite: GIT needs to be installed (e.g., from [Git-scm](https://git-scm.com/download/win) or using the [Git for Windows](https://gitforwindows.org/) installer).

  To check that prerequisites are correctly installed, change directory to a folder including a git repository (root folder or subfolders) and run the following command:

  ```cmd
  git --no-optional-locks status --porcelain
  ```

  This command shall produce valid results. In case of errors, upgrading GIT is needed.

_________________

## [ex_classify_samp.lua](examples/ex_classify_samp.lua)

to be completed

- Status:
- Description:
- Installation:
- Prerequisite:
_________________

## [ex_cmd_sep.lua](examples/ex_cmd_sep.lua): separators and redirection symbol extension

- Status: ready to install.
- Description: this script applies colors to command separators and redirection symbols in the input line. Matched characters are `|`, `<`, `>`, `>&`. These characters are colored in magenta when not included within double quotes. 
- Installation: working plugin which can be installed by simply moving it to one of the allowed directories to run LUA scripts, like `C:\Users\%USERNAME%\AppData\Local\clink` (check with `clink info`).
- Prerequisite: none.
_________________

## [ex_findline.lua](examples/ex_findline.lua):
_________________

## [ex_findprompt.lua](examples/ex_findprompt.lua):
_________________

## [ex_findstr.lua](examples/ex_findstr.lua):
_________________

## [ex_fzf.lua](examples/ex_fzf.lua):

- Status: ready to install.
- Description:
- Installation: This script is a working plugin and can be installed by simply moving it to one of the allowed directories to run LUA scripts, like `C:\Users\%USERNAME%\AppData\Local\clink` (check with `clink info`).
- Configuration:
- Prerequisite: none.
_________________

## [ex_generate.lua](examples/ex_generate.lua):
_________________

## [ex_prompt.lua](examples/ex_prompt.lua):

- Status:
- Description:
- Installation: after configuration, it can be installed by simply moving it to one of the allowed directories to run LUA scripts.
- Configuration:
- Prerequisite:
_________________

to be completed

## [ex_right_prompt.lua](examples/ex_right_prompt.lua):

- Status: ready to install.
- Description:
- Installation: working plugin which can be installed by simply moving it to one of the allowed directories to run LUA scripts, like `C:\Users\%USERNAME%\AppData\Local\clink` (check with `clink info`).
- Configuration:
- Prerequisite: none.
