# patc 

A simple patcher for any file 

## File format 

```
@<file_to_patch>
??
<content_to_replace>
??
!!
<replace_text>
!!
```

The `<content_to_replace>` must be matching fully. A file can contain more than 1 patch rule.
If a rule does not match nothing is done. Currently the patcher replaces all occurences of a match.

Replacement options will be supported in the future

## Installation

Just clone the repo and run `make`. This will create an executable `patc`. 

Place the executable somewhere in your path if you want.

To uninstall just `rm` the executable and optionally remove the cloned repo
