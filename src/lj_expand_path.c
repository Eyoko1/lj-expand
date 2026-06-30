#include "lj_expand_path.h"

#include "lauxlib.h"
#include "lj_expand_globals.h"
#include "lj_expand_isolation.h"
#include "lj_expand_log.h"

#include <string.h>
#include <stdbool.h>

#include "lj_tab.h"

#define LJE_PATH_MT      "lje_path"
#define LJE_PATH_MAX_OPS 16
#define LJE_PATH_STR_MAX 128
#define LJE_EXPECT_TYPE_STR_MAX 64

// Essentially the path system is a fixed-function pipeline where you can retrieve certain values from
// another state undetected and without creating any GC steps or references.

typedef enum LJEPathOpKind
{
  LJE_PATH_OP_INDEX = 0,
  LJE_PATH_OP_UPVALUE = 1,
  LJE_PATH_OP_EXPECT = 2,
} LJEPathOpKind;

typedef struct LJEPathIndexOp
{
  char key[LJE_PATH_STR_MAX];
  int length;
} LJEPathIndexOp;

typedef struct LJEPathUpvalueOp
{
  int index;
} LJEPathUpvalueOp;

typedef struct LJEPathExpectOp
{
  char type[LJE_EXPECT_TYPE_STR_MAX];
} LJEPathExpectOp;

typedef struct LJEPathOp
{
  LJEPathOpKind kind;
  union
  {
    LJEPathIndexOp index;
    LJEPathUpvalueOp upvalue;
    LJEPathExpectOp expect;
  };
} LJEPathOp;

typedef struct LJEPath
{
  lua_State* target_state;
  int num_ops;
  LJEPathOp ops[LJE_PATH_MAX_OPS];
  char str[LJE_PATH_STR_MAX];
  int length;
} LJEPath;

// Handlers may return false to signal an unrecoverable error (i.e. indexing a non-table type)
// Handlers modify the current object by writing to the TValue.
typedef bool (*LJEPathOpHandlerFn)(lua_State* target_state, TValue* current_object, LJEPathOp* op);

bool index_path_op(lua_State* target_state, TValue* current_object, LJEPathOp* op);
bool upvalue_path_op(lua_State* target_state, TValue* current_object, LJEPathOp* op);
bool expect_path_op(lua_State* target_state, TValue* current_object, LJEPathOp* op);

static LJEPathOpHandlerFn path_op_handlers[] = {
  index_path_op,
  upvalue_path_op,
  expect_path_op,
};

static int method_index(lua_State* L)
{
  LJEPath* path = (LJEPath*)luaL_checkudata(L, 1, LJE_PATH_MT);
  const char* key = luaL_checkstring(L, 2);

  if (path->num_ops >= LJE_PATH_MAX_OPS)
  {
    luaL_error(L, "path operation limit exceeded");
    return 0;
  }

  LJEPathOp* op = &path->ops[path->num_ops++];
  op->kind = LJE_PATH_OP_INDEX;
  strncpy(op->index.key, key, LJE_PATH_STR_MAX - 1);
  op->index.key[LJE_PATH_STR_MAX - 1] = '\0';
  op->index.length = strlen(op->index.key);

  lua_pushvalue(L, 1); // Daisy-chain
  return 1;
}

static int method_upvalue(lua_State* L)
{
  LJEPath* path = (LJEPath*)luaL_checkudata(L, 1, LJE_PATH_MT);
  int index = luaL_checkinteger(L, 2);

  if (path->num_ops >= LJE_PATH_MAX_OPS)
  {
    luaL_error(L, "path operation limit exceeded");
    return 0;
  }

  LJEPathOp* op = &path->ops[path->num_ops++];
  op->kind = LJE_PATH_OP_UPVALUE;
  op->upvalue.index = index;

  lua_pushvalue(L, 1); // Daisy-chain
  return 1;
}

static int method_expect(lua_State* L)
{
  LJEPath* path = (LJEPath*)luaL_checkudata(L, 1, LJE_PATH_MT);
  const char* type_str = luaL_checkstring(L, 2);

  if (path->num_ops >= LJE_PATH_MAX_OPS)
  {
    luaL_error(L, "path operation limit exceeded");
    return 0;
  }

  LJEPathOp* op = &path->ops[path->num_ops++];
  op->kind = LJE_PATH_OP_EXPECT;
  strncpy(op->expect.type, type_str, LJE_EXPECT_TYPE_STR_MAX - 1);
  op->expect.type[LJE_EXPECT_TYPE_STR_MAX - 1] = '\0';

  lua_pushvalue(L, 1); // Daisy-chain
  return 1;
}

// Copy is the final pseudo-operation which evaluates the path and computes the result.
static int method_copy(lua_State* L)
{
  LJEPath* path = (LJEPath*)luaL_checkudata(L, 1, LJE_PATH_MT);
  TValue current_object;
  setnilV(&current_object);

  // Handle first string which determines the root of the path, we do this
  // by indexing it into the global state of the target
  lua_State* T = path->target_state;
  GCtab* env = tabref(T->env);
  cTValue* root = lj_tab_getstr_raw(env, path->str, path->length);
  if (!root)
  {
    LJE_WARN("Path root '%s' not found in target state", path->str);
    lua_pushnil(L);
    return 1;
  }

  if (tvisnil(root))
  {
    LJE_WARN("Path root '%s' is nil in target state", path->str);
    lua_pushnil(L);
    return 1;
  }

  copyTV(T, &current_object, root); // set up root for any future operations

  for (int i = 0; i < path->num_ops; ++i)
  {
    LJEPathOp* op = &path->ops[i];
    LJEPathOpHandlerFn handler = path_op_handlers[op->kind];

    if (!handler(T, &current_object, op))
    {
      LJE_WARN("Path operation failed at index %d", i);
      lua_pushnil(L);
      return 1;
    }
  }

  // The final result is still in foreign-state, bring it back.
  lje_copy_to_isolated_state_tv(T, L, &current_object);
  return 1;
}

// Leaves it on stack
static void initialize_mt(lua_State* L)
{
  if (luaL_newmetatable(L, LJE_PATH_MT))
  {
    lua_pushcfunction(L, method_index);
    lua_setfield(L, -2, "index");

    lua_pushcfunction(L, method_upvalue);
    lua_setfield(L, -2, "upvalue");

    lua_pushcfunction(L, method_expect);
    lua_setfield(L, -2, "expect");

    lua_pushcfunction(L, method_copy);
    lua_setfield(L, -2, "copy");

    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
  }
}

int lje_state_path(lua_State* L)
{
  lua_State* T = lua_touserdata(L, 1); // lightuserdata
  const char* path = luaL_checkstring(L, 2);

  LJEPath* path_ud = (LJEPath*)lua_newuserdata(L, sizeof(LJEPath));
  path_ud->target_state = T;
  path_ud->num_ops = 0;
  strncpy(path_ud->str, path, LJE_PATH_STR_MAX - 1);
  path_ud->str[LJE_PATH_STR_MAX - 1] = '\0';
  path_ud->length = strlen(path_ud->str);

  // Apply metatable
  initialize_mt(L);
  lua_setmetatable(L, -2);

  return 1;
}

void lje_path_install_state_globals(lua_State* L)
{
  if (!LJEG()->main_state)
  {
    LJE_WARN("Cannot install path globals, main state is null");
    return;
  }

  lua_getfield(L, LUA_GLOBALSINDEX, "lje");
  lua_getfield(L, -1, "state");
  lua_pushlightuserdata(L, (void*)LJEG()->main_state);
  lua_setfield(L, -2, "client");
  lua_pop(L, 1);
}

bool index_path_op(lua_State* target_state, TValue* current_object, LJEPathOp* op)
{
  if (!tvistab(current_object))
  {
    LJE_WARN("Index path op: current object is not a table");
    return false;
  }

  GCtab* tab = tabV(current_object);
  cTValue* value = lj_tab_getstr_raw(tab, op->index.key, op->index.length);
  if (!value)
  {
    LJE_WARN("Index path op: key '%s' not found in table", op->index.key);
    return false;
  }

  copyTV(target_state, current_object, value);
  return true;
}

bool upvalue_path_op(lua_State* target_state, TValue* current_object, LJEPathOp* op)
{
  if (!tvisfunc(current_object))
  {
    LJE_WARN("Upvalue path op: current object is not a function");
    return false;
  }

  GCfunc* func = funcV(current_object);
  if (isffunc(func))
  {
    LJE_WARN("Upvalue path op: current object is a fast function, cannot access upvalues");
    return false;
  }

  size_t upper_limit = isluafunc(func) ? funcproto(func)->sizeuv : func->c.nupvalues;
  if (op->upvalue.index < 1 || op->upvalue.index > upper_limit)
  {
    LJE_WARN("Upvalue path op: index %d out of bounds", op->upvalue.index);
    return false;
  }

  // Bit more complicated, we do want to handle Lua upvalues and C upvalues.
  if (isluafunc(func))
  {
    // Lua upvalues are stored in GCupvals->uv, and stored as TValues.
    TValue* uv = uvval(&gcref(func->l.uvptr[op->upvalue.index - 1])->uv);
    if (!uv)
    {
      LJE_WARN("Upvalue path op: Lua upvalue at index %d is null", op->upvalue.index);
      return false;
    }

    copyTV(target_state, current_object, uv);
    return true;
  } else if (iscfunc(func))
  {
    // C upvalues are stored directly in the function's c.upvalue array.
    copyTV(target_state, current_object, &func->c.upvalue[op->upvalue.index - 1]);
    return true;
  }

  return true;
}

// Resolves the GMod metatype (MetaName) of an object, or NULL if it has none.
// Instances (e.g. a Player userdata) carry it on their metatable; a raw
// metatable carries it directly.
static const char* gmod_metatype(TValue* o)
{
  GCtab* mt = NULL;
  if (tvisudata(o))
    mt = tabref(udataV(o)->metatable);
  else if (tvistab(o))
    mt = tabref(tabV(o)->metatable);

  if (mt)
  {
    cTValue* name = lj_tab_getstr_lit(mt, "MetaName");
    if (name && tvisstr(name))
      return strdata(strV(name));
  }

  if (tvistab(o))
  {
    cTValue* name = lj_tab_getstr_lit(tabV(o), "MetaName");
    if (name && tvisstr(name))
      return strdata(strV(name));
  }

  return NULL;
}

bool expect_path_op(lua_State* target_state, TValue* current_object, LJEPathOp* op)
{
  const char* expected_type = op->expect.type;

  // GMod metatype (e.g. "Player", "Vector") takes priority over the bare Lua type.
  const char* meta_name = gmod_metatype(current_object);
  if (meta_name && strcmp(expected_type, meta_name) == 0)
    return true;

  const char* actual_type = lj_typename(current_object);
  if (strcmp(expected_type, actual_type) == 0)
    return true;

  if (meta_name)
    LJE_WARN("Expect path op: expected '%s', got '%s' (%s)", expected_type, actual_type, meta_name);
  else
    LJE_WARN("Expect path op: expected '%s', got '%s'", expected_type, actual_type);
  return false;
}