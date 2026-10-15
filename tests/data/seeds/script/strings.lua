local t = {}
for i = 1, 100 do t[i] = string.format("%05d", i) end
table.sort(t, function(a, b) return a > b end)
local s = table.concat(t, ",")
for w in s:gmatch("[^,]+") do t[#t+1] = w:rep(2) end
function f() return utf8.char(72, 105) .. #t end
