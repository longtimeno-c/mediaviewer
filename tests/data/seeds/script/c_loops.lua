local t = setmetatable({}, {__len = function() return math.maxinteger - 1 end})
pcall(table.insert, t, 1, 0)
pcall(table.remove, t, 1)
local u = setmetatable({}, {__len = function() return 2^31 - 2 end})
pcall(table.sort, u, rawequal)
function f()
  local s = string.rep("a", 4096)
  return s:find(string.rep("a", 2048) .. "b", 1, true), s:find(string.rep("a", 2048) .. "b")
end
