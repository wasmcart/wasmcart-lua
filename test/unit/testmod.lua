-- a module loaded via require, to prove the cart module loader works
local M = {}
M.answer = 42
function M.double(n) return n * 2 end
return M
