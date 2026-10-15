local ok = pcall(function() return getmetatable("").__index end)
local g = load("return io")
local d = string.dump
collectgarbage("count")
function f() return setmetatable({}, {__index = function(t, k) return k end}).x end
