keep = setmetatable({}, {__gc = function() while true do end end})
local w = setmetatable({}, {__mode = "k"})
local keys = {}
for i = 1, 64 do keys[i] = {} end
for i = 1, 63 do w[keys[i]] = keys[i + 1] end
collectgarbage()
function f() setmetatable({}, {__gc = function() mv.notify("gc") end}) collectgarbage() end
