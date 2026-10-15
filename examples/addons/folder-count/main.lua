-- SPDX-License-Identifier: MIT
-- Folder Count: the example script add-on (docs/ADDONS.md, "Scripts").
--
-- MediaViewer runs this file once when the add-on starts, on a worker of its
-- own. The command in addon.json ("Count files", Ctrl+Shift+K, Cmd+Shift+K on a
-- Mac) calls count_files() each time its key is pressed. mv.folder is here
-- only because addon.json asks for "folder.read"; without it, mv.folder is nil.

function count_files()
  local n = mv.folder.count()
  if n == 0 then
    mv.notify("No files open")
  elseif n == 1 then
    mv.notify("1 file in this folder")
  else
    mv.notify(n .. " files in this folder")
  end
end
