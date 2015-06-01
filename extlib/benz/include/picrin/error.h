/**
 * See Copyright Notice in picrin.h
 */

#ifndef PICRIN_ERROR_H
#define PICRIN_ERROR_H

#if defined(__cplusplus)
extern "C" {
#endif

struct pic_error {
  PIC_OBJECT_HEADER
  pic_sym *type;
  pic_str *msg;
  pic_value irrs;
  pic_str *stack;
};

#define pic_error_p(v) (pic_type(v) == PIC_TT_ERROR)
#define pic_error_ptr(v) ((struct pic_error *)pic_ptr(v))

struct pic_error *pic_make_error(pic_state *, pic_sym *, const char *, pic_list);

/* do not return from try block! */

#define pic_try                                 \
  pic_try_(PIC_GENSYM(escape))
#define pic_catch                               \
  pic_catch_(PIC_GENSYM(label))
#define pic_try_(escape)                                                \
  do {                                                                  \
    struct pic_escape *escape = pic_malloc(pic, sizeof(struct pic_escape)); \
    pic_save_point(pic, escape);                                        \
    if (PIC_SETJMP(pic, (void *)escape->jmp) == 0) {                    \
      do {                                                              \
        pic_push_try(pic, pic_make_econt(pic, escape));
#define pic_catch_(label)                                 \
        pic_pop_try(pic);                                 \
      } while (0);                                        \
    } else {                                              \
      goto label;                                         \
    }                                                     \
  } while (0);                                            \
  if (0)                                                  \
  label:

void pic_push_try(pic_state *, struct pic_proc *);
void pic_pop_try(pic_state *);

pic_value pic_raise_continuable(pic_state *, pic_value);
PIC_NORETURN void pic_raise(pic_state *, pic_value);
PIC_NORETURN void pic_throw(pic_state *, pic_sym *, const char *, pic_list);
PIC_NORETURN void pic_error(pic_state *, const char *, pic_list);

#if defined(__cplusplus)
}
#endif

#endif
