// Lua on gpu_vm: the interpreter with its base, coroutine, string, table and utf8 libraries runs a script of
// recursion, closures, tables and sorting, string patterns, coroutines, errors caught by pcall (longjmp),
// metatables, integer arithmetic and goto. Integers and strings only are printed (the runtime's %g is not libc's).
#include <stdio.h>
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

static const char *script =
    "local function fib(n) if n < 2 then return n end return fib(n-1) + fib(n-2) end\n"
    "print('fib', fib(22))\n"
    "local t = {}\n"
    "for i = 1, 200 do t[i] = (i * 7919) % 1009 end\n"
    "table.sort(t, function(a, b) return a > b end)\n"
    "print('sorted', t[1], t[100], t[200], #t)\n"
    "local s = string.rep('gpu_vm ', 5) .. 'bundle'\n"
    "print(s:gsub('gpu_vm', 'GVM'))\n"
    "print(string.format('%5d|%-5d|%x|%s', 42, 7, 255, 'ok'))\n"
    "print(s:find('bun'), s:match('(%a+)_(%a+)'), #s, s:upper():sub(1, 6))\n"
    "local words = {}\n"
    "for w in ('the quick brown fox jumps'):gmatch('%a+') do words[#words + 1] = w:reverse() end\n"
    "print(table.concat(words, ','))\n"
    "local function counter() local c = 0 return function() c = c + 1 return c end end\n"
    "local c1, c2 = counter(), counter()\n"
    "c1() c1()\n"
    "print('closures', c1(), c2())\n"
    "local gen = coroutine.wrap(function() for i = 1, 5 do coroutine.yield(i * i) end return 'done' end)\n"
    "local acc = {}\n"
    "for i = 1, 6 do acc[i] = tostring(gen()) end\n"
    "print('coroutine', table.concat(acc, ' '))\n"
    "local ok, err = pcall(function() error({code = 42}) end)\n"
    "print('pcall', ok, err.code)\n"
    "local ok2, err2 = pcall(function() local x = nil; return x.field end)\n"
    "print('pcall2', ok2, (err2:gsub('^.-:%d+: ', '')))\n"
    "local V = {}\n"
    "V.__index = V\n"
    "V.__add = function(a, b) return setmetatable({x = a.x + b.x, y = a.y + b.y}, V) end\n"
    "V.__tostring = function(v) return '(' .. v.x .. ',' .. v.y .. ')' end\n"
    "local v = setmetatable({x = 1, y = 2}, V) + setmetatable({x = 30, y = 40}, V)\n"
    "print('meta', tostring(v))\n"
    "print('ints', 7 // 2, -7 // 2, 7 % -3, 1 << 40, 0xff ~ 0x0f, 0x7fffffffffffffff // 3)\n"
    "local n = 0\n"
    "::top:: n = n + 1 if n < 10 then goto top end\n"
    "print('goto', n)\n"
    "local u = utf8.char(72, 233, 8364, 128512)\n"
    "print('utf8', #u, utf8.len(u))\n"
    "local big = {}\n"
    "for i = 1, 3000 do big['k' .. i] = i end\n"
    "local sum = 0\n"
    "for k, val in pairs(big) do sum = sum + val end\n"
    "print('hash', sum)\n"
    "collectgarbage()\n"
    "print('gc', collectgarbage('count') > 0)\n";

int main(void) {
  lua_State *L = luaL_newstate();
  if (!L) {
    puts("no state");
    return 1;
  }
  luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
  luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
  luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
  luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
  luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
  lua_settop(L, 0);
  if (luaL_dostring(L, script)) {
    printf("error: %s\n", lua_tostring(L, -1));
    return 1;
  }
  lua_close(L);
  puts("lua ok");
  return 0;
}
