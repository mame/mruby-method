#include "mruby.h"
#include "mruby/data.h"
#include "mruby/class.h"
#include "mruby/variable.h"
#include "mruby/proc.h"

static struct RObject *
method_object_alloc(
  mrb_state *mrb,
  struct RClass *mclass,
  struct RClass *owner,
  mrb_value recv,
  mrb_sym name,
  struct RProc *proc
) {
  struct RObject *c = (struct RObject*)mrb_obj_alloc(mrb, MRB_TT_CLASS, mclass);

  mrb_obj_iv_set(mrb, c, mrb_intern_lit(mrb, "@owner"), mrb_obj_value(owner));
  mrb_obj_iv_set(mrb, c, mrb_intern_lit(mrb, "@recv"), recv);
  mrb_obj_iv_set(mrb, c, mrb_intern_lit(mrb, "@name"), mrb_symbol_value(name));
  mrb_obj_iv_set(mrb, c, mrb_intern_lit(mrb, "@proc"), mrb_obj_value(proc));

  return c;
}

static mrb_value
unbound_method_bind(mrb_state *mrb, mrb_value self)
{
  struct RObject *me;
  mrb_value owner = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@owner"));
  mrb_value name = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@name"));
  mrb_value proc = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@proc"));
  mrb_value recv;

  mrb_get_args(mrb, "o", &recv);

  if (mrb_type(owner) != MRB_TT_MODULE &&
      mrb_class_ptr(owner) != mrb_obj_class(mrb, recv) &&
      !mrb_obj_is_kind_of(mrb, recv, mrb_class_ptr(owner))) {
        if (mrb_type(owner) == MRB_TT_SCLASS) {
          mrb_raise(mrb, E_TYPE_ERROR, "singleton method called for a different object");
        } else {
          const char *s = mrb_class_name(mrb, mrb_class_ptr(owner));
          mrb_raisef(mrb, E_TYPE_ERROR, "bind argument must be an instance of %S", mrb_str_new_static(mrb, s, strlen(s)));
        }
  }
  me = method_object_alloc(
    mrb,
    mrb_class_get(mrb, "Method"),
    mrb_class_ptr(owner),
    recv,
    mrb_symbol(name),
    mrb_proc_ptr(proc)
  );
  return mrb_obj_value(me);
}

static mrb_value
method_call(mrb_state *mrb, mrb_value self)
{
  mrb_value proc = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@proc"));
  mrb_value name = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@name"));
  mrb_value recv = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@recv"));
  struct RClass *owner = mrb_class_ptr(mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@owner")));
  mrb_int argc;
  mrb_value *argv, ret;
  mrb_sym orig_mid;

  mrb_get_args(mrb, "*", &argv, &argc);
  orig_mid = mrb->c->ci->mid;
  mrb->c->ci->mid = mrb_symbol(name);
  ret = mrb_yield_with_class(mrb, proc, argc, argv, recv, owner);
  mrb->c->ci->mid = orig_mid;
  return ret;
}

static mrb_value
method_unbind(mrb_state *mrb, mrb_value self)
{
  struct RObject *ume;
  mrb_value owner = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@owner"));
  mrb_value name = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@name"));
  mrb_value proc = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@proc"));
  ume = method_object_alloc(
    mrb,
    mrb_class_get(mrb, "UnboundMethod"),
    mrb_class_ptr(owner),
    mrb_nil_value(),
    mrb_symbol(name),
    mrb_proc_ptr(proc)
  );
  return mrb_obj_value(ume);
}

static mrb_value
method_super_method(mrb_state *mrb, mrb_value self)
{
  mrb_value recv = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@recv"));
  mrb_sym name = mrb_symbol(mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@name")));
  struct RClass *owner = mrb_class_ptr(mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@owner")));
  struct RClass *super = owner->super;
  struct RProc *proc;
  struct RObject *me;

  proc = mrb_method_search_vm(mrb, &super, name);
  if (!proc) return mrb_nil_value();

  me = method_object_alloc(
    mrb,
    mrb_obj_class(mrb, self),
    super,
    recv,
    name,
    proc
  );
  return mrb_obj_value(me);
}

static void
mrb_search_method_owner(mrb_state *mrb, struct RClass *c, mrb_value obj, mrb_sym name, struct RClass **owner, struct RProc **proc)
{
  *owner = c;
  *proc = mrb_method_search_vm(mrb, owner, name);
  if (!*proc) {
    mrb_sym respond_to_missing = mrb_intern_lit(mrb, "respond_to_missing?");
    mrb_value str_name = mrb_sym2str(mrb, name);
    *proc = mrb_method_search_vm(mrb, owner, respond_to_missing);
    if (*proc) {
      if (mrb_test(mrb_funcall(mrb, obj, "respond_to_missing?", 2, mrb_symbol_value(name), mrb_false_value()))) {
        *owner = c;
      }
      else {
        const char *s = mrb_class_name(mrb, c);
        mrb_raisef(mrb, E_NAME_ERROR, "undefined method `%S' for class `%S'", str_name, mrb_str_new_static(mrb, s, strlen(s)));
      }
    }
    else {
      const char *s = mrb_class_name(mrb, c);
      mrb_raisef(mrb, E_NAME_ERROR, "undefined method `%S' for class `%S'", str_name, mrb_str_new_static(mrb, s, strlen(s)));
    }
  }
}

static mrb_value
mrb_kernel_method(mrb_state *mrb, mrb_value self)
{
  struct RClass *owner;
  struct RProc *proc;
  struct RObject *me;
  mrb_sym name;

  mrb_get_args(mrb, "n", &name);

  mrb_search_method_owner(mrb, mrb_class(mrb, self), self, name, &owner, &proc);

  me = method_object_alloc(
    mrb,
    mrb_class_get(mrb, "Method"),
    owner,
    self,
    name,
    proc
  );
  return mrb_obj_value(me);
}

static mrb_value
mrb_module_instance_method(mrb_state *mrb, mrb_value self)
{
  struct RClass *owner;
  struct RProc *proc;
  struct RObject *ume;
  mrb_sym name;

  mrb_get_args(mrb, "n", &name);

  mrb_search_method_owner(mrb, mrb_class_ptr(self), self, name, &owner, &proc);

  ume = method_object_alloc(
    mrb,
    mrb_class_get(mrb, "UnboundMethod"),
    owner,
    mrb_nil_value(),
    name,
    proc
  );

  return mrb_obj_value(ume);
}

void
mrb_mruby_method_gem_init(mrb_state* mrb)
{
  struct RClass *unbound_method = mrb_define_class(mrb, "UnboundMethod", mrb->object_class);
  struct RClass *method = mrb_define_class(mrb, "Method", mrb->object_class);

  mrb_undef_class_method(mrb, unbound_method, "new");
  mrb_define_method(mrb, unbound_method, "bind", unbound_method_bind, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, unbound_method, "super_method", method_super_method, MRB_ARGS_NONE());

  mrb_undef_class_method(mrb, method, "new");
  mrb_define_method(mrb, method, "call", method_call, MRB_ARGS_ANY());
  mrb_alias_method(mrb, method, mrb_intern_lit(mrb, "[]"), mrb_intern_lit(mrb, "call"));
  mrb_define_method(mrb, method, "unbind", method_unbind, MRB_ARGS_NONE());
  mrb_define_method(mrb, method, "super_method", method_super_method, MRB_ARGS_NONE());

  mrb_define_method(mrb, mrb->kernel_module, "method", mrb_kernel_method, MRB_ARGS_REQ(1));

  mrb_define_method(mrb, mrb->module_class, "instance_method", mrb_module_instance_method, MRB_ARGS_REQ(1));
}

void
mrb_mruby_method_gem_final(mrb_state* mrb)
{
}
