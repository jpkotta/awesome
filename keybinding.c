/*
 * keybinding.c - Key bindings configuration management
 *
 * Copyright © 2008 Julien Danjou <julien@danjou.info>
 * Copyright © 2008 Pierre Habouzit <madcoder@debian.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

/* XStringToKeysym() */
#include <X11/Xlib.h>

#include "keybinding.h"
#include "event.h"
#include "lua.h"
#include "window.h"

extern awesome_t globalconf;

DO_LUA_NEW(static, keybinding_t, keybinding, "keybinding", keybinding_ref)
DO_LUA_GC(keybinding_t, keybinding, "keybinding", keybinding_unref)

void keybinding_idx_wipe(keybinding_idx_t *idx)
{
    keybinding_array_wipe(&idx->by_code);
    keybinding_array_wipe(&idx->by_sym);
}

void keybinding_delete(keybinding_t **kbp)
{
    p_delete(kbp);
}

static int
keybinding_ev_cmp(xcb_keysym_t keysym, xcb_keycode_t keycode,
                  unsigned long mod, const keybinding_t *k)
{
    if (k->keysym) {
        if (k->keysym != keysym)
            return k->keysym > keysym ? 1 : -1;
    }
    if (k->keycode) {
        if (k->keycode != keycode)
            return k->keycode > keycode ? 1 : -1;
    }
    return k->mod == mod ? 0 : (k->mod > mod ? 1 : -1);
}

static int
keybinding_cmp(const keybinding_t *k1, const keybinding_t *k2)
{
    assert ((k1->keysym && k2->keysym) || (k1->keycode && k2->keycode));
    assert ((!k1->keysym && !k2->keysym) || (!k1->keycode && !k2->keycode));

    if (k1->keysym != k2->keysym)
        return k2->keysym > k1->keysym ? 1 : -1;
    if (k1->keycode != k2->keycode)
        return k2->keycode > k1->keycode ? 1 : -1;
    return k1->mod == k2->mod ? 0 : (k2->mod > k1->mod ? 1 : -1);
}

void
keybinding_register_root(keybinding_t *k)
{
    keybinding_idx_t *idx = &globalconf.keys;
    keybinding_array_t *arr = k->keysym ? &idx->by_sym : &idx->by_code;
    int l = 0, r = arr->len;

    keybinding_ref(&k);

    while (l < r) {
        int i = (r + l) / 2;
        switch (keybinding_cmp(k, arr->tab[i])) {
          case -1: /* k < arr->tab[i] */
            r = i;
            break;
          case 0: /* k == arr->tab[i] */
            keybinding_unref(&arr->tab[i]);
            arr->tab[i] = k;
            return;
          case 1: /* k > arr->tab[i] */
            l = i + 1;
            break;
        }
    }

    keybinding_array_splice(arr, r, 0, &k, 1);
    window_root_grabkey(k);
}

void
keybinding_unregister_root(keybinding_t **k)
{
    keybinding_idx_t *idx = &globalconf.keys;
    keybinding_array_t *arr = (*k)->keysym ? &idx->by_sym : &idx->by_code;
    int l = 0, r = arr->len;

    while (l < r) {
        int i = (r + l) / 2;
        switch (keybinding_cmp(*k, arr->tab[i])) {
          case -1: /* k < arr->tab[i] */
            r = i;
            break;
          case 0: /* k == arr->tab[i] */
            keybinding_array_take(arr, i);
            window_root_ungrabkey(*k);
            keybinding_unref(k);
            return;
          case 1: /* k > arr->tab[i] */
            l = i + 1;
            break;
        }
    }
}

keybinding_t *
keybinding_find(const keybinding_idx_t *idx, const xcb_key_press_event_t *ev)
{
    const keybinding_array_t *arr = &idx->by_sym;
    int l, r, mod = CLEANMASK(ev->state);
    xcb_keysym_t keysym;

    keysym = xcb_key_symbols_get_keysym(globalconf.keysyms, ev->detail, 0);

  again:
    l = 0;
    r = arr->len;
    while (l < r) {
        int i = (r + l) / 2;
        switch (keybinding_ev_cmp(keysym, ev->detail, mod, arr->tab[i])) {
          case -1: /* ev < arr->tab[i] */
            r = i;
            break;
          case 0: /* ev == arr->tab[i] */
            return arr->tab[i];
          case 1: /* ev > arr->tab[i] */
            l = i + 1;
            break;
        }
    }
    if (arr != &idx->by_code) {
        arr = &idx->by_code;
        goto again;
    }
    return NULL;
}

static void
__luaA_keystore(keybinding_t *key, const char *str)
{
    if(!a_strlen(str))
        return;
    else if(*str != '#')
        key->keysym = XStringToKeysym(str);
    else
        key->keycode = atoi(str + 1);
}

/** Define a global key binding. This key binding will always be available.
 * \param L The Lua VM state.
 *
 * \luastack
 * \lparam A table with modifier keys.
 * \lparam A key name.
 * \lparam A function to execute.
 * \lreturn The keybinding.
 */
static int
luaA_keybinding_new(lua_State *L)
{
    size_t i, len;
    keybinding_t *k;
    const char *key;

    /* arg 1 is key mod table */
    luaA_checktable(L, 1);
    /* arg 2 is key */
    key = luaL_checkstring(L, 2);
    /* arg 3 is cmd to run */
    luaA_checkfunction(L, 3);

    /* get the last arg as function */
    k = p_new(keybinding_t, 1);
    __luaA_keystore(k, key);
    k->fct = luaL_ref(L, LUA_REGISTRYINDEX);

    len = lua_objlen(L, 1);
    for(i = 1; i <= len; i++)
    {
        lua_rawgeti(L, 1, i);
        k->mod |= xutil_keymask_fromstr(luaL_checkstring(L, -1));
    }

    return luaA_keybinding_userdata_new(L, k);
}

/** Add a global key binding. This key binding will always be available.
 * \param L The Lua VM state.
 *
 * \luastack
 * \lvalue A keybinding.
 */
static int
luaA_keybinding_add(lua_State *L)
{
    keybinding_t **k = luaA_checkudata(L, 1, "keybinding");

    keybinding_register_root(*k);
    return 0;
}

/** Remove a global key binding.
 * \param L The Lua VM state.
 *
 * \luastack
 * \lvalue A keybinding.
 */
static int
luaA_keybinding_remove(lua_State *L)
{
    keybinding_t **k = luaA_checkudata(L, 1, "keybinding");
    keybinding_unregister_root(k);
    return 0;
}

/** Convert a keybinding to a printable string.
 * \return A string.
 */
static int
luaA_keybinding_tostring(lua_State *L)
{
    keybinding_t **p = luaA_checkudata(L, 1, "keybinding");
    lua_pushfstring(L, "[keybinding udata(%p)]", *p);
    return 1;
}

const struct luaL_reg awesome_keybinding_methods[] =
{
    { "new", luaA_keybinding_new },
    { NULL, NULL }
};
const struct luaL_reg awesome_keybinding_meta[] =
{
    {"add", luaA_keybinding_add },
    {"remove", luaA_keybinding_remove },
    {"__tostring", luaA_keybinding_tostring },
    {"__gc", luaA_keybinding_gc },
    { NULL, NULL },
};

