#include "lj_expand_isolation.h"

#include "lauxlib.h"
#include "lj_expand_globals.h"
#include "lj_expand_lib.h"
#include "lj_expand_log.h"
#include "lualib.h"

#include <windows.h>

#include "lj_dispatch.h"
#include "lj_func.h"
#include "lj_gc.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_udata.h"

typedef lua_State* (*luaL_newstate_t)(void);
typedef void       (*luaL_openlibs_t)(lua_State*);

lua_State* lje_create_isolated_state() {
    HMODULE gmod_lua = GetModuleHandleA("lua_shared.dll");
    if (!gmod_lua)
    {
        LJE_ERROR("Failed to locate GMod's lua_shared.dll for isolated state creation.");
        return NULL;
    }

    luaL_newstate_t gmod_newstate = (luaL_newstate_t)GetProcAddress(gmod_lua, "luaL_newstate");
    luaL_openlibs_t gmod_openlibs = (luaL_openlibs_t)GetProcAddress(gmod_lua, "luaL_openlibs");
    if (!gmod_newstate || !gmod_openlibs)
    {
        LJE_ERROR("Failed to resolve GMod luaL_newstate/luaL_openlibs exports.");
        return NULL;
    }

    // We deliberately use GMod's luaL_newstate (and luaL_openlibs) instead of our own
    // bundled LuaJIT's, because the engine's loader/parser/lexer all operate against
    // GMod's lua_State and global_State layouts. A state allocated by our LuaJIT would
    // be the wrong shape and the keyword table indices wouldn't match the engine's
    // TK_* enum, causing nonsense parser errors (e.g. "near 'in'" on every script).
    lua_State* L = gmod_newstate();
    if (!L)
    {
        LJE_ERROR("Failed to create isolated Lua state.");
        return NULL;
    }

    // An isolated state basically just means we create an entirely separated Lua universe
    // for LJE only. Then we pull in our API functions only.

    LJEG()->isolated_state = L;
    gmod_openlibs(L); // Open standard libraries (using GMod's openlibs, since this is a GMod-shaped state)
    lje_addfuncs(L);  // Add LJE-specific functions to the isolated state
    // Open dispatch table with our stuff, however.
    lj_dispatch_init((GG_State*)L);

    // Create the shadow registry
    lua_newtable(L);
    LJEG()->shadow_registry = tabV(L->top - 1);
    lua_setfield(L, LUA_REGISTRYINDEX, "__lje_shadow_registry");

    LJE_SUCCESS("Created isolated Lua state: %p", (void*)L);
    return L;
}

static TValue *stkindex2adr(lua_State *L, int idx)
{
    if (idx > 0) {
        TValue *o = L->base + (idx - 1);
        return o < L->top ? o : niltv(L);
    } else {
        return L->top + idx;
    }
}

// Wraps every C function we pull so we can securely call it.
static int lje_secure_gmod_api(lua_State* L)
{
    void* func_ptr = lua_touserdata(L, lua_upvalueindex(1));
    if (!func_ptr)
    {
        lua_pushnil(L);
        return 1;
    }

    // we just need to dispatch the c function ourselves.
    lua_CFunction func = (lua_CFunction)func_ptr;
    int prev = LJEG()->redirect_to_isolation;
    LJEG()->redirect_to_isolation = 1;
    int results = func(LJEG()->main_state); /* make it think it's running in the main state, it won't know any better! */
    LJEG()->redirect_to_isolation = prev;
    return results;
}

int lje_push_safe_cfunction(lua_State* L, lua_CFunction func)
{
    // We push a wrapper C function that securely redirects to the main state, and we store the real function pointer in an upvalue.
    lua_pushlightuserdata(L, (void*)func);
    lua_pushcclosure(L, lje_secure_gmod_api, 1);
    return 1;
}


static int copy_to_isolated_state(lua_State* from, lua_State* to, cTValue* val, TValue* dst, GCtab* seen);
/* Recover hbits from hmask. hmask is always (1<<hbits)-1 or 0 for the empty sentinel. */
static uint32_t hmask_to_hbits(uint32_t hmask)
{
    if (hmask == 0) return 0;
    uint32_t hbits = 0;
    uint32_t n = hmask + 1;
    while (n > 1) { n >>= 1; hbits++; }
    return hbits;
}

static GCtab* deep_copy_table(lua_State* from, lua_State* to, GCtab* from_table, GCtab* seen)
{
    TValue key;
    setlightudV(&key, from_table);
    cTValue* existing = lj_tab_get(to, seen, &key);
    if (!tvisnil(existing))
    {
        lua_assert(tvistab(existing));
        return tabV(existing);
    }

    Node* from_nodes = noderef(from_table->node);
    int has_hash_part = (from_nodes != &G(from)->nilnode);
    uint32_t hbits = has_hash_part ? hmask_to_hbits(from_table->hmask) : 0;

    GCtab* new = lj_tab_new(to, from_table->asize, hbits);

    // Barrier for any insertions into the new table.
    lj_gc_anybarriert(to, new);

    { /* Record the mapping BEFORE recursing so cycles terminate.
         Storing `new` as a real table value means `seen` transitively
         keeps every destination table we've created so far alive for
         the entire duration of the copy. */
        TValue* slot = lj_tab_set(to, seen, &key);
        settabV(to, slot, new);
        lj_gc_anybarriert(to, seen);
    }

    /* Sanity: lj_tab_new should have produced the same shape we asked for. */
    lua_assert(new->asize == from_table->asize);
    lua_assert(!has_hash_part || new->hmask == from_table->hmask);

    if (from_table->asize > 0)
    {
        TValue* from_array = tvref(from_table->array);
        TValue* to_array = tvref(new->array);
        for (uint32_t i = 0; i < from_table->asize; i++)
        {
            copy_to_isolated_state(from, to, &from_array[i], &to_array[i], seen);
        }
    }

    if (has_hash_part)
    {
        for (uint32_t i = 0; i <= from_table->hmask; i++)
        {
            if (tvisnil(&from_nodes[i].key))
                continue;

            /* Build the key in a temp TValue first */
            TValue newkey;
            copy_to_isolated_state(from, to, &from_nodes[i].key, &newkey, seen);

            /* Let LuaJIT place it in the correct main position */
            TValue* slot = lj_tab_set(to, new, &newkey);

            /* Now copy the value into the slot LuaJIT chose */
            copy_to_isolated_state(from, to, &from_nodes[i].val, slot, seen);
        }
    }

    if (gcref(from_table->metatable))
    {
        GCtab* mt_src = tabref(from_table->metatable);
        GCtab* mt_dst = deep_copy_table(from, to, mt_src, seen);
        setgcref(new->metatable, obj2gco(mt_dst));
    }

    new->nomm = 0;

    return new;
}

static int copy_to_isolated_state(lua_State* from, lua_State* to, cTValue* val, TValue* dst, GCtab* seen)
{
    /* LJE: Fast routine to copy a value from one state to the other. Functions
       are (mostly) not supported — C functions are wrapped, Lua functions become nil.
       The main issue with other tools trying to do this is the inane amount of function
       calls to the Lua C API that it requires since they're exports and can't be inlined.
       We use the internal functions directly. */

    if (tvisnumber(val))
    {
        /* No direct type tag, so it's handled before the switch. */
        setnumV(dst, numV(val));
        return 1;
    }

    int unsupported = 0;
    switch (itype(val))
    {
        case LJ_TNIL:
            setnilV(dst);
            break;
        case LJ_TFALSE:
            setboolV(dst, 0);
            break;
        case LJ_TTRUE:
            setboolV(dst, 1);
            break;
        case LJ_TLIGHTUD:
            setlightudV(dst, lightudV(val));
            break;
        case LJ_TSTR:
        {
            /* String interning is *not* consistent between universes, must copy. */
            GCstr* copy = lj_str_new(to, strdata(strV(val)), strV(val)->len);
            setstrV(to, dst, copy);
            break;
        }
        case LJ_TTAB:
            settabV(to, dst, deep_copy_table(from, to, tabV(val), seen));
            lj_gc_barriert(to, tabV(dst), dst);
            break;
        case LJ_TUDATA:
        {
            GCudata* from_ud = udataV(val);
            GCudata* to_ud = lj_udata_new(to, from_ud->len, tabref(to->env));
            memcpy(uddata(to_ud), uddata(from_ud), from_ud->len);
            if (gcref(from_ud->metatable))
            {
                // We need to copy the metatable, check if it's a GMod one:
                GCtab* mt_src = tabref(from_ud->metatable);
                cTValue* meta_name = lj_tab_getstr_lit(mt_src, "MetaName");
                if (meta_name && tvisstr(meta_name))
                {
                    const char* name = strdata(strV(meta_name));
                    // Check if the to state has its own version already (metatables can be manipulated)
                    GCtab* to_reg = tabV(registry(LJEG()->isolated_state)); /* not shadow, metatables are stored in main */
                    cTValue* to_metatable = lj_tab_getstr_raw(to_reg, name, strlen(name)); // Can't use raw GCstr, comes from the from state
                    if (to_metatable && tvistab(to_metatable))
                    {
                        // This metatable exists within the to state, so use it.
                        setgcref(to_ud->metatable, obj2gco(tabV(to_metatable)));
                        lj_gc_objbarrier(to, to_ud, tabV(to_metatable));
                    }
                }
            }
            setudataV(to, dst, to_ud);
            break;
        }
        case LJ_TFUNC:
        {
            /* C functions are wrappable. Lua/fast functions are not supported. */
            GCfunc* from_fn = funcV(val);
            if (iscfunc(from_fn))
            {
                GCfunc* new = lj_func_newC(to, 1, tabref(to->env));
                new->c.f = lje_secure_gmod_api;
                setlightudV(&new->c.upvalue[0], from_fn->c.f);
                setfuncV(to, dst, new);
                break;
            } else if (isluafunc(from_fn))
            {
              // Print out exactly what this is
              GCproto* pt = funcproto(from_fn);
              LJE_DEBUG("Encountered Lua function from %s, not copying.", proto_chunknamestr(pt));
              // Set an empty Lua function, then.
              // Our registry has 13371010 as the empty Lua function, so we can just copy that in.
              cTValue* empty_lua_func = lj_tab_getint(LJEG()->shadow_registry, 13371010);
              setfuncV(to, dst, funcV(empty_lua_func));
              break;
            }
            unsupported = 1;
            break;
        }
        default:
            unsupported = 1;
            break;
    }

    if (unsupported)
    {
        setnilV(dst);
        return 1;
    }

    return 1;
}

static int lje_copy_with_seen(lua_State* from, lua_State* to, cTValue* val)
{
    GCtab* seen = lj_tab_new(to, 0, 4);
    settabV(to, to->top, seen);
    to->top++;

    /* Copy into the slot above the anchor. */
    int r;
    if (tvisnil(val)) {
        setnilV(to->top);
        r = 0;
    } else {
        r = copy_to_isolated_state(from, to, val, to->top, seen);
    }
    to->top++;

    /* Stack is now [..., seen, result]. Collapse to [..., result]. */
    copyTV(to, to->top - 2, to->top - 1);
    to->top--;

    return r;
}

int lje_copy_to_isolated_state(lua_State* from, lua_State* to, int idx)
{
    cTValue* val = stkindex2adr(from, idx);
    return lje_copy_with_seen(from, to, val);
}

int lje_copy_to_isolated_state_tv(lua_State* from, lua_State* to, cTValue* val)
{
    return lje_copy_with_seen(from, to, val);
}