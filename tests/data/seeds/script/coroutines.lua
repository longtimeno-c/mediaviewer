local co = coroutine.wrap(function(a) for i = 1, 3 do a = coroutine.yield(a + i) end end)
local x = co(1)
mv.on("e", function(q) return q end)
function f() pcall(error, {code = 1}) return load("return 1")() + x end
