/**
 * See Copyright Notice in picrin.h
 */

#include "picrin.h"

static bool
pic_var_p(pic_value obj)
{
  return pic_sym_p(obj) || pic_id_p(obj);
}

struct pic_id *
pic_make_id(pic_state *pic, pic_value var, struct pic_env *env)
{
  struct pic_id *id;

  assert(pic_var_p(var));

  id = (struct pic_id *)pic_obj_alloc(pic, sizeof(struct pic_id), PIC_TT_ID);
  id->var = var;
  id->env = env;
  return id;
}

struct pic_env *
pic_make_env(pic_state *pic, struct pic_env *up)
{
  struct pic_env *env;

  env = (struct pic_env *)pic_obj_alloc(pic, sizeof(struct pic_env), PIC_TT_ENV);
  env->up = up;
  env->defer = pic_nil_value();
  xh_init_ptr(&env->map, sizeof(pic_sym *));
  return env;
}

pic_sym *
pic_var_name(pic_state PIC_UNUSED(*pic), pic_value var)
{
  assert(pic_var_p(var));

  while (pic_id_p(var)) {
    var = pic_id_ptr(var)->var;
  }
  return pic_sym_ptr(var);
}

pic_sym *
pic_uniq(pic_state *pic, pic_value var)
{
  pic_str *str;

  assert(pic_var_p(var));

  str = pic_format(pic, "%s.%d", pic_symbol_name(pic, pic_var_name(pic, var)), pic->ucnt++);

  return pic_intern(pic, str);
}

static pic_sym *
lookup(pic_state PIC_UNUSED(*pic), pic_value var, struct pic_env *env)
{
  xh_entry *e;

  assert(pic_var_p(var));

  while (env != NULL) {
    if ((e = xh_get_ptr(&env->map, pic_ptr(var))) != NULL) {
      return xh_val(e, pic_sym *);
    }
    env = env->up;
  }
  return NULL;
}

static pic_sym *
resolve(pic_state *pic, pic_value var, struct pic_env *env)
{
  pic_sym *uid;

  assert(pic_var_p(var));

  while ((uid = lookup(pic, var, env)) == NULL) {
    if (pic_sym_p(var)) {
      return NULL;
    }
    env = pic_id_ptr(var)->env;
    var = pic_id_ptr(var)->var;
  }
  return uid;
}

pic_sym *
pic_add_variable(pic_state *pic, struct pic_env *env, pic_value var)
{
  pic_sym *uid;

  assert(pic_var_p(var));

  uid = pic_uniq(pic, var);

  pic_put_variable(pic, env, var, uid);

  return uid;
}

void
pic_put_variable(pic_state PIC_UNUSED(*pic), struct pic_env *env, pic_value var, pic_sym *uid)
{
  assert(pic_var_p(var));

  xh_put_ptr(&env->map, pic_ptr(var), &uid);
}

pic_sym *
pic_find_variable(pic_state PIC_UNUSED(*pic), struct pic_env *env, pic_value var)
{
  xh_entry *e;

  assert(pic_var_p(var));

  if ((e = xh_get_ptr(&env->map, pic_ptr(var))) == NULL) {
    return NULL;
  }
  return xh_val(e, pic_sym *);
}

static void
define_macro(pic_state *pic, pic_sym *uid, struct pic_proc *mac)
{
  pic_dict_set(pic, pic->macros, uid, pic_obj_value(mac));
}

static struct pic_proc *
find_macro(pic_state *pic, pic_sym *uid)
{
  if (! pic_dict_has(pic, pic->macros, uid)) {
    return NULL;
  }
  return pic_proc_ptr(pic_dict_ref(pic, pic->macros, uid));
}

static pic_value macroexpand(pic_state *, pic_value, struct pic_env *);
static pic_value macroexpand_lambda(pic_state *, pic_value, struct pic_env *);

static pic_value
macroexpand_var(pic_state *pic, pic_value var, struct pic_env *env)
{
  pic_sym *uid;

  if ((uid = resolve(pic, var, env)) == NULL) {
    pic_errorf(pic, "unbound variable found: ~s", var);
  }
  return pic_obj_value(uid);
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
    pic_value var = pic_car(pic, a);

    if (! pic_var_p(var)) {
      pic_errorf(pic, "syntax error");
    }
    pic_add_variable(pic, in, var);
  }
  if (pic_var_p(a)) {
    pic_add_variable(pic, in, a);
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
  pic_sym *uid;
  pic_value var, val;

  while (pic_length(pic, expr) >= 2 && pic_pair_p(pic_cadr(pic, expr))) {
    var = pic_car(pic, pic_cadr(pic, expr));
    val = pic_cdr(pic, pic_cadr(pic, expr));

    expr = pic_list3(pic, pic_obj_value(pic->uDEFINE), var, pic_cons(pic, pic_obj_value(pic->sLAMBDA), pic_cons(pic, val, pic_cddr(pic, expr))));
  }

  if (pic_length(pic, expr) != 3) {
    pic_errorf(pic, "syntax error");
  }

  var = pic_cadr(pic, expr);
  if (! pic_var_p(var)) {
    pic_errorf(pic, "binding to non-variable object");
  }
  if ((uid = pic_find_variable(pic, env, var)) == NULL) {
    uid = pic_add_variable(pic, env, var);
  }
  val = macroexpand(pic, pic_list_ref(pic, expr, 2), env);

  return pic_list3(pic, pic_obj_value(pic->uDEFINE), pic_obj_value(uid), val);
}

static pic_value
macroexpand_defmacro(pic_state *pic, pic_value expr, struct pic_env *env)
{
  pic_value var, val;
  pic_sym *uid;

  if (pic_length(pic, expr) != 3) {
    pic_errorf(pic, "syntax error");
  }

  var = pic_cadr(pic, expr);
  if (! pic_var_p(var)) {
    pic_errorf(pic, "binding to non-variable object");
  }
  if ((uid = pic_find_variable(pic, env, var)) == NULL) {
    uid = pic_add_variable(pic, env, var);
  } else {
    pic_warnf(pic, "redefining syntax variable: ~s", var);
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

  define_macro(pic, uid, pic_proc_ptr(val));

  return pic_undef_value();
}

static pic_value
macroexpand_macro(pic_state *pic, struct pic_proc *mac, pic_value expr, struct pic_env *env)
{
  pic_value v;

#if DEBUG
  puts("before expand-1:");
  pic_debug(pic, expr);
  puts("");
#endif

  pic_try {
    v = pic_apply2(pic, mac, expr, pic_obj_value(env));
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
  case PIC_TT_ID:
  case PIC_TT_SYMBOL: {
    return macroexpand_var(pic, expr, env);
  }
  case PIC_TT_PAIR: {
    struct pic_proc *mac;

    if (! pic_list_p(expr)) {
      pic_errorf(pic, "cannot macroexpand improper list: ~s", expr);
    }

    if (pic_var_p(pic_car(pic, expr))) {
      pic_sym *functor;

      if ((functor = resolve(pic, pic_car(pic, expr), env)) == NULL) {
        goto call;
      }

      if (functor == pic->uDEFINE_MACRO) {
        return macroexpand_defmacro(pic, expr, env);
      }
      else if (functor == pic->uLAMBDA) {
        return macroexpand_defer(pic, expr, env);
      }
      else if (functor == pic->uDEFINE) {
        return macroexpand_define(pic, expr, env);
      }
      else if (functor == pic->uQUOTE) {
        return macroexpand_quote(pic, expr);
      }

      if ((mac = find_macro(pic, functor)) != NULL) {
        return macroexpand_node(pic, macroexpand_macro(pic, mac, expr, env), env);
      }
    }
    call:

    return macroexpand_list(pic, expr, env);
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

  pic_put_variable(pic, pic->lib->env, pic_obj_value(name), id);

  proc = pic_make_proc(pic, defmacro_call, "defmacro_call");
  pic_attr_set(pic, pic_obj_value(proc), "@@transformer", pic_obj_value(trans));

  /* symbol registration */
  define_macro(pic, id, proc);

  /* auto export! */
  pic_export(pic, name);
}

static pic_value
pic_macro_identifier_p(pic_state *pic)
{
  pic_value obj;

  pic_get_args(pic, "o", &obj);

  return pic_bool_value(pic_id_p(obj));
}

static pic_value
pic_macro_make_identifier(pic_state *pic)
{
  pic_value var, env;

  pic_get_args(pic, "oo", &var, &env);

  pic_assert_type(pic, var, var);
  pic_assert_type(pic, env, env);

  return pic_obj_value(pic_make_id(pic, var, pic_env_ptr(env)));
}

static pic_value
pic_macro_variable_p(pic_state *pic)
{
  pic_value obj;

  pic_get_args(pic, "o", &obj);

  return pic_bool_value(pic_var_p(obj));
}

static pic_value
pic_macro_variable_eq_p(pic_state *pic)
{
  pic_value var1, var2;
  pic_sym *uid1, *uid2;

  pic_get_args(pic, "oo", &var1, &var2);

  pic_assert_type(pic, var1, var);
  pic_assert_type(pic, var2, var);

  if (pic_eq_p(var1, var2)) {
    return pic_true_value();
  }

  uid1 = resolve(pic, var1, NULL);
  uid2 = resolve(pic, var2, NULL);

  if (uid1 || uid2) {
    return pic_bool_value(uid1 == uid2);
  }
  return pic_false_value();
}

void
pic_init_macro(pic_state *pic)
{
  pic_defun(pic, "identifier?", pic_macro_identifier_p);
  pic_defun(pic, "make-identifier", pic_macro_make_identifier);

  pic_defun(pic, "variable?", pic_macro_variable_p);
  pic_defun(pic, "variable=?", pic_macro_variable_eq_p);
}
