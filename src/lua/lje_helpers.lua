-- Pure-Lua helpers for the secure environment. This file contains no initialization
-- logic (no C API pulls, registry hacks or metatable setup) -- only self-contained
-- helper functions. It runs alongside lje_secure_preinit.lua.

local function valueToString(v, indent, seen)
    local t = type(v)

    if t == "string" then
        return '"' .. v .. '"'
    elseif t == "number" or t == "boolean" then
        return tostring(v)
    elseif t == "nil" then
        return "nil"
    elseif t ~= "table" then
        -- function, userdata, thread, cdata, etc.
        return "<" .. t .. ": " .. tostring(v) .. ">"
    end

    seen = seen or {}

    -- It's a table from here on.
    if seen[v] then
        return "<recursion: " .. tostring(v) .. ">"
    end
    seen[v] = true

    local childIndent = indent .. "  "
    local out = "{\n"
    local isEmpty = true

    for k, val in pairs(v) do
        isEmpty = false

        local keyStr
        if type(k) == "string" then
            keyStr = k
        else
            keyStr = "[" .. tostring(k) .. "]"
        end

        out = out .. childIndent .. keyStr .. " = " .. valueToString(val, childIndent, seen) .. ",\n"
    end

    -- Allow this table to be visited again on sibling branches once we're done with it.
    seen[v] = nil

    if isEmpty then
        return "{}"
    end

    return out .. indent .. "}"
end

-- Neatly print out any value using lje.con_print. Handles nested tables with
-- indentation and guards against recursive (cyclic) references.
function lje.util.inspect(x)
    lje.con_print(valueToString(x, ""))
end

-- Recursively deep copy a value. Tables are duplicated key-by-key; everything
-- else (functions, userdata, cdata, etc.) is referenced as-is since those can't
-- be meaningfully duplicated. Cyclic references are preserved via the `seen` map,
-- and metatables are kept (by reference, as they're shared).
function lje.util.deep_copy(value, seen)
    if type(value) ~= "table" then
        return value
    end

    seen = seen or {}
    if seen[value] then
        return seen[value]
    end

    local copy = {}
    seen[value] = copy

    for k, v in pairs(value) do
        copy[k] = lje.util.deep_copy(v, seen)
    end

    local mt = getmetatable(value)
    if mt ~= nil then
        setmetatable(copy, mt)
    end

    return copy
end

-- Little printf console helper with color parsing
-- Usage: lje.con_printf("$red{Error}: Something happened!")
local ANSI_COLORS = {
    black = "1;30m",
    red = "1;31m",
    green = "1;32m",
    yellow = "1;33m",
    blue = "1;34m",
    magenta = "1;35m",
    cyan = "1;36m",
    white = "1;37m",
    default = "1;39m",
}

local COLOR_PATTERN = "%$(%a+)(%b{})"
lje.con_printf = function(fmt, ...)
    -- First, replace color codes
    local result = string.format(fmt, ...)
    local coloredResult = string.gsub(result, COLOR_PATTERN, function(colorName, text)
        local colorCode = ANSI_COLORS[string.lower(colorName)] or ANSI_COLORS["default"]
        return "\x1b[" .. colorCode .. string.sub(text, 2, -2) .. "\x1b[0m" -- remove braces
    end)

    lje.con_print(coloredResult .. "\x1b[0m") -- Reset color at the end
end

-- GMod's print is messed up. Just redirect to ours
function print(...)
    lje.con_print(table.concat({ ... }, "\t"))
end

function Color(r, g, b, a)
    return { r = r, g = g, b = b, a = a or 255 }
end

-- Binds a settings object to the calling script. Call at load time (when there is
-- a current script) and reuse the returned object anywhere, including deferred
-- callbacks like hooks/render/timers where there is no current script context.
function lje.settings.open()
    local name = lje.env.current_script()
    if not name then
        lje.con_print("lje.settings.open() called with no current script context!")
        return nil
    end
    return lje.settings.bind(name)
end
