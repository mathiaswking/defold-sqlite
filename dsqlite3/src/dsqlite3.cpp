#define EXTENSION_NAME dsqlite3
#define LIB_NAME "dsqlite3"
#define MODULE_NAME "dsqlite3"

#define DLIB_LOG_DOMAIN LIB_NAME
#include <dmsdk/sdk.h>
#include <stdlib.h>
#include "sqlite3.h"
// #include "TaskQueue.hpp"
// #include "variant.hpp"
// #include <string>
// #include <vector>
// #include <list>
// #include <mutex>


// static std::unique_ptr<TaskQueue> _task_queue(new TaskQueue(1));
// static std::list<TASK> _callback_tasks;
// static std::recursive_mutex _mutex;

// struct LuaCallbackInfo {
//     LuaCallbackInfo() : L(0), callback(LUA_NOREF), self(LUA_NOREF) {}
//     lua_State  *L;
//     int         callback;
//     int         self;
// };

// class NilType {};
// struct BlobType {
// 	std::string blob;
// };

// using var_t = mpark::variant<NilType, bool, int, double, std::string, BlobType>;
// using row_t = std::vector<var_t>;

// static void RegisterCallback(lua_State *L, int index, LuaCallbackInfo *cbk)
// {
//     if(cbk->callback != LUA_NOREF) {
//         dmScript::Unref(cbk->L, LUA_REGISTRYINDEX, cbk->callback);
//         dmScript::Unref(cbk->L, LUA_REGISTRYINDEX, cbk->self);
//     }

//     cbk->L = dmScript::GetMainThread(L);

//     luaL_checktype(L, index, LUA_TFUNCTION);
//     lua_pushvalue(L, index);
//     cbk->callback = dmScript::Ref(L, LUA_REGISTRYINDEX);

//     dmScript::GetInstance(L);
//     cbk->self = dmScript::Ref(L, LUA_REGISTRYINDEX);
// }

// static void UnregisterCallback(LuaCallbackInfo *cbk)
// {
//     if(cbk->callback != LUA_NOREF) {
//         dmScript::Unref(cbk->L, LUA_REGISTRYINDEX, cbk->callback);
//         dmScript::Unref(cbk->L, LUA_REGISTRYINDEX, cbk->self);
//         cbk->callback = LUA_NOREF;
//     }
// }

// static void InvokeCallback(LuaCallbackInfo *cbk, bool result, std::vector<row_t> rows)
// {
//     if(cbk->callback == LUA_NOREF)
//         return;

//     lua_State* L = cbk->L;
//     int top = lua_gettop(L);

//     lua_rawgeti(L, LUA_REGISTRYINDEX, cbk->callback);

//     // Setup self (the script instance)
//     lua_rawgeti(L, LUA_REGISTRYINDEX, cbk->self);
//     lua_pushvalue(L, -1);

//     dmScript::SetInstance(L);
//     if (!dmScript::IsInstanceValid(L)) {
//         lua_pop(L, 2);
//     } else {
//         lua_pushboolean(L, result);
//         int number_of_arguments = 2; // instance + 1
//         int ret = lua_pcall(L, number_of_arguments, 0, 0);
//         if(ret != 0) {
//             dmLogError("Error running callback: %s", lua_tostring(L, -1));
//             lua_pop(L, 1);
//         }
//     }
//     assert(top == lua_gettop(L));
// }

/* compile time features */
#if !defined(SQLITE_OMIT_PROGRESS_CALLBACK)
    #define SQLITE_OMIT_PROGRESS_CALLBACK 0
#endif
#if !defined(LSQLITE_OMIT_UPDATE_HOOK)
    #define LSQLITE_OMIT_UPDATE_HOOK 0
#endif
#if defined(LSQLITE_OMIT_OPEN_V2)
    #define SQLITE3_OPEN(L,filename,flags) sqlite3_open(L,filename)
#else
    #define SQLITE3_OPEN(L,filename,flags) sqlite3_open_v2(L,filename,flags,NULL)
#endif

typedef struct sdb sdb;
typedef struct sdb_vm sdb_vm;
typedef struct sdb_bu sdb_bu;
typedef struct sdb_func sdb_func;

/* to use as C user data so i know what function sqlite is calling */
struct sdb_func {
    /* references to associated lua values */
    int fn_step;
    int fn_finalize;
    int udata;

    sdb *db;
    char aggregate;

    sdb_func *next;
};

/* information about database */
struct sdb {
    /* associated lua state */
    lua_State *L;
    /* sqlite database handle */
    sqlite3 *db;

    /* sql functions stack usage */
    sdb_func *func;         /* top SQL function being called */

    /* references */
    int busy_cb;        /* busy callback */
    int busy_udata;

    int progress_cb;    /* progress handler */
    int progress_udata;

    int trace_cb;       /* trace callback */
    int trace_udata;

#if !defined(LSQLITE_OMIT_UPDATE_HOOK) || !LSQLITE_OMIT_UPDATE_HOOK

    int update_hook_cb; /* update_hook callback */
    int update_hook_udata;

    int commit_hook_cb; /* commit_hook callback */
    int commit_hook_udata;

    int rollback_hook_cb; /* rollback_hook callback */
    int rollback_hook_udata;

#endif
};

static const char *sqlite_meta      = ":sqlite3";
static const char *sqlite_vm_meta   = ":sqlite3:vm";

/* Lua 5.3 introduced an integer type, but depending on the implementation, it could be 32 
** or 64 bits (or something else?). This helper macro tries to do "the right thing."
*/

#if LUA_VERSION_NUM > 502
#define PUSH_INT64(L,i64in,fallback) \
    do { \
        sqlite_int64 i64 = i64in; \
        lua_Integer i = (lua_Integer )i64; \
        if (i == i64) lua_pushinteger(L, i);\
        else { \
            lua_Number n = (lua_Number)i64; \
            if (n == i64) lua_pushnumber(L, n); \
            else fallback; \
        } \
    } while (0)
#else
#define PUSH_INT64(L,i64in,fallback) \
    do { \
        sqlite_int64 i64 = i64in; \
        lua_Number n = (lua_Number)i64; \
        if (n == i64) lua_pushnumber(L, n); \
        else fallback; \
    } while (0)
#endif

/*
** =======================================================
** Database Virtual Machine Operations
** =======================================================
*/

static void vm_push_column(lua_State *L, sqlite3_stmt *vm, int idx) {
    switch (sqlite3_column_type(vm, idx)) {
        case SQLITE_INTEGER:
            PUSH_INT64(L, sqlite3_column_int64(vm, idx)
                     , lua_pushlstring(L, (const char*)sqlite3_column_text(vm, idx)
                                        , sqlite3_column_bytes(vm, idx)));
            break;
        case SQLITE_FLOAT:
            lua_pushnumber(L, sqlite3_column_double(vm, idx));
            break;
        case SQLITE_TEXT:
            lua_pushlstring(L, (const char*)sqlite3_column_text(vm, idx), sqlite3_column_bytes(vm, idx));
            break;
        case SQLITE_BLOB:
            lua_pushlstring(L, (const char *)sqlite3_column_blob(vm, idx), sqlite3_column_bytes(vm, idx));
            break;
        case SQLITE_NULL:
            lua_pushnil(L);
            break;
        default:
            lua_pushnil(L);
            break;
    }
}

/* virtual machine information */
struct sdb_vm {
    sdb *db;                /* associated database handle */
    sqlite3_stmt *vm;       /* virtual machine */

    /* sqlite3_step info */
    int columns;            /* number of columns in result */
    char has_values;        /* true when step succeeds */

    char temp;              /* temporary vm used in db:rows */
};

/* called with db,sql text on the lua stack */
static sdb_vm *newvm(lua_State *L, sdb *db) {
    sdb_vm *svm = (sdb_vm*)lua_newuserdata(L, sizeof(sdb_vm)); /* db sql svm_ud -- */

    luaL_getmetatable(L, sqlite_vm_meta);
    lua_setmetatable(L, -2);        /* set metatable */

    svm->db = db;
    svm->columns = 0;
    svm->has_values = 0;
    svm->vm = NULL;
    svm->temp = 0;

    /* add an entry on the database table: svm -> db to keep db live while svm is live */
    lua_pushlightuserdata(L, db);     /* db sql svm_ud db_lud -- */
    lua_rawget(L, LUA_REGISTRYINDEX); /* db sql svm_ud reg[db_lud] -- */
    lua_pushlightuserdata(L, svm);    /* db sql svm_ud reg[db_lud] svm_lud -- */
    lua_pushvalue(L, -5);             /* db sql svm_ud reg[db_lud] svm_lud db -- */
    lua_rawset(L, -3);                /* (reg[db_lud])[svm_lud] = db ; set the db for this vm */
    lua_pop(L, 1);                    /* db sql svm_ud -- */

    return svm;
}

static int cleanupvm(lua_State *L, sdb_vm *svm) {

    /* remove entry in database table - no harm if not present in the table */
    lua_pushlightuserdata(L, svm->db);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_pushlightuserdata(L, svm);
    lua_pushnil(L);
    lua_rawset(L, -3);
    lua_pop(L, 1);

    svm->columns = 0;
    svm->has_values = 0;

    if (!svm->vm) return 0;

    lua_pushinteger(L, sqlite3_finalize(svm->vm));
    svm->vm = NULL;
    return 1;
}

static int stepvm(lua_State *L, sdb_vm *svm) {
    return sqlite3_step(svm->vm);
}

static sdb_vm *lsqlite_getvm(lua_State *L, int index) {
    sdb_vm *svm = (sdb_vm*)luaL_checkudata(L, index, sqlite_vm_meta);
    if (svm == NULL) luaL_argerror(L, index, "bad sqlite virtual machine");
    return svm;
}

static sdb_vm *lsqlite_checkvm(lua_State *L, int index) {
    sdb_vm *svm = lsqlite_getvm(L, index);
    if (svm->vm == NULL) luaL_argerror(L, index, "attempt to use closed sqlite virtual machine");
    return svm;
}

static int dbvm_isopen(lua_State *L) {
    sdb_vm *svm = lsqlite_getvm(L, 1);
    lua_pushboolean(L, svm->vm != NULL ? 1 : 0);
    return 1;
}

static int dbvm_tostring(lua_State *L) {
    char buff[39];
    sdb_vm *svm = lsqlite_getvm(L, 1);
    if (svm->vm == NULL)
        strcpy(buff, "closed");
    else
        sprintf(buff, "%p", svm);
    lua_pushfstring(L, "sqlite virtual machine (%s)", buff);
    return 1;
}

static int dbvm_gc(lua_State *L) {
    sdb_vm *svm = lsqlite_getvm(L, 1);
    if (svm->vm != NULL)  /* ignore closed vms */
        cleanupvm(L, svm);
    return 0;
}

static int dbvm_step(lua_State *L) {
    int result;
    sdb_vm *svm = lsqlite_checkvm(L, 1);

    result = stepvm(L, svm);
    svm->has_values = result == SQLITE_ROW ? 1 : 0;
    svm->columns = sqlite3_data_count(svm->vm);

    lua_pushinteger(L, result);
    return 1;
}

static int dbvm_finalize(lua_State *L) {
    sdb_vm *svm = lsqlite_checkvm(L, 1);
    return cleanupvm(L, svm);
}

static int dbvm_reset(lua_State *L) {
    sdb_vm *svm = lsqlite_checkvm(L, 1);
    sqlite3_reset(svm->vm);
    lua_pushinteger(L, sqlite3_errcode(svm->db->db));
    return 1;
}

static void dbvm_check_contents(lua_State *L, sdb_vm *svm) {
    if (!svm->has_values) {
        luaL_error(L, "misuse of function");
    }
}

static void dbvm_check_index(lua_State *L, sdb_vm *svm, int index) {
    if (index < 0 || index >= svm->columns) {
        luaL_error(L, "index out of range [0..%d]", svm->columns - 1);
    }
}

static void dbvm_check_bind_index(lua_State *L, sdb_vm *svm, int index) {
    if (index < 1 || index > sqlite3_bind_parameter_count(svm->vm)) {
        luaL_error(L, "bind index out of range [1..%d]", sqlite3_bind_parameter_count(svm->vm));
    }
}

static int dbvm_last_insert_rowid(lua_State *L) {
    sdb_vm *svm = lsqlite_checkvm(L, 1);
    /* conversion warning: int64 -> luaNumber */
    sqlite_int64 rowid = sqlite3_last_insert_rowid(svm->db->db);
    PUSH_INT64(L, rowid, lua_pushfstring(L, "%lld", rowid));
    return 1;
}

/*
** =======================================================
** Virtual Machine - generic info
** =======================================================
*/
static int dbvm_columns(lua_State *L) {
    sdb_vm *svm = lsqlite_checkvm(L, 1);
    lua_pushinteger(L, sqlite3_column_count(svm->vm));
    return 1;
}

/*
** =======================================================
** Virtual Machine - getters
** =======================================================
*/

static int dbvm_get_value(lua_State *L) {
    sdb_vm *svm = lsqlite_checkvm(L, 1);
    int index = luaL_checkint(L, 2);
    dbvm_check_contents(L, svm);
    dbvm_check_index(L, svm, index);
    vm_push_column(L, svm->vm, index);
    return 1;
}

static int dbvm_get_name(lua_State *L) {
    sdb_vm *svm = lsqlite_checkvm(L, 1);
	int index = luaL_checkint(L, 2);
    dbvm_check_index(L, svm, index);
    lua_pushstring(L, sqlite3_column_name(svm->vm, index));
    return 1;
}

static int dbvm_get_type(lua_State *L) {
    sdb_vm *svm = lsqlite_checkvm(L, 1);
	int index = luaL_checkint(L, 2);
    dbvm_check_index(L, svm, index);
    lua_pushstring(L, sqlite3_column_decltype(svm->vm, index));
    return 1;
}

static int dbvm_get_values(lua_State *L) {
    sdb_vm *svm = lsqlite_checkvm(L, 1);
    sqlite3_stmt *vm = svm->vm;
    int columns = svm->columns;
    int n;
    dbvm_check_contents(L, svm);

    lua_createtable(L, columns, 0);
    for (n = 0; n < columns;) {
        vm_push_column(L, vm, n++);
        lua_rawseti(L, -2, n);
    }
    return 1;
}

static int dbvm_get_names(lua_State *L) {
    sdb_vm *svm = lsqlite_checkvm(L, 1);
    sqlite3_stmt *vm = svm->vm;
    int columns = sqlite3_column_count(vm); /* valid as soon as statement prepared */
    int n;

    lua_createtable(L, columns, 0);
    for (n = 0; n < columns;) {
        lua_pushstring(L, sqlite3_column_name(vm, n++));
        lua_rawseti(L, -2, n);
    }
    return 1;
}

static int dbvm_get_types(lua_State *L) {
    sdb_vm *svm = lsqlite_checkvm(L, 1);
    sqlite3_stmt *vm = svm->vm;
    int columns = sqlite3_column_count(vm); /* valid as soon as statement prepared */
    int n;

    lua_createtable(L, columns, 0);
    for (n = 0; n < columns;) {
        lua_pushstring(L, sqlite3_column_decltype(vm, n++));
        lua_rawseti(L, -2, n);
    }
    return 1;
}

static int dbvm_get_uvalues(lua_State *L) {
    sdb_vm *svm = lsqlite_checkvm(L, 1);
    sqlite3_stmt *vm = svm->vm;
    int columns = svm->columns;
    int n;
    dbvm_check_contents(L, svm);

    lua_checkstack(L, columns);
    for (n = 0; n < columns; ++n)
        vm_push_column(L, vm, n);
    return columns;
}

static int dbvm_get_unames(lua_State *L) {
    sdb_vm *svm = lsqlite_checkvm(L, 1);
    sqlite3_stmt *vm = svm->vm;
    int columns = sqlite3_column_count(vm); /* valid as soon as statement prepared */
    int n;

    lua_checkstack(L, columns);
    for (n = 0; n < columns; ++n)
        lua_pushstring(L, sqlite3_column_name(vm, n));
    return columns;
}

static int dbvm_get_utypes(lua_State *L) {
    sdb_vm *svm = lsqlite_checkvm(L, 1);
    sqlite3_stmt *vm = svm->vm;
    int columns = sqlite3_column_count(vm); /* valid as soon as statement prepared */
    int n;

    lua_checkstack(L, columns);
    for (n = 0; n < columns; ++n)
        lua_pushstring(L, sqlite3_column_decltype(vm, n));
    return columns;
}

static int dbvm_get_named_values(lua_State *L) {
    sdb_vm *svm = lsqlite_checkvm(L, 1);
    sqlite3_stmt *vm = svm->vm;
    int columns = svm->columns;
    int n;
    dbvm_check_contents(L, svm);

    lua_createtable(L, 0, columns);
    for (n = 0; n < columns; ++n) {
        lua_pushstring(L, sqlite3_column_name(vm, n));
        vm_push_column(L, vm, n);
        lua_rawset(L, -3);
    }
    return 1;
}

static int dbvm_get_named_types(lua_State *L) {
    sdb_vm *svm = lsqlite_checkvm(L, 1);
    sqlite3_stmt *vm = svm->vm;
    int columns = sqlite3_column_count(vm);
    int n;

    lua_createtable(L, 0, columns);
    for (n = 0; n < columns; ++n) {
        lua_pushstring(L, sqlite3_column_name(vm, n));
        lua_pushstring(L, sqlite3_column_decltype(vm, n));
        lua_rawset(L, -3);
    }
    return 1;
}

/*
** =======================================================
** Virtual Machine - Bind
** =======================================================
*/

static int dbvm_bind_index(lua_State *L, sqlite3_stmt *vm, int index, int lindex) {
    switch (lua_type(L, lindex)) {
        case LUA_TSTRING:
            return sqlite3_bind_text(vm, index, lua_tostring(L, lindex), (int)lua_strlen(L, lindex), SQLITE_TRANSIENT);
        case LUA_TNUMBER:
#if LUA_VERSION_NUM > 502
            if (lua_isinteger(L, lindex))
                return sqlite3_bind_int64(vm, index, lua_tointeger(L, lindex));
#endif
            return sqlite3_bind_double(vm, index, lua_tonumber(L, lindex));
        case LUA_TBOOLEAN:
            return sqlite3_bind_int(vm, index, lua_toboolean(L, lindex) ? 1 : 0);
        case LUA_TNONE:
        case LUA_TNIL:
            return sqlite3_bind_null(vm, index);
        default:
            luaL_error(L, "index (%d) - invalid data type for bind (%s)", index, lua_typename(L, lua_type(L, lindex)));
            return SQLITE_MISUSE; /*!*/
    }
}


static int dbvm_bind_parameter_count(lua_State *L) {
    sdb_vm *svm = lsqlite_checkvm(L, 1);
    lua_pushinteger(L, sqlite3_bind_parameter_count(svm->vm));
    return 1;
}

static int dbvm_bind_parameter_name(lua_State *L) {
    sdb_vm *svm = lsqlite_checkvm(L, 1);
	int index = luaL_checkint(L, 2);
    dbvm_check_bind_index(L, svm, index);
    lua_pushstring(L, sqlite3_bind_parameter_name(svm->vm, index));
    return 1;
}

static int dbvm_bind(lua_State *L) {
    sdb_vm *svm = lsqlite_checkvm(L, 1);
    sqlite3_stmt *vm = svm->vm;
    int index = luaL_checkint(L, 2);
    int result;

    dbvm_check_bind_index(L, svm, index);
    result = dbvm_bind_index(L, vm, index, 3);

    lua_pushinteger(L, result);
    return 1;
}

static int dbvm_bind_blob(lua_State *L) {
    sdb_vm *svm = lsqlite_checkvm(L, 1);
    int index = luaL_checkint(L, 2);
    const char *value = luaL_checkstring(L, 3);
    int len = (int)lua_strlen(L, 3);

    lua_pushinteger(L, sqlite3_bind_blob(svm->vm, index, value, len, SQLITE_TRANSIENT));
    return 1;
}

static int dbvm_bind_values(lua_State *L) {
    sdb_vm *svm = lsqlite_checkvm(L, 1);
    sqlite3_stmt *vm = svm->vm;
    int top = lua_gettop(L);
    int result, n;

    if (top - 1 != sqlite3_bind_parameter_count(vm))
        luaL_error(L,
            "incorrect number of parameters to bind (%d given, %d to bind)",
            top - 1,
            sqlite3_bind_parameter_count(vm)
        );

    for (n = 2; n <= top; ++n) {
        if ((result = dbvm_bind_index(L, vm, n - 1, n)) != SQLITE_OK) {
            lua_pushinteger(L, result);
            return 1;
        }
    }

    lua_pushinteger(L, SQLITE_OK);
    return 1;
}

static int dbvm_bind_names(lua_State *L) {
    sdb_vm *svm = lsqlite_checkvm(L, 1);
    sqlite3_stmt *vm = svm->vm;
    int count = sqlite3_bind_parameter_count(vm);
    const char *name;
    int result, n;
    luaL_checktype(L, 2, LUA_TTABLE);

    for (n = 1; n <= count; ++n) {
        name = sqlite3_bind_parameter_name(vm, n);
        if (name && (name[0] == ':' || name[0] == '$')) {
            lua_pushstring(L, ++name);
            lua_gettable(L, 2);
            result = dbvm_bind_index(L, vm, n, -1);
            lua_pop(L, 1);
        }
        else {
            lua_pushinteger(L, n);
            lua_gettable(L, 2);
            result = dbvm_bind_index(L, vm, n, -1);
            lua_pop(L, 1);
        }

        if (result != SQLITE_OK) {
            lua_pushinteger(L, result);
            return 1;
        }
    }

    lua_pushinteger(L, SQLITE_OK);
    return 1;
}

/*
** =======================================================
** Database (internal management)
** =======================================================
*/

/*
** When creating database handles, always creates a `closed' database handle
** before opening the actual database; so, if there is a memory error, the
** database is not left opened.
**
** Creates a new 'table' and leaves it in the stack
*/
static sdb *newdb (lua_State *L) {
    sdb *db = (sdb*)lua_newuserdata(L, sizeof(sdb));
    db->L = L;
    db->db = NULL;  /* database handle is currently `closed' */
    db->func = NULL;

    db->busy_cb =
    db->busy_udata =
    db->progress_cb =
    db->progress_udata =
    db->trace_cb =
    db->trace_udata = 
#if !defined(LSQLITE_OMIT_UPDATE_HOOK) || !LSQLITE_OMIT_UPDATE_HOOK
    db->update_hook_cb =
    db->update_hook_udata =
    db->commit_hook_cb =
    db->commit_hook_udata =
    db->rollback_hook_cb =
    db->rollback_hook_udata =
#endif
     LUA_NOREF;

    luaL_getmetatable(L, sqlite_meta);
    lua_setmetatable(L, -2);        /* set metatable */

    /* to keep track of 'open' virtual machines */
    lua_pushlightuserdata(L, db);
    lua_newtable(L);
    lua_rawset(L, LUA_REGISTRYINDEX);

    return db;
}

static int cleanupdb(lua_State *L, sdb *db) {
    sdb_func *func;
    sdb_func *func_next;
    int top;
    int result;

    /* free associated virtual machines */
    lua_pushlightuserdata(L, db);
    lua_rawget(L, LUA_REGISTRYINDEX);

    /* close all used handles */
    top = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        sdb_vm *svm = (sdb_vm *)lua_touserdata(L, -2); /* key: vm; val: sql text */
        cleanupvm(L, svm);

        lua_settop(L, top);
        lua_pushnil(L);
    }

    lua_pop(L, 1); /* pop vm table */

    /* remove entry in lua registry table */
    lua_pushlightuserdata(L, db);
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);

    /* 'free' all references */
    luaL_unref(L, LUA_REGISTRYINDEX, db->busy_cb);
    luaL_unref(L, LUA_REGISTRYINDEX, db->busy_udata);
    luaL_unref(L, LUA_REGISTRYINDEX, db->progress_cb);
    luaL_unref(L, LUA_REGISTRYINDEX, db->progress_udata);
    luaL_unref(L, LUA_REGISTRYINDEX, db->trace_cb);
    luaL_unref(L, LUA_REGISTRYINDEX, db->trace_udata);
#if !defined(LSQLITE_OMIT_UPDATE_HOOK) || !LSQLITE_OMIT_UPDATE_HOOK
    luaL_unref(L, LUA_REGISTRYINDEX, db->update_hook_cb);
    luaL_unref(L, LUA_REGISTRYINDEX, db->update_hook_udata);
    luaL_unref(L, LUA_REGISTRYINDEX, db->commit_hook_cb);
    luaL_unref(L, LUA_REGISTRYINDEX, db->commit_hook_udata);
    luaL_unref(L, LUA_REGISTRYINDEX, db->rollback_hook_cb);
    luaL_unref(L, LUA_REGISTRYINDEX, db->rollback_hook_udata);
#endif

    /* close database */
    result = sqlite3_close(db->db);
    db->db = NULL;

    /* free associated memory with created functions */
    func = db->func;
    while (func) {
        func_next = func->next;
        luaL_unref(L, LUA_REGISTRYINDEX, func->fn_step);
        luaL_unref(L, LUA_REGISTRYINDEX, func->fn_finalize);
        luaL_unref(L, LUA_REGISTRYINDEX, func->udata);
        free(func);
        func = func_next;
    }
    db->func = NULL;
    return result;
}

static sdb *lsqlite_getdb(lua_State *L, int index) {
    sdb *db = (sdb*)luaL_checkudata(L, index, sqlite_meta);
    if (db == NULL) luaL_typerror(L, index, "sqlite database");
    return db;
}

static sdb *lsqlite_checkdb(lua_State *L, int index) {
    sdb *db = lsqlite_getdb(L, index);
    if (db->db == NULL) luaL_argerror(L, index, "attempt to use closed sqlite database");
    return db;
}

/*
** =======================================================
** Database Methods
** =======================================================
*/

static int db_isopen(lua_State *L) {
    sdb *db = lsqlite_getdb(L, 1);
    lua_pushboolean(L, db->db != NULL ? 1 : 0);
    return 1;
}

static int db_last_insert_rowid(lua_State *L) {
    sdb *db = lsqlite_checkdb(L, 1);
    /* conversion warning: int64 -> luaNumber */
    sqlite_int64 rowid = sqlite3_last_insert_rowid(db->db);
    PUSH_INT64(L, rowid, lua_pushfstring(L, "%lld", rowid));
    return 1;
}

static int db_errcode(lua_State *L) {
    sdb *db = lsqlite_checkdb(L, 1);
    lua_pushinteger(L, sqlite3_errcode(db->db));
    return 1;
}

static int db_errmsg(lua_State *L) {
    sdb *db = lsqlite_checkdb(L, 1);
    lua_pushstring(L, sqlite3_errmsg(db->db));
    return 1;
}

/*
** Params: db, sql, callback, user
** returns: code [, errmsg]
**
** Callback:
** Params: user, number of columns, values, names
** Returns: 0 to continue, other value will cause abort
*/
static int db_exec_callback(void* user, int columns, char **data, char **names) {
    int result = SQLITE_ABORT; /* abort by default */
    lua_State *L = (lua_State*)user;
    int n;

    int top = lua_gettop(L);

    lua_pushvalue(L, 3); /* function to call */
    lua_pushvalue(L, 4); /* user data */
    lua_pushinteger(L, columns); /* total number of rows in result */

    /* column values */
    lua_pushvalue(L, 6);
    for (n = 0; n < columns;) {
        lua_pushstring(L, data[n++]);
        lua_rawseti(L, -2, n);
    }

    /* columns names */
    lua_pushvalue(L, 5);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_createtable(L, columns, 0);
        lua_pushvalue(L, -1);
        lua_replace(L, 5);
        for (n = 0; n < columns;) {
            lua_pushstring(L, names[n++]);
            lua_rawseti(L, -2, n);
        }
    }

    /* call lua function */
    if (!lua_pcall(L, 4, 1, 0)) {

#if LUA_VERSION_NUM > 502
        if (lua_isinteger(L, -1))
            result = lua_tointeger(L, -1);
        else
#endif
        if (lua_isnumber(L, -1))
            result = (int)lua_tonumber(L, -1);
    }

    lua_settop(L, top);
    return result;
}

static int db_exec(lua_State *L) {
    sdb *db = lsqlite_checkdb(L, 1);
    const char *sql = luaL_checkstring(L, 2);
    int result;

    if (!lua_isnoneornil(L, 3)) {
        /* stack:
        **  3: callback function
        **  4: userdata
        **  5: column names
        **  6: reusable column values
        */
        luaL_checktype(L, 3, LUA_TFUNCTION);
        lua_settop(L, 4);   /* 'trap' userdata - nil extra parameters */
        lua_pushnil(L);     /* column names not known at this point */
        lua_newtable(L);    /* column values table */

        result = sqlite3_exec(db->db, sql, db_exec_callback, L, NULL);
    }
    else {
        /* no callbacks */
        result = sqlite3_exec(db->db, sql, NULL, NULL, NULL);
    }

    lua_pushinteger(L, result);
    return 1;
}

/*
** Params: db, sql
** returns: code, compiled length or error message
*/
static int db_prepare(lua_State *L) {
    sdb *db = lsqlite_checkdb(L, 1);
    const char *sql = luaL_checkstring(L, 2);
    int sql_len = (int)lua_strlen(L, 2);
    const char *sqltail;
    sdb_vm *svm;
    lua_settop(L,2); /* db,sql is on top of stack for call to newvm */
    svm = newvm(L, db);

    if (sqlite3_prepare_v2(db->db, sql, sql_len, &svm->vm, &sqltail) != SQLITE_OK) {
        lua_pushnil(L);
        lua_pushinteger(L, sqlite3_errcode(db->db));
        if (cleanupvm(L, svm) == 1)
            lua_pop(L, 1); /* this should not happen since sqlite3_prepare_v2 will not set ->vm on error */
        return 2;
    }

    /* vm already in the stack */
    lua_pushstring(L, sqltail);
    return 2;
}

static int db_do_next_row(lua_State *L, int packed) {
    int result;
    sdb_vm *svm = lsqlite_checkvm(L, 1);
    sqlite3_stmt *vm;
    int columns;
    int i;

    result = stepvm(L, svm);
    vm = svm->vm; /* stepvm may change svm->vm if re-prepare is needed */
    svm->has_values = result == SQLITE_ROW ? 1 : 0;
    svm->columns = columns = sqlite3_data_count(vm);

    if (result == SQLITE_ROW) {
        if (packed) {
            if (packed == 1) {
                lua_createtable(L, columns, 0);
                for (i = 0; i < columns;) {
                    vm_push_column(L, vm, i);
                    lua_rawseti(L, -2, ++i);
                }
            }
            else {
                lua_createtable(L, 0, columns);
                for (i = 0; i < columns; ++i) {
                    lua_pushstring(L, sqlite3_column_name(vm, i));
                    vm_push_column(L, vm, i);
                    lua_rawset(L, -3);
                }
            }
            return 1;
        }
        else {
            lua_checkstack(L, columns);
            for (i = 0; i < columns; ++i)
                vm_push_column(L, vm, i);
            return svm->columns;
        }
    }

    if (svm->temp) {
        /* finalize and check for errors */
        result = sqlite3_finalize(vm);
        svm->vm = NULL;
        cleanupvm(L, svm);
    }
    else if (result == SQLITE_DONE) {
        result = sqlite3_reset(vm);
    }

    if (result != SQLITE_OK) {
        lua_pushstring(L, sqlite3_errmsg(svm->db->db));
        lua_error(L);
    }
    return 0;
}

static int db_next_row(lua_State *L) {
    return db_do_next_row(L, 0);
}

static int db_next_packed_row(lua_State *L) {
    return db_do_next_row(L, 1);
}

static int db_next_named_row(lua_State *L) {
    return db_do_next_row(L, 2);
}

static int dbvm_do_rows(lua_State *L, int(*f)(lua_State *)) {
    /* sdb_vm *svm =  */
    lsqlite_checkvm(L, 1);
    lua_pushvalue(L,1);
    lua_pushcfunction(L, f);
    lua_insert(L, -2);
    return 2;
}

static int dbvm_rows(lua_State *L) {
    return dbvm_do_rows(L, db_next_packed_row);
}

static int dbvm_nrows(lua_State *L) {
    return dbvm_do_rows(L, db_next_named_row);
}

static int dbvm_urows(lua_State *L) {
    return dbvm_do_rows(L, db_next_row);
}

static int db_do_rows(lua_State *L, int(*f)(lua_State *)) {
    sdb *db = lsqlite_checkdb(L, 1);
    const char *sql = luaL_checkstring(L, 2);
    sdb_vm *svm;
    lua_settop(L,2); /* db,sql is on top of stack for call to newvm */
    svm = newvm(L, db);
    svm->temp = 1;

    if (sqlite3_prepare_v2(db->db, sql, -1, &svm->vm, NULL) != SQLITE_OK) {
        lua_pushstring(L, sqlite3_errmsg(svm->db->db));
        if (cleanupvm(L, svm) == 1)
            lua_pop(L, 1); /* this should not happen since sqlite3_prepare_v2 will not set ->vm on error */
        lua_error(L);
    }

    lua_pushcfunction(L, f);
    lua_insert(L, -2);
    return 2;
}

static int db_rows(lua_State *L) {
    return db_do_rows(L, db_next_packed_row);
}

static int db_nrows(lua_State *L) {
    return db_do_rows(L, db_next_named_row);
}

/* unpacked version of db:rows */
static int db_urows(lua_State *L) {
    return db_do_rows(L, db_next_row);
}

static int db_tostring(lua_State *L) {
    char buff[32];
    sdb *db = lsqlite_getdb(L, 1);
    if (db->db == NULL)
        strcpy(buff, "closed");
    else
        sprintf(buff, "%p", lua_touserdata(L, 1));
    lua_pushfstring(L, "sqlite database (%s)", buff);
    return 1;
}

static int db_close(lua_State *L) {
    sdb *db = lsqlite_checkdb(L, 1);
    lua_pushinteger(L, cleanupdb(L, db));
    return 1;
}

static int db_close_vm(lua_State *L) {
    sdb *db = lsqlite_checkdb(L, 1);
    /* cleanup temporary only tables? */
    int temp = lua_toboolean(L, 2);

    /* free associated virtual machines */
    lua_pushlightuserdata(L, db);
    lua_rawget(L, LUA_REGISTRYINDEX);

    /* close all used handles */
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        sdb_vm *svm = (sdb_vm *)lua_touserdata(L, -2); /* key: vm; val: sql text */

        if ((!temp || svm->temp) && svm->vm)
        {
            sqlite3_finalize(svm->vm);
            svm->vm = NULL;
        }

        /* leave key in the stack */
        lua_pop(L, 1);
    }
    return 0;
}

static int db_gc(lua_State *L) {
    sdb *db = lsqlite_getdb(L, 1);
    if (db->db != NULL)  /* ignore closed databases */
        cleanupdb(L, db);
    return 0;
}

/*
** =======================================================
** General library functions
** =======================================================
*/

static int lsqlite_version(lua_State *L) {
    lua_pushstring(L, sqlite3_libversion());
    return 1;
}

static int lsqlite_do_open(lua_State *L, const char *filename, int flags) {
    sdb *db = newdb(L); /* create and leave in stack */

    if (SQLITE3_OPEN(filename, &db->db, flags) == SQLITE_OK) {
        /* database handle already in the stack - return it */
        return 1;
    }

    /* failed to open database */
    lua_pushnil(L);                             /* push nil */
    lua_pushinteger(L, sqlite3_errcode(db->db));
    lua_pushstring(L, sqlite3_errmsg(db->db));  /* push error message */

    /* clean things up */
    cleanupdb(L, db);

    /* return */
    return 3;
}

static int lsqlite_open(lua_State *L) {
    const char *filename = luaL_checkstring(L, 1);
    int flags = (int)luaL_optinteger(L, 2, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    return lsqlite_do_open(L, filename, flags);
}

static int lsqlite_open_memory(lua_State *L) {
    return lsqlite_do_open(L, ":memory:", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
}

static int lsqlite_newindex(lua_State *L) {
    lua_pushliteral(L, "attempt to change readonly table");
    lua_error(L);
    return 0;
}

/*
** =======================================================
** Register functions
** =======================================================
*/

#define SC(s)   { #s, SQLITE_ ## s },
#define LSC(s)  { #s, LSQLITE_ ## s },

static const struct {
    const char* name;
    int value;
} sqlite_constants[] = {
    /* error codes */
    SC(OK)          SC(ERROR)       SC(INTERNAL)    SC(PERM)
    SC(ABORT)       SC(BUSY)        SC(LOCKED)      SC(NOMEM)
    SC(READONLY)    SC(INTERRUPT)   SC(IOERR)       SC(CORRUPT)
    SC(NOTFOUND)    SC(FULL)        SC(CANTOPEN)    SC(PROTOCOL)
    SC(EMPTY)       SC(SCHEMA)      SC(TOOBIG)      SC(CONSTRAINT)
    SC(MISMATCH)    SC(MISUSE)      SC(NOLFS)
    SC(FORMAT)      SC(NOTADB)

    /* sqlite_step specific return values */
    SC(RANGE)       SC(ROW)         SC(DONE)

    /* column types */
    SC(INTEGER)     SC(FLOAT)       SC(TEXT)        SC(BLOB)
    SC(NULL)

    /* Authorizer Action Codes */
    SC(CREATE_INDEX       )
    SC(CREATE_TABLE       )
    SC(CREATE_TEMP_INDEX  )
    SC(CREATE_TEMP_TABLE  )
    SC(CREATE_TEMP_TRIGGER)
    SC(CREATE_TEMP_VIEW   )
    SC(CREATE_TRIGGER     )
    SC(CREATE_VIEW        )
    SC(DELETE             )
    SC(DROP_INDEX         )
    SC(DROP_TABLE         )
    SC(DROP_TEMP_INDEX    )
    SC(DROP_TEMP_TABLE    )
    SC(DROP_TEMP_TRIGGER  )
    SC(DROP_TEMP_VIEW     )
    SC(DROP_TRIGGER       )
    SC(DROP_VIEW          )
    SC(INSERT             )
    SC(PRAGMA             )
    SC(READ               )
    SC(SELECT             )
    SC(TRANSACTION        )
    SC(UPDATE             )
    SC(ATTACH             )
    SC(DETACH             )
    SC(ALTER_TABLE        )
    SC(REINDEX            )
    SC(ANALYZE            )
    SC(CREATE_VTABLE      )
    SC(DROP_VTABLE        )
    SC(FUNCTION           )
    SC(SAVEPOINT          )

    /* file open flags */
    SC(OPEN_READONLY)
    SC(OPEN_READWRITE)
    SC(OPEN_CREATE)
    SC(OPEN_URI)
    SC(OPEN_MEMORY)
    SC(OPEN_NOMUTEX)
    SC(OPEN_FULLMUTEX)
    SC(OPEN_SHAREDCACHE)
    SC(OPEN_PRIVATECACHE)
    
    /* terminator */
    { NULL, 0 }
};

/* ======================================================= */

static const luaL_Reg dblib[] = {
    {"isopen",              db_isopen               },
    {"last_insert_rowid",   db_last_insert_rowid    },
    {"error_code",          db_errcode              },
    {"error_message",       db_errmsg               },
    {"prepare",             db_prepare              },
    {"rows",                db_rows                 },
    {"urows",               db_urows                },
    {"nrows",               db_nrows                },

    {"exec",                db_exec                 },
    {"close",               db_close                },
    {"close_vm",            db_close_vm             },

    {"__tostring",          db_tostring             },
    {"__gc",                db_gc                   },

    {NULL, NULL}
};

static const luaL_Reg vmlib[] = {
    {"isopen",              dbvm_isopen             },

    {"step",                dbvm_step               },
    {"reset",               dbvm_reset              },
    {"finalize",            dbvm_finalize           },

    {"columns",             dbvm_columns            },

    {"bind",                dbvm_bind               },
    {"bind_values",         dbvm_bind_values        },
    {"bind_names",          dbvm_bind_names         },
    {"bind_blob",           dbvm_bind_blob          },
    {"bind_parameter_count",dbvm_bind_parameter_count},
    {"bind_parameter_name", dbvm_bind_parameter_name},

    {"get_value",           dbvm_get_value          },
    {"get_values",          dbvm_get_values         },
    {"get_name",            dbvm_get_name           },
    {"get_names",           dbvm_get_names          },
    {"get_type",            dbvm_get_type           },
    {"get_types",           dbvm_get_types          },
    {"get_uvalues",         dbvm_get_uvalues        },
    {"get_unames",          dbvm_get_unames         },
    {"get_utypes",          dbvm_get_utypes         },

    {"get_named_values",    dbvm_get_named_values   },
    {"get_named_types",     dbvm_get_named_types    },

    {"rows",                dbvm_rows               },
    {"urows",               dbvm_urows              },
    {"nrows",               dbvm_nrows              },

    {"last_insert_rowid",   dbvm_last_insert_rowid  },

    /* compatibility names (added by request) */
    {"idata",               dbvm_get_values         },
    {"inames",              dbvm_get_names          },
    {"itypes",              dbvm_get_types          },
    {"data",                dbvm_get_named_values   },
    {"type",                dbvm_get_named_types    },

    {"__tostring",          dbvm_tostring           },
    {"__gc",                dbvm_gc                 },

    { NULL, NULL }
};

static const luaL_Reg sqlitelib[] = {
    {"version",         lsqlite_version         },
    {"open",            lsqlite_open            },
    {"open_memory",     lsqlite_open_memory     },
    {"__newindex",      lsqlite_newindex        },
    {NULL, NULL}
};

static void create_meta(lua_State *L, const char *name, const luaL_Reg *lib) {
    luaL_newmetatable(L, name);
    lua_pushstring(L, "__index");
    lua_pushvalue(L, -2);               /* push metatable */
    lua_rawset(L, -3);                  /* metatable.__index = metatable */

    /* register metatable functions */
    luaL_openlib(L, NULL, lib, 0);

    /* remove metatable from stack */
    lua_pop(L, 1);
}

static void LuaInit(lua_State* L)
{
    int top = lua_gettop(L);

    create_meta(L, sqlite_meta, dblib);
    create_meta(L, sqlite_vm_meta, vmlib);

    /* register (local) sqlite metatable */
    luaL_register(L, MODULE_NAME, sqlitelib);

    int i = 0;
    /* add constants to global table */
    while (sqlite_constants[i].name) {
        lua_pushstring(L, sqlite_constants[i].name);
        lua_pushinteger(L, sqlite_constants[i].value);
        lua_rawset(L, -3);
        ++i;
    }

    /* set sqlite's metatable to itself - set as readonly (__newindex) */
    lua_pushvalue(L, -1);
    lua_setmetatable(L, -2);

    lua_pop(L, 1);
    assert(top == lua_gettop(L));
}

dmExtension::Result AppInitializeDefoldSqlite3Extension(dmExtension::AppParams* params)
{
    return dmExtension::RESULT_OK;
}

dmExtension::Result AppFinalizeDefoldSqlite3Extension(dmExtension::AppParams* params)
{
    return dmExtension::RESULT_OK;
}

dmExtension::Result InitializeDefoldSqlite3Extension(dmExtension::Params* params)
{
    LuaInit(params->m_L);
    return dmExtension::RESULT_OK;
}

// dmExtension::Result UpdateDefoldSqlite3Extension(dmExtension::Params* params)
// {
// 	std::unique_lock<std::recursive_mutex> lock(_mutex);
// 	if (!_callback_tasks.empty()) {
// 		auto& task = _callback_tasks.front();
// 		task();
// 		_callback_tasks.pop_front();
// 	}
//     return dmExtension::RESULT_OK;
// }

dmExtension::Result FinalizeDefoldSqlite3Extension(dmExtension::Params* params)
{
    return dmExtension::RESULT_OK;
}

// DM_DECLARE_EXTENSION(EXTENSION_NAME, LIB_NAME, AppInitializeDefoldSqlite3Extension, AppFinalizeDefoldSqlite3Extension, InitializeDefoldSqlite3Extension, UpdateDefoldSqlite3Extension, 0, FinalizeDefoldSqlite3Extension)
DM_DECLARE_EXTENSION(EXTENSION_NAME, LIB_NAME, AppInitializeDefoldSqlite3Extension, AppFinalizeDefoldSqlite3Extension, InitializeDefoldSqlite3Extension, 0, 0, FinalizeDefoldSqlite3Extension)

