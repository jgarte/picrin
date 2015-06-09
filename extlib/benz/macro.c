/**
 * See Copyright Notice in picrin.h
 */

#include "picrin.h"

pic_sym *
pic_add_rename(pic_state *pic, struct pic_env *env, pic_sym *sym)
{
  pic_sym *rename = pic_gensym(pic, sym);

  pic_put_rename(pic, env, sym, rename);

  return rename;
}

void
pic_put_rename(pic_state *pic, struct pic_env *env, pic_sym *sym, pic_sym *rename)
{
  pic_dict_set(pic, env->map, sym, pic_obj_value(rename));
}

pic_sym *
pic_find_rename(pic_state *pic, struct pic_env *env, pic_sym *sym)
{
  if (! pic_dict_has(pic, env->map, sym)) {
    return NULL;
  }
  return pic_sym_ptr(pic_dict_ref(pic, env->map, sym));
}

static void
define_macro(pic_state *pic, pic_sym *rename, struct pic_proc *mac)
{
  pic_dict_set(pic, pic->macros, rename, pic_obj_value(mac));
}

static struct pic_proc *
find_macro(pic_state *pic, pic_sym *rename)
{
  if (! pic_dict_has(pic, pic->macros, rename)) {
    return NULL;
  }
  return pic_proc_ptr(pic_dict_ref(pic, pic->macros, rename));
}

static pic_sym *
make_identifier(pic_state *pic, pic_sym *sym, struct pic_env *env)
{
  pic_sym *rename;

  while (true) {
    if ((rename = pic_find_rename(pic, env, sym)) != NULL) {
      return rename;
    }
    if (! env->up)
      break;
    env = env->up;
  }
  if (! pic_interned_p(pic, sym)) {
    return sym;
  }
  else {
    return pic_gensym(pic, sym);
  }
}

static pic_value macroexpand(pic_state *, pic_value, struct pic_env *);
static pic_value macroexpand_lambda(pic_state *, pic_value, struct pic_env *);

static pic_value
macroexpand_symbol(pic_state *pic, pic_sym *sym, struct pic_env *env)
{
  return pic_obj_value(make_identifier(pic, sym, env));
}

static pic_value
macroexpand_quote(pic_state *pic, pic_value expr)
{
  return pic_cons(pic, pic_obj_value(pic->uQUOTE), pic_cdr(pic, expr));
}

static pic_value
macroexpand_list(pic_state *pic, pic_value obj, struct pic_env *env)
{
  size_t ai = pic_gc_arena_preserve(pic);
  pic_value x, head, tail;

  if (pic_pair_p(obj)) {
    head = macroexpand(pic, pic_car(pic, obj), env);
    tail = macroexpand_list(pic, pic_cdr(pic, obj), env);
    x = pic_cons(pic, head, tail);
  } else {
    x = macroexpand(pic, obj, env);
  }

  pic_gc_arena_restore(pic, ai);
  pic_gc_protect(pic, x);
  return x;
}

static pic_value
macroexpand_defer(pic_state *pic, pic_value expr, struct pic_env *env)
{
  pic_value skel = pic_list1(pic, pic_invalid_value()); /* (#<invalid>) */

  pic_push(pic, pic_cons(pic, expr, skel), env->defer);

  return skel;
}

static void
macroexpand_deferred(pic_state *pic, struct pic_env *env)
{
  pic_value defer, val, src, dst, it;

  pic_for_each (defer, pic_reverse(pic, env->defer), it) {
    src = pic_car(pic, defer);
    dst = pic_cdr(pic, defer);

    val = macroexpand_lambda(pic, src, env);

    /* copy */
    pic_pair_ptr(dst)->car = pic_car(pic, val);
    pic_pair_ptr(dst)->cdr = pic_cdr(pic, val);
  }

  env->defer = pic_nil_value();
}

static pic_value
macroexpand_lambda(pic_state *pic, pic_value expr, struct pic_env *env)
{
  pic_value formal, body;
  struct pic_env *in;
  pic_value a;

  if (pic_length(pic, expr) < 2) {
    pic_errorf(pic, "syntax error");
  }

  in = pic_make_env(pic, env);

  for (a = pic_cadr(pic, expr); pic_pair_p(a); a = pic_cdr(pic, a)) {
    pic_value v = pic_car(pic, a);

    if (! pic_sym_p(v)) {
      pic_errorf(pic, "syntax error");
    }
    pic_add_rename(pic, in, pic_sym_ptr(v));
  }
  if (pic_sym_p(a)) {
    pic_add_rename(pic, in, pic_sym_ptr(a));
  }
  else if (! pic_nil_p(a)) {
    pic_errorf(pic, "syntax error");
  }

  formal = macroexpand_list(pic, pic_cadr(pic, expr), in);
  body = macroexpand_list(pic, pic_cddr(pic, expr), in);

  macroexpand_deferred(pic, in);

  return pic_cons(pic, pic_obj_value(pic->uLAMBDA), pic_cons(pic, formal, body));
}

static pic_value
macroexpand_define(pic_state *pic, pic_value expr, struct pic_env *env)
{
  pic_sym *sym, *rename;
  pic_value var, val;

  while (pic_length(pic, expr) >= 2 && pic_pair_p(pic_cadr(pic, expr))) {
    var = pic_car(pic, pic_cadr(pic, expr));
    val = pic_cdr(pic, pic_cadr(pic, expr));

    expr = pic_list3(pic, pic_obj_value(pic->uDEFINE), var, pic_cons(pic, pic_obj_value(pic->uLAMBDA), pic_cons(pic, val, pic_cddr(pic, expr))));
  }

  if (pic_length(pic, expr) != 3) {
    pic_errorf(pic, "syntax error");
  }

  var = pic_cadr(pic, expr);
  if (! pic_sym_p(var)) {
    pic_errorf(pic, "binding to non-symbol object");
  }
  sym = pic_sym_ptr(var);
  if ((rename = pic_find_rename(pic, env, sym)) == NULL) {
    rename = pic_add_rename(pic, env, sym);
  }
  val = macroexpand(pic, pic_list_ref(pic, expr, 2), env);

  return pic_list3(pic, pic_obj_value(pic->uDEFINE), pic_obj_value(rename), val);
}

static pic_value
macroexpand_defsyntax(pic_state *pic, pic_value expr, struct pic_env *env)
{
  pic_value var, val;
  pic_sym *sym, *rename;

  if (pic_length(pic, expr) != 3) {
    pic_errorf(pic, "syntax error");
  }

  var = pic_cadr(pic, expr);
  if (! pic_sym_p(var)) {
    pic_errorf(pic, "binding to non-symbol object");
  }
  sym = pic_sym_ptr(var);
  if ((rename = pic_find_rename(pic, env, sym)) == NULL) {
    rename = pic_add_rename(pic, env, sym);
  } else {
    pic_warnf(pic, "redefining syntax variable: ~s", pic_obj_value(sym));
  }

  val = pic_cadr(pic, pic_cdr(pic, expr));

  pic_try {
    val = pic_eval(pic, val, pic->lib);
  } pic_catch {
    pic_errorf(pic, "macroexpand error while definition: %s", pic_errmsg(pic));
  }

  if (! pic_proc_p(val)) {
    pic_errorf(pic, "macro definition \"~s\" evaluates to non-procedure object", var);
  }

  val = pic_apply1(pic, pic_proc_ptr(val), pic_obj_value(env));

  if (! pic_proc_p(val)) {
    pic_errorf(pic, "macro definition \"~s\" evaluates to non-procedure object", var);
  }

  define_macro(pic, rename, pic_proc_ptr(val));

  return pic_undef_value();
}

static pic_value
macroexpand_macro(pic_state *pic, struct pic_proc *mac, pic_value expr, struct pic_env *env)
{
  pic_value v, args;

#if DEBUG
  puts("before expand-1:");
  pic_debug(pic, expr);
  puts("");
#endif

  args = pic_list2(pic, expr, pic_obj_value(env));

  pic_try {
    v = pic_apply(pic, mac, args);
  } pic_catch {
    pic_errorf(pic, "macroexpand error while application: %s", pic_errmsg(pic));
  }

#if DEBUG
  puts("after expand-1:");
  pic_debug(pic, v);
  puts("");
#endif

  return v;
}

static pic_value
macroexpand_node(pic_state *pic, pic_value expr, struct pic_env *env)
{
  switch (pic_type(expr)) {
  case PIC_TT_SYMBOL: {
    return macroexpand_symbol(pic, pic_sym_ptr(expr), env);
  }
  case PIC_TT_PAIR: {
    pic_value car;
    struct pic_proc *mac;

    if (! pic_list_p(expr)) {
      pic_errorf(pic, "cannot macroexpand improper list: ~s", expr);
    }

    car = macroexpand(pic, pic_car(pic, expr), env);
    if (pic_sym_p(car)) {
      pic_sym *tag = pic_sym_ptr(car);

      if (tag == pic->uDEFINE_SYNTAX) {
        return macroexpand_defsyntax(pic, expr, env);
      }
      else if (tag == pic->uLAMBDA) {
        return macroexpand_defer(pic, expr, env);
      }
      else if (tag == pic->uDEFINE) {
        return macroexpand_define(pic, expr, env);
      }
      else if (tag == pic->uQUOTE) {
        return macroexpand_quote(pic, expr);
      }

      if ((mac = find_macro(pic, tag)) != NULL) {
        return macroexpand_node(pic, macroexpand_macro(pic, mac, expr, env), env);
      }
    }

    return pic_cons(pic, car, macroexpand_list(pic, pic_cdr(pic, expr), env));
  }
  default:
    return expr;
  }
}

static pic_value
macroexpand(pic_state *pic, pic_value expr, struct pic_env *env)
{
  size_t ai = pic_gc_arena_preserve(pic);
  pic_value v;

#if DEBUG
  printf("[macroexpand] expanding... ");
  pic_debug(pic, expr);
  puts("");
#endif

  v = macroexpand_node(pic, expr, env);

  pic_gc_arena_restore(pic, ai);
  pic_gc_protect(pic, v);
  return v;
}

pic_value
pic_macroexpand(pic_state *pic, pic_value expr, struct pic_lib *lib)
{
  struct pic_lib *prev;
  pic_value v;

#if DEBUG
  puts("before expand:");
  pic_debug(pic, expr);
  puts("");
#endif

  /* change library for macro-expansion time processing */
  prev = pic->lib;
  pic->lib = lib;

  lib->env->defer = pic_nil_value(); /* the last expansion could fail and leave defer field old */

  v = macroexpand(pic, expr, lib->env);

  macroexpand_deferred(pic, lib->env);

  pic->lib = prev;

#if DEBUG
  puts("after expand:");
  pic_debug(pic, v);
  puts("");
#endif

  return v;
}

struct pic_env *
pic_make_env(pic_state *pic, struct pic_env *up)
{
  struct pic_env *env;
  struct pic_dict *map;

  map = pic_make_dict(pic);

  env = (struct pic_env *)pic_obj_alloc(pic, sizeof(struct pic_env), PIC_TT_ENV);
  env->up = up;
  env->defer = pic_nil_value();
  env->map = map;

  return env;
}

static pic_value
defmacro_call(pic_state *pic)
{
  struct pic_proc *self = pic_get_proc(pic);
  pic_value args, tmp, proc;

  pic_get_args(pic, "oo", &args, &tmp);

  proc = pic_attr_ref(pic, pic_obj_value(self), "@@transformer");

  return pic_apply_trampoline(pic, pic_proc_ptr(proc), pic_cdr(pic, args));
}

void
pic_defmacro(pic_state *pic, pic_sym *name, pic_sym *id, pic_func_t func)
{
  struct pic_proc *proc, *trans;

  trans = pic_make_proc(pic, func, pic_symbol_name(pic, name));

  pic_put_rename(pic, pic->lib->env, name, id);

  proc = pic_make_proc(pic, defmacro_call, "defmacro_call");
  pic_attr_set(pic, pic_obj_value(proc), "@@transformer", pic_obj_value(trans));

  /* symbol registration */
  define_macro(pic, id, proc);

  /* auto export! */
  pic_export(pic, name);
}

bool
pic_identifier_p(pic_state *pic, pic_value obj)
{
  return pic_sym_p(obj) && ! pic_interned_p(pic, pic_sym_ptr(obj));
}

bool
pic_identifier_eq_p(pic_state *pic, struct pic_env *env1, pic_sym *sym1, struct pic_env *env2, pic_sym *sym2)
{
  pic_sym *a, *b;

  a = make_identifier(pic, sym1, env1);
  if (a != make_identifier(pic, sym1, env1)) {
    a = sym1;
  }

  b = make_identifier(pic, sym2, env2);
  if (b != make_identifier(pic, sym2, env2)) {
    b = sym2;
  }

  return pic_eq_p(pic_obj_value(a), pic_obj_value(b));
}

static pic_value
pic_macro_identifier_p(pic_state *pic)
{
  pic_value obj;

  pic_get_args(pic, "o", &obj);

  return pic_bool_value(pic_identifier_p(pic, obj));
}

static pic_value
pic_macro_make_identifier(pic_state *pic)
{
  pic_value obj;
  pic_sym *sym;

  pic_get_args(pic, "mo", &sym, &obj);

  pic_assert_type(pic, obj, env);

  return pic_obj_value(make_identifier(pic, sym, pic_env_ptr(obj)));
}

static pic_value
pic_macro_identifier_eq_p(pic_state *pic)
{
  pic_sym *sym1, *sym2;
  pic_value env1, env2;

  pic_get_args(pic, "omom", &env1, &sym1, &env2, &sym2);

  pic_assert_type(pic, env1, env);
  pic_assert_type(pic, env2, env);

  return pic_bool_value(pic_identifier_eq_p(pic, pic_env_ptr(env1), sym1, pic_env_ptr(env2), sym2));
}

void
pic_init_macro(pic_state *pic)
{
  pic_defun(pic, "identifier?", pic_macro_identifier_p);
  pic_defun(pic, "identifier=?", pic_macro_identifier_eq_p);
  pic_defun(pic, "make-identifier", pic_macro_make_identifier);
}
