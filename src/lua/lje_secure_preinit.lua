-- Secure preinit script. No need for a custom environment as this takes place entirely within a
-- separate Lua universe. All we do here is ensure that all the C API is pulled in, add a hook library,
-- ensure metatables exist in registry and perform some GMod-specific registry hacks.
-- Pure-Lua helpers live in lje_helpers.lua.

lje.con_print("Running secure preinit script...")

local registry = lje.util.get_registry()

registry["__lje_shadow_registry"][2] = { hook = { Call = function() end } } -- hook ref GMod uses
registry["__lje_shadow_registry"][1337153] = function() return "hello" end
registry["__lje_shadow_registry"][13371010] = function() return end         -- Dummy Lua function


-- Only a subset of necessary GMod C APIs are pulled in. We have our own versions of any base library as well.
achievements = lje.secure.pull("achievements")
cam = lje.secure.pull("cam")
chat = lje.secure.pull("chat")
engine = lje.secure.pull("engine")
ents = lje.secure.pull("ents")
file = lje.secure.pull("file")
game = lje.secure.pull("game")
gameevent = lje.secure.pull("gameevent")
gmod = lje.secure.pull("gmod")
gui = lje.secure.pull("gui")
input = lje.secure.pull("input")
mesh = lje.secure.pull("mesh")
net = lje.secure.pull("net")
player = lje.secure.pull("player")
render = lje.secure.pull("render")
sound = lje.secure.pull("sound")
sql = lje.secure.pull("sql")
surface = lje.secure.pull("surface")
steamworks = lje.secure.pull("steamworks")
system = lje.secure.pull("system")
timer = lje.secure.pull("timer")
util = lje.secure.pull("util")
vgui = lje.secure.pull("vgui")

-- Globals
local globalEnv = lje.secure.pull("_G")
for k, v in pairs(globalEnv) do
    if _G[k] == nil then
        _G[k] = v -- Merge
    end
end

-- Classes
registry.Angle = lje.secure.pull("_R.Angle")
registry.CMoveData = lje.secure.pull("_R.CMoveData")
registry.ConVar = lje.secure.pull("_R.ConVar")
registry.CSEnt = lje.secure.pull("_R.CSEnt")
registry.CUserCmd = lje.secure.pull("_R.CUserCmd")
registry.Entity = lje.secure.pull("_R.Entity")
registry.File = lje.secure.pull("_R.File")
registry.IMaterial = lje.secure.pull("_R.IMaterial")
registry.IMesh = lje.secure.pull("_R.IMesh")
registry.ITexture = lje.secure.pull("_R.ITexture")
registry.NPC = lje.secure.pull("_R.NPC")
registry.Panel = lje.secure.pull("_R.Panel")
registry.PhysObj = lje.secure.pull("_R.PhysObj")
registry.Player = lje.secure.pull("_R.Player")
registry.ProjectedTexture = lje.secure.pull("_R.ProjectedTexture")
registry.Vector = lje.secure.pull("_R.Vector")
registry.Vehicle = lje.secure.pull("_R.Vehicle")
registry.VMatrix = lje.secure.pull("_R.VMatrix")
registry.Weapon = lje.secure.pull("_R.Weapon")

NULL = lje.secure.pull("NULL") -- For some reason, the null entity is a special userdata..?

-- Polyfills for common things
local cam2D = { type = "2D" }
function cam.Start2D()
    cam.Start(cam2D)
end

-- Small hook library replacement, no returns for now.
hook = {}
hook._listeners = {}

function hook.Add(event, identifier, func)
    local script = lje.env.current_script()
    hook._listeners[script] = hook._listeners[script] or {}
    if not hook._listeners[script][event] then
        hook._listeners[script][event] = {}
    end
    hook._listeners[script][event][identifier] = func
end

function hook.Listen()
    local script = lje.env.current_script()
    if not hook._listeners[script] then
        hook._listeners[script] = {}
    end

    lje.vm.set_engine_call_hook(function(func, nargs, nresults, ...)
        local name = ...
        local listeners = hook._listeners
        local callbacks = listeners[script][name]
        if callbacks then
            for _, listener in pairs(callbacks) do
                listener(...)
            end
        end
    end)
end

function hook.Call() --[[no-op]] end

lje.includeCache = {}
lje.require = function(path)
    local currentScript = lje.env.current_script()
    if not currentScript then
        lje.con_print("Error: lje.require called outside of a script context!")
        return
    end

    lje.includeCache[currentScript] = lje.includeCache[currentScript] or {}
    local scriptCache = lje.includeCache[currentScript]
    if scriptCache[path] then
        return scriptCache[path]
    end

    local result = lje.include(path)
    scriptCache[path] = result
    return result
end
