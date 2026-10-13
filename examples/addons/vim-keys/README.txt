Vim Keys, an example key layout for MediaViewer (docs/ADDONS.md, "Key layouts").

Each command's keys go, in turn, to that command's rows in MediaViewer's default
table. Previous has the rows Left, A, Backspace, ...: ["Left", "H"] keeps Left and
puts H where A was. Next keeps Right and puts L where D was. Toggle mark keeps
Insert and puts X where Shift+Space was.

A key the layout gives out is taken from whatever held it in the same modes, so
H no longer flips the photo and L no longer jumps forward in a clip; Settings
shows those as unbound. Reset to default puts every default key back.

An add-on is its manifest plus the files it lists, and it must list at least one
file: this one.
