function f()
  local n = mv.folder.count()
  mv.notify(n .. " files")
  for i = 1, n do mv.log(mv.folder.name(i)) end
  mv.log(mv.selection.current(), mv.folder.path())
end
