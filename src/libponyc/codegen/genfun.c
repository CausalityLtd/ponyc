#include "genfun.h"
#include "genname.h"
#include "gencall.h"
#include "gentrace.h"
#include "gencontrol.h"
#include "genexpr.h"
#include "genreference.h"
#include "../pass/names.h"
#include "../type/assemble.h"
#include "../type/subtype.h"
#include "../type/reify.h"
#include "../type/lookup.h"
#include "../../libponyrt/ds/fun.h"
#include "../../libponyrt/mem/pool.h"
#include "../../libponyrt/mem/heap.h"
#include "ponyassert.h"
#include <string.h>

static void compile_method_free(void* p)
{
  POOL_FREE(compile_method_t, p);
}

static void name_param(compile_t* c, reach_type_t* t, reach_method_t* m,
  LLVMValueRef func, const char* name, unsigned index, size_t line, size_t pos)
{
  compile_type_t* c_t = (compile_type_t*)t->c_type;
  compile_method_t* c_m = (compile_method_t*)m->c_method;

  LLVMValueRef value = LLVMGetParam(func, index);
  LLVMSetValueName(value, name);

  LLVMValueRef alloc = LLVMBuildAlloca(c->builder, c_t->mem_type, name);
  value = gen_assign_cast(c, c_t->mem_type, value, t->ast_cap);
  LLVMBuildStore(c->builder, value, alloc);
  codegen_setlocal(c, name, alloc);

  LLVMMetadataRef info;

  if(index == 0)
  {
    info = LLVMDIBuilderCreateArtificialVariable(c->di,
      c_m->di_method, name, index + 1, c_m->di_file,
      (unsigned)ast_line(m->fun->ast), c_t->di_type);
  } else {
    info = LLVMDIBuilderCreateParameterVariable(c->di, c_m->di_method,
      name, strlen(name), index + 1, c_m->di_file,
      (unsigned)ast_line(m->fun->ast), c_t->di_type, false, LLVMDIFlagZero);
  }

  LLVMMetadataRef expr = LLVMDIBuilderCreateExpression(c->di, NULL, 0);

  LLVMDIBuilderInsertDeclare(c->di, alloc, info, expr,
    (unsigned)line, (unsigned)pos, c_m->di_method,
    LLVMGetInsertBlock(c->builder));
}

static void name_params(compile_t* c, reach_type_t* t, reach_method_t* m,
  LLVMValueRef func)
{
  unsigned offset = 0;

  if(m->cap != TK_AT)
  {
    // Name the receiver 'this'.
    name_param(c, t, m, func, c->str_this, 0, ast_line(m->fun->ast),
      ast_pos(m->fun->ast));
    offset = 1;
  }

  ast_t* params = ast_childidx(m->fun->ast, 3);
  ast_t* param = ast_child(params);

  // Name each parameter.
  for(size_t i = 0; i < m->param_count; i++)
  {
    reach_param_t* r_param = &m->params[i];
    name_param(c, r_param->type, m, func, r_param->name, (unsigned)i + offset,
      ast_line(param), ast_pos(param));

    param = ast_sibling(param);
  }
}

static void make_signature(compile_t* c, reach_type_t* t,
  reach_method_name_t* n, reach_method_t* m, bool message_type)
{
  // Count the parameters, including the receiver if the method isn't bare.
  size_t count = m->param_count;
  size_t offset = 0;
  if(m->cap != TK_AT)
  {
    count++;
    offset++;
  }

  size_t tparam_size = count * sizeof(LLVMTypeRef);

  if(message_type)
  {
    tparam_size += tparam_size + (2 * sizeof(LLVMTypeRef));

    // must be at least the size of 3 or else we get a heap buffer overflow
    if((2 * sizeof(LLVMTypeRef)) == tparam_size)
      tparam_size += tparam_size + sizeof(LLVMTypeRef);
  }

  LLVMTypeRef* tparams = (LLVMTypeRef*)ponyint_pool_alloc_size(tparam_size);
  LLVMTypeRef* mparams = NULL;

  if(message_type)
    mparams = &tparams[count];

  bool bare_void = false;
  compile_type_t* c_t = (compile_type_t*)t->c_type;
  compile_method_t* c_m = (compile_method_t*)m->c_method;

  if(m->cap == TK_AT)
  {
    bare_void = is_none(m->result->ast);
  } else {
    // Get a type for the receiver.
    tparams[0] = c_t->use_type;
  }

  // Get a type for each parameter.
  for(size_t i = 0; i < m->param_count; i++)
  {
    compile_type_t* p_c_t = (compile_type_t*)m->params[i].type->c_type;
    tparams[i + offset] = p_c_t->use_type;

    if(message_type)
      mparams[i + offset + 2] = p_c_t->mem_type;
  }

  // Generate the function type.
  // Bare methods returning None return void to maintain compatibility with C.
  // Class constructors return void to avoid clobbering nocapture information.
  if(bare_void || (n->name == c->str__final) ||
    ((ast_id(m->fun->ast) == TK_NEW) && (t->underlying == TK_CLASS)))
    c_m->func_type = LLVMFunctionType(c->void_type, tparams, (int)count, false);
  else
    c_m->func_type = LLVMFunctionType(
      ((compile_type_t*)m->result->c_type)->use_type, tparams, (int)count,
      false);

  if(message_type)
  {
    mparams[0] = c->i32;
    mparams[1] = c->i32;
    mparams[2] = c->ptr;

    c_m->msg_type = LLVMStructTypeInContext(c->context, mparams, (int)count + 2,
      false);
  }

  ponyint_pool_free_size(tparam_size, tparams);
}

static void make_function_debug(compile_t* c, reach_type_t* t,
  reach_method_t* m, LLVMValueRef func)
{
  // Count the parameters, including the receiver and the result.
  size_t count = m->param_count + 1;
  size_t offset = 1;
  if(m->cap != TK_AT)
  {
    count++;
    offset++;
  }

  size_t md_size = count * sizeof(reach_type_t*);
  LLVMMetadataRef* md = (LLVMMetadataRef*)ponyint_pool_alloc_size(md_size);
  compile_type_t* c_t = (compile_type_t*)t->c_type;
  compile_method_t* c_m = (compile_method_t*)m->c_method;

  md[0] = ((compile_type_t*)m->result->c_type)->di_type;
  if(m->cap != TK_AT)
    md[1] = c_t->di_type;

  for(size_t i = 0; i < m->param_count; i++)
    md[i + offset] = ((compile_type_t*)m->params[i].type->c_type)->di_type;

  c_m->di_file = c_t->di_file;

  LLVMMetadataRef subroutine = LLVMDIBuilderCreateSubroutineType(c->di,
    c_m->di_file, md, (unsigned int)count, LLVMDIFlagZero);

  LLVMMetadataRef scope;

  if(c_t->di_type_embed != NULL)
    scope = c_t->di_type_embed;
  else
    scope = c_t->di_type;

#ifdef _MSC_VER
  // CodeView on Windows doesn't like "non-class" methods
  if (c_t->primitive != NULL)
  {
    scope = LLVMDIBuilderCreateNamespace(c->di, c->di_unit, t->name,
      c_t->di_file, (unsigned)ast_line(t->ast));
  }
#endif

  ast_t* id = ast_childidx(m->fun->ast, 1);

  c_m->di_method = LLVMDIBuilderCreateMethod(c->di, scope, ast_name(id),
    m->full_name, c_m->di_file, (unsigned)ast_line(m->fun->ast), subroutine,
    func, c->opt->release);

  ponyint_pool_free_size(md_size, md);
}

static void make_prototype(compile_t* c, reach_type_t* t,
  reach_method_name_t* n, reach_method_t* m)
{
  if(m->intrinsic)
    return;

  // Behaviours and actor constructors also have handler functions.
  bool handler = false;
  bool is_trait = false;

  switch(ast_id(m->fun->ast))
  {
    case TK_NEW:
      handler = t->underlying == TK_ACTOR;
      break;

    case TK_BE:
      handler = true;
      break;

    default: {}
  }

  switch(t->underlying)
  {
    case TK_PRIMITIVE:
    case TK_STRUCT:
    case TK_CLASS:
    case TK_ACTOR:
      break;

    case TK_UNIONTYPE:
    case TK_ISECTTYPE:
    case TK_INTERFACE:
    case TK_TRAIT:
      is_trait = true;
      break;

    default:
      pony_assert(0);
      return;
  }

  make_signature(c, t, n, m, handler || is_trait);

  if(is_trait)
    return;

  compile_method_t* c_m = (compile_method_t*)m->c_method;

  if(!handler)
  {
    // Generate the function prototype.
    c_m->func = codegen_addfun(c, m->full_name, c_m->func_type, true);
    genfun_param_attrs(c, t, m, c_m->func);
    make_function_debug(c, t, m, c_m->func);
  } else {
    size_t count = LLVMCountParamTypes(c_m->func_type);
    size_t buf_size = count * sizeof(LLVMTypeRef);
    LLVMTypeRef* tparams = (LLVMTypeRef*)ponyint_pool_alloc_size(buf_size);
    LLVMGetParamTypes(c_m->func_type, tparams);

    // Generate the sender prototype.
    const char* sender_name = genname_be(m->full_name);
    c_m->func = codegen_addfun(c, sender_name, c_m->func_type, true);
    genfun_param_attrs(c, t, m, c_m->func);

    // If the method is a forwarding mangling, we don't need the handler.
    if(!m->forwarding)
    {
      // Change the return type to void for the handler.
      LLVMTypeRef handler_type = LLVMFunctionType(c->void_type, tparams,
        (int)count, false);

      // Generate the handler prototype.
      c_m->func_handler = codegen_addfun(c, m->full_name, handler_type, true);
      genfun_param_attrs(c, t, m, c_m->func_handler);
      make_function_debug(c, t, m, c_m->func_handler);
    }

    ponyint_pool_free_size(buf_size, tparams);
  }

  compile_type_t* c_t = (compile_type_t*)t->c_type;

  if(n->name == c->str__final)
  {
    // Store the finaliser and use the C calling convention and an external
    // linkage.
    pony_assert(c_t->final_fn == NULL);
    c_t->final_fn = c_m->func;
    LLVMSetFunctionCallConv(c_m->func, LLVMCCallConv);
    LLVMSetLinkage(c_m->func, LLVMExternalLinkage);
  } else if(n->name == c->str__serialise_space) {
    c_t->custom_serialise_space_fn = c_m->func;
    LLVMSetFunctionCallConv(c_m->func, LLVMCCallConv);
    LLVMSetLinkage(c_m->func, LLVMExternalLinkage);
  } else if(n->name == c->str__serialise) {
    c_t->custom_serialise_fn = c_m->func;
  } else if(n->name == c->str__deserialise) {
    c_t->custom_deserialise_fn = c_m->func;
    LLVMSetFunctionCallConv(c_m->func, LLVMCCallConv);
    LLVMSetLinkage(c_m->func, LLVMExternalLinkage);
  }

  if(n->cap == TK_AT)
  {
    LLVMSetFunctionCallConv(c_m->func, LLVMCCallConv);
    LLVMSetLinkage(c_m->func, LLVMExternalLinkage);
    LLVMSetUnnamedAddr(c_m->func, false);

    if(t->bare_method == m)
    {
      pony_assert(c_t->instance == NULL);
      c_t->instance = c_m->func;
    }
  }
}

static void add_dispatch_case(compile_t* c, reach_type_t* t,
  reach_param_t* params, uint32_t index, LLVMValueRef handler,
  LLVMTypeRef fun_type, LLVMTypeRef msg_type)
{
  // Add a case to the dispatch function to handle this message.
  compile_type_t* c_t = (compile_type_t*)t->c_type;
  codegen_startfun(c, c_t->dispatch_fn, NULL, NULL, NULL, false);
  LLVMBasicBlockRef block = codegen_block(c, "handler");
  LLVMValueRef id = LLVMConstInt(c->i32, index, false);
  LLVMAddCase(c_t->dispatch_switch, id, block);

  // Destructure the message.
  LLVMPositionBuilderAtEnd(c->builder, block);
  LLVMValueRef ctx = LLVMGetParam(c_t->dispatch_fn, 0);
  LLVMValueRef this_ptr = LLVMGetParam(c_t->dispatch_fn, 1);
  LLVMValueRef msg = LLVMGetParam(c_t->dispatch_fn, 2);

  size_t count = LLVMCountParamTypes(fun_type);
  size_t params_buf_size = count * sizeof(LLVMTypeRef);
  LLVMTypeRef* param_types =
    (LLVMTypeRef*)ponyint_pool_alloc_size(params_buf_size);
  LLVMGetParamTypes(fun_type, param_types);

  size_t args_buf_size = count * sizeof(LLVMValueRef);
  LLVMValueRef* args = (LLVMValueRef*)ponyint_pool_alloc_size(args_buf_size);
  args[0] = this_ptr;

  for(int i = 1; i < (int)count; i++)
  {
    LLVMValueRef field = LLVMBuildStructGEP2(c->builder, msg_type, msg, i + 2,
      "");
    LLVMTypeRef field_type = LLVMStructGetTypeAtIndex(msg_type, i + 2);
    args[i] = LLVMBuildLoad2(c->builder, field_type, field, "");
    args[i] = gen_assign_cast(c, param_types[i], args[i],
      params[i - 1].type->ast_cap);
  }

  // Trace the message.
  bool need_trace = false;

  for(size_t i = 0; i < count - 1; i++)
  {
    if(gentrace_needed(c, params[i].ast, params[i].ast))
    {
      need_trace = true;
      break;
    }
  }

  if(need_trace)
  {
    gencall_runtime(c, "pony_gc_recv", &ctx, 1, "");

    for(size_t i = 1; i < count; i++)
      gentrace(c, ctx, args[i], args[i], params[i - 1].ast, params[i - 1].ast);

    gencall_runtime(c, "pony_recv_done", &ctx, 1, "");
  }

  // Call the handler.
  codegen_call(c, LLVMGlobalGetValueType(handler), handler, args, count, true);
  genfun_build_ret_void(c);
  codegen_finishfun(c);
  ponyint_pool_free_size(args_buf_size, args);
  ponyint_pool_free_size(params_buf_size, param_types);
}

static void call_embed_finalisers(compile_t* c, reach_type_t* t,
  ast_t* call_location, LLVMValueRef obj)
{
  compile_type_t* c_t = (compile_type_t*)t->c_type;
  uint32_t base = 0;
  if(t->underlying != TK_STRUCT)
    base++;

  if(t->underlying == TK_ACTOR)
    base++;

  for(uint32_t i = 0; i < t->field_count; i++)
  {
    reach_field_t* field = &t->fields[i];
    if(!field->embed)
      continue;

    LLVMValueRef final_fn = ((compile_type_t*)field->type->c_type)->final_fn;
    if(final_fn == NULL)
      continue;

    LLVMTypeRef final_fn_type = LLVMGlobalGetValueType(final_fn);

    LLVMValueRef field_ref = LLVMBuildStructGEP2(c->builder, c_t->structure,
      obj, base + i, "");
    codegen_debugloc(c, call_location);
    LLVMBuildCall2(c->builder, final_fn_type, final_fn, &field_ref, 1, "");
    codegen_debugloc(c, NULL);
  }
}

static bool
genfun_has_terminator(compile_t* c)
{
  LLVMBasicBlockRef current_block = LLVMGetInsertBlock(c->builder);

  pony_assert(current_block);

  return LLVMGetBasicBlockTerminator(current_block);
}

LLVMValueRef
genfun_build_ret(compile_t* c, LLVMValueRef v)
{
  if (!genfun_has_terminator(c)) {
    return LLVMBuildRet(c->builder, v);
  }

  return NULL;
}

LLVMValueRef
genfun_build_ret_void(compile_t* c)
{
  if (!genfun_has_terminator(c)) {
    return LLVMBuildRetVoid(c->builder);
  }

  return NULL;
}

static bool genfun_fun(compile_t* c, reach_type_t* t, reach_method_t* m)
{
  compile_type_t* c_t = (compile_type_t*)t->c_type;
  compile_method_t* c_m = (compile_method_t*)m->c_method;
  pony_assert(c_m->func != NULL);

  AST_GET_CHILDREN(m->fun->ast, cap, id, typeparams, params, result, can_error,
    body);

  codegen_startfun(c, c_m->func, c_m->di_file, c_m->di_method, m->fun,
    ast_id(cap) == TK_AT);
  name_params(c, t, m, c_m->func);

  bool finaliser = c_m->func == c_t->final_fn;

  if(finaliser)
    call_embed_finalisers(c, t, body, gen_this(c, NULL));

  LLVMValueRef value = gen_expr(c, body);

  if(value == NULL)
    return false;

  if(value != GEN_NOVALUE)
  {
    ast_t* r_result = deferred_reify(m->fun, result, c->opt);

    if(finaliser || ((ast_id(cap) == TK_AT) && is_none(r_result)))
    {
      ast_free_unattached(r_result);
      codegen_scope_lifetime_end(c);
      codegen_debugloc(c, ast_childlast(body));

      genfun_build_ret_void(c);
    } else {
      LLVMTypeRef f_type = LLVMGlobalGetValueType(c_m->func);
      LLVMTypeRef r_type = LLVMGetReturnType(f_type);

      ast_t* body_type = deferred_reify(m->fun, ast_type(body), c->opt);
      LLVMValueRef ret = gen_assign_cast(c, r_type, value, body_type);

      ast_free_unattached(body_type);
      ast_free_unattached(r_result);

      if(ret == NULL)
        return false;

      codegen_scope_lifetime_end(c);
      codegen_debugloc(c, ast_childlast(body));

      genfun_build_ret(c, ret);
    }

    codegen_debugloc(c, NULL);
  }

  codegen_finishfun(c);

  return true;
}

static bool genfun_be(compile_t* c, reach_type_t* t, reach_method_t* m)
{
  compile_method_t* c_m = (compile_method_t*)m->c_method;
  pony_assert(c_m->func != NULL);
  pony_assert(c_m->func_handler != NULL);

  AST_GET_CHILDREN(m->fun->ast, cap, id, typeparams, params, result, can_error,
    body);

  // Generate the handler.
  codegen_startfun(c, c_m->func_handler, c_m->di_file, c_m->di_method, m->fun,
    false);
  name_params(c, t, m, c_m->func_handler);

  LLVMValueRef value = gen_expr(c, body);

  if(value == NULL)
    return false;

  codegen_scope_lifetime_end(c);
  if(value != GEN_NOVALUE)
    genfun_build_ret_void(c);

  codegen_finishfun(c);

  // Generate the sender.
  codegen_startfun(c, c_m->func, NULL, NULL, m->fun, false);
  size_t buf_size = (m->param_count + 1) * sizeof(LLVMValueRef);
  LLVMValueRef* param_vals = (LLVMValueRef*)ponyint_pool_alloc_size(buf_size);
  LLVMGetParams(c_m->func, param_vals);

  // Send the arguments in a message to 'this'.
  gen_send_message(c, m, param_vals, params);

  // Return None.
  genfun_build_ret(c, c->none_instance);
  codegen_finishfun(c);

  ponyint_pool_free_size(buf_size, param_vals);

  // Add the dispatch case.
  add_dispatch_case(c, t, m->params, m->vtable_index, c_m->func_handler,
    c_m->func_type, c_m->msg_type);

  return true;
}

static bool genfun_new(compile_t* c, reach_type_t* t, reach_method_t* m)
{
  compile_type_t* c_t = (compile_type_t*)t->c_type;
  compile_method_t* c_m = (compile_method_t*)m->c_method;
  pony_assert(c_m->func != NULL);

  AST_GET_CHILDREN(m->fun->ast, cap, id, typeparams, params, result, can_error,
    body);

  codegen_startfun(c, c_m->func, c_m->di_file, c_m->di_method, m->fun, false);
  name_params(c, t, m, c_m->func);

  LLVMValueRef value = gen_expr(c, body);

  if(value == NULL)
    return false;

  // Return 'this'.
  if(c_t->primitive == NULL)
    value = LLVMGetParam(c_m->func, 0);

  codegen_scope_lifetime_end(c);
  codegen_debugloc(c, ast_childlast(body));
  if(t->underlying == TK_CLASS)
    genfun_build_ret_void(c);
  else
    genfun_build_ret(c, value);
  codegen_debugloc(c, NULL);

  codegen_finishfun(c);

  return true;
}

static bool genfun_newbe(compile_t* c, reach_type_t* t, reach_method_t* m)
{
  compile_method_t* c_m = (compile_method_t*)m->c_method;
  pony_assert(c_m->func != NULL);
  pony_assert(c_m->func_handler != NULL);

  AST_GET_CHILDREN(m->fun->ast, cap, id, typeparams, params, result, can_error,
    body);

  // Generate the handler.
  codegen_startfun(c, c_m->func_handler, c_m->di_file, c_m->di_method, m->fun,
    false);
  name_params(c, t, m, c_m->func_handler);

  LLVMValueRef value = gen_expr(c, body);

  if(value == NULL)
    return false;

  codegen_scope_lifetime_end(c);
  genfun_build_ret_void(c);
  codegen_finishfun(c);

  // Generate the sender.
  codegen_startfun(c, c_m->func, NULL, NULL, m->fun, false);
  size_t buf_size = (m->param_count + 1) * sizeof(LLVMValueRef);
  LLVMValueRef* param_vals = (LLVMValueRef*)ponyint_pool_alloc_size(buf_size);
  LLVMGetParams(c_m->func, param_vals);

  // Send the arguments in a message to 'this'.
  gen_send_message(c, m, param_vals, params);

  // Return 'this'.
  genfun_build_ret(c, param_vals[0]);
  codegen_finishfun(c);

  ponyint_pool_free_size(buf_size, param_vals);

  // Add the dispatch case.
  add_dispatch_case(c, t, m->params, m->vtable_index, c_m->func_handler,
    c_m->func_type, c_m->msg_type);

  return true;
}

static void copy_subordinate(reach_method_t* m)
{
  compile_method_t* c_m = (compile_method_t*)m->c_method;
  reach_method_t* m2 = m->subordinate;

  while(m2 != NULL)
  {
    compile_method_t* c_m2 = (compile_method_t*)m2->c_method;
    c_m2->func_type = c_m->func_type;
    c_m2->func = c_m->func;
    m2 = m2->subordinate;
  }
}

static void genfun_implicit_final_prototype(compile_t* c, reach_type_t* t,
  reach_method_t* m)
{
  compile_type_t* c_t = (compile_type_t*)t->c_type;
  compile_method_t* c_m = (compile_method_t*)m->c_method;

  c_m->func_type = LLVMFunctionType(c->void_type, &c_t->use_type, 1, false);
  c_m->func = codegen_addfun(c, m->full_name, c_m->func_type, true);

  c_t->final_fn = c_m->func;
  LLVMSetFunctionCallConv(c_m->func, LLVMCCallConv);
  LLVMSetLinkage(c_m->func, LLVMExternalLinkage);
}

static bool genfun_implicit_final(compile_t* c, reach_type_t* t,
  reach_method_t* m)
{
  compile_method_t* c_m = (compile_method_t*)m->c_method;

  codegen_startfun(c, c_m->func, NULL, NULL, NULL, false);
  call_embed_finalisers(c, t, NULL, gen_this(c, NULL));
  genfun_build_ret_void(c);
  codegen_finishfun(c);

  return true;
}

static bool genfun_allocator(compile_t* c, reach_type_t* t)
{
  switch(t->underlying)
  {
    case TK_PRIMITIVE:
    case TK_STRUCT:
    case TK_CLASS:
    case TK_ACTOR:
      break;

    default:
      return true;
  }

  compile_type_t* c_t = (compile_type_t*)t->c_type;

  // No allocator for machine word types or pointers.
  if((c_t->primitive != NULL) || is_pointer(t->ast) || is_nullable_pointer(t->ast))
    return true;

  const char* funname = genname_alloc(t->name);
  LLVMTypeRef ftype = LLVMFunctionType(c_t->use_type, NULL, 0, false);
  LLVMValueRef fun = codegen_addfun(c, funname, ftype, true);
  if(t->underlying != TK_PRIMITIVE)
  {
    LLVM_DECLARE_ATTRIBUTEREF(noalias_attr, noalias, 0);
    LLVM_DECLARE_ATTRIBUTEREF(align_attr, align, HEAP_MIN);

    LLVMAddAttributeAtIndex(fun, LLVMAttributeReturnIndex, noalias_attr);
    LLVMAddAttributeAtIndex(fun, LLVMAttributeReturnIndex, align_attr);

    size_t size = (size_t)LLVMABISizeOfType(c->target_data, c_t->structure);
    if (size > 0) {
      LLVM_DECLARE_ATTRIBUTEREF(deref_attr, dereferenceable, size);
      LLVMAddAttributeAtIndex(fun, LLVMAttributeReturnIndex, deref_attr);
    }
  }
  codegen_startfun(c, fun, NULL, NULL, NULL, false);

  LLVMValueRef result;

  switch(t->underlying)
  {
    case TK_PRIMITIVE:
    case TK_STRUCT:
    case TK_CLASS:
      // Allocate the object or return the global instance.
      result = gencall_alloc(c, t, NULL);
      break;

    case TK_ACTOR:
      // Allocate the actor.
      result = gencall_create(c, t, NULL);
      break;

    default:
      pony_assert(0);
      return false;
  }

  genfun_build_ret(c, result);
  codegen_finishfun(c);
  return true;
}

static bool genfun_forward(compile_t* c, reach_type_t* t,
  reach_method_name_t* n, reach_method_t* m)
{
  compile_method_t* c_m = (compile_method_t*)m->c_method;
  pony_assert(c_m->func != NULL);

  reach_method_t* m2 = reach_method(t, m->cap, n->name, m->typeargs);
  pony_assert(m2 != NULL);
  pony_assert(m2 != m);
  compile_method_t* c_m2 = (compile_method_t*)m2->c_method;

  codegen_startfun(c, c_m->func, c_m->di_file, c_m->di_method, m->fun,
    m->cap == TK_AT);

  int count = LLVMCountParams(c_m->func);
  size_t buf_size = count * sizeof(LLVMValueRef);

  LLVMValueRef* args = (LLVMValueRef*)ponyint_pool_alloc_size(buf_size);
  args[0] = LLVMGetParam(c_m->func, 0);

  for(int i = 1; i < count; i++)
  {
    LLVMValueRef value = LLVMGetParam(c_m->func, i);
    args[i] = gen_assign_cast(c,
      ((compile_type_t*)m2->params[i - 1].type->c_type)->use_type, value,
      m->params[i - 1].type->ast_cap);
  }

  codegen_debugloc(c, m2->fun->ast);
  LLVMValueRef ret = codegen_call(c, LLVMGlobalGetValueType(c_m2->func),
    c_m2->func, args, count, m->cap != TK_AT);
  codegen_debugloc(c, NULL);
  ret = gen_assign_cast(c, ((compile_type_t*)m->result->c_type)->use_type, ret,
    m2->result->ast_cap);
  genfun_build_ret(c, ret);
  codegen_finishfun(c);
  ponyint_pool_free_size(buf_size, args);
  return true;
}

static bool genfun_method(compile_t* c, reach_type_t* t,
  reach_method_name_t* n, reach_method_t* m)
{
  if(m->intrinsic)
  {
    if(m->internal && (n->name == c->str__final))
    {
      if(!genfun_implicit_final(c, t, m))
        return false;
    }
  } else if(m->forwarding) {
    if(!genfun_forward(c, t, n, m))
      return false;
  } else {
    switch(ast_id(m->fun->ast))
    {
      case TK_NEW:
        if(t->underlying == TK_ACTOR)
        {
          if(!genfun_newbe(c, t, m))
            return false;
        } else {
          if(!genfun_new(c, t, m))
            return false;
        }
        break;

      case TK_BE:
        if(!genfun_be(c, t, m))
          return false;
        break;

      case TK_FUN:
        if(!genfun_fun(c, t, m))
          return false;
        break;

      default:
        pony_assert(0);
        return false;
    }
  }

  return true;
}

static void genfun_param_deref_attr(compile_t* c, LLVMValueRef fun,
  LLVMValueRef param, unsigned int param_index, compile_type_t* param_c_t)
{
  LLVMTypeRef param_type = LLVMTypeOf(param);

  // If the parameter is a pointer to an object, tell LLVM the size of the
  // object in bytes that are dereferenceable via that pointer, if we know.
  if (LLVMGetTypeKind(param_type) == LLVMPointerTypeKind) {
    LLVMTypeRef structure = param_c_t->structure;
    if (structure != NULL) {
      size_t size = (size_t)LLVMABISizeOfType(c->target_data, structure);
      if (size > 0) {
        LLVM_DECLARE_ATTRIBUTEREF(deref_attr, dereferenceable, size);
        LLVMAddAttributeAtIndex(fun, param_index, deref_attr);
      }
    }
  }
}

void genfun_param_attrs(compile_t* c, reach_type_t* t, reach_method_t* m,
  LLVMValueRef fun)
{
  LLVM_DECLARE_ATTRIBUTEREF(noalias_attr, noalias, 0);
  LLVM_DECLARE_ATTRIBUTEREF(readonly_attr, readonly, 0);

  LLVMValueRef param = LLVMGetFirstParam(fun);
  reach_type_t* type = t;
  token_id cap = m->cap;
  int i = 0;
  int offset = 1;

  if(cap == TK_AT)
  {
    i = 1;
    offset = 0;
  }

  while(param != NULL)
  {
    LLVMTypeRef m_type = LLVMTypeOf(param);
    if(LLVMGetTypeKind(m_type) == LLVMPointerTypeKind)
    {
      if(i > 0)
      {
        type = m->params[i-1].type;
        cap = m->params[i-1].cap;
      } else if(ast_id(m->fun->ast) == TK_NEW) {
        param = LLVMGetNextParam(param);
        ++i;
        continue;
      }

      if(type->underlying != TK_ACTOR)
      {
        switch(cap)
        {
          case TK_ISO:
            LLVMAddAttributeAtIndex(fun, i + offset, noalias_attr);
            break;

          case TK_TRN:
          case TK_REF:
            break;

          case TK_VAL:
          case TK_TAG:
          case TK_BOX:
            LLVMAddAttributeAtIndex(fun, i + offset, readonly_attr);
            break;

          default:
            pony_assert(0);
            break;
        }
      }
    }

    genfun_param_deref_attr(c, fun, param, i + offset,
      (compile_type_t*)(i > 0 ? m->params[i - 1].type->c_type : t->c_type));

    param = LLVMGetNextParam(param);
    ++i;
  }
}

void genfun_allocate_compile_methods(compile_t* c, reach_type_t* t)
{
  (void)c;

  size_t i = HASHMAP_BEGIN;
  reach_method_name_t* n;

  while((n = reach_method_names_next(&t->methods, &i)) != NULL)
  {
    size_t j = HASHMAP_BEGIN;
    reach_method_t* m;

    while((m = reach_mangled_next(&n->r_mangled, &j)) != NULL)
    {
      compile_method_t* c_m = POOL_ALLOC(compile_method_t);
      memset(c_m, 0, sizeof(compile_method_t));
      c_m->free_fn = compile_method_free;
      m->c_method = (compile_opaque_t*)c_m;
    }
  }
}

bool genfun_method_sigs(compile_t* c, reach_type_t* t)
{
  size_t i = HASHMAP_BEGIN;
  reach_method_name_t* n;

  while((n = reach_method_names_next(&t->methods, &i)) != NULL)
  {
    size_t j = HASHMAP_BEGIN;
    reach_method_t* m;

    while((m = reach_mangled_next(&n->r_mangled, &j)) != NULL)
    {
      if(m->internal && (n->name == c->str__final))
        genfun_implicit_final_prototype(c, t, m);
      else
        make_prototype(c, t, n, m);

      copy_subordinate(m);
    }
  }

  if(!genfun_allocator(c, t))
    return false;

  return true;
}

bool genfun_method_bodies(compile_t* c, reach_type_t* t)
{
  switch(t->underlying)
  {
    case TK_PRIMITIVE:
    case TK_STRUCT:
    case TK_CLASS:
    case TK_ACTOR:
      break;

    default:
      return true;
  }

  size_t i = HASHMAP_BEGIN;
  reach_method_name_t* n;

  while((n = reach_method_names_next(&t->methods, &i)) != NULL)
  {
    size_t j = HASHMAP_BEGIN;
    reach_method_t* m;

    while((m = reach_mangled_next(&n->r_mangled, &j)) != NULL)
    {
      if(!genfun_method(c, t, n, m))
      {
        if(errors_get_count(c->opt->check.errors) == 0)
        {
          pony_assert(m->fun != NULL);
          ast_error(c->opt->check.errors, m->fun->ast,
            "internal failure: code generation failed for method %s",
            m->full_name);
        }

        return false;
      }
    }
  }

  return true;
}

static bool need_primitive_call(compile_t* c, const char* method)
{
  size_t i = HASHMAP_BEGIN;
  reach_type_t* t;

  while((t = reach_types_next(&c->reach->types, &i)) != NULL)
  {
    if(t->underlying != TK_PRIMITIVE)
      continue;

    reach_method_name_t* n = reach_method_name(t, method);

    if(n == NULL)
      continue;

    return true;
  }

  return false;
}

static void primitive_call(compile_t* c, const char* method)
{
  size_t i = HASHMAP_BEGIN;
  reach_type_t* t;

  while((t = reach_types_next(&c->reach->types, &i)) != NULL)
  {
    if(t->underlying != TK_PRIMITIVE)
      continue;

    reach_method_t* m = reach_method(t, TK_NONE, method, NULL);

    if(m == NULL)
      continue;

    compile_type_t* c_t = (compile_type_t*)t->c_type;
    compile_method_t* c_m = (compile_method_t*)m->c_method;
    LLVMValueRef value = codegen_call(c, LLVMGlobalGetValueType(c_m->func),
      c_m->func, &c_t->instance, 1, true);

    if(c->str__final == method)
      LLVMSetInstructionCallConv(value, LLVMCCallConv);
  }
}

void genfun_primitive_calls(compile_t* c)
{
  LLVMTypeRef fn_type = NULL;

  if(need_primitive_call(c, c->str__init))
  {
    fn_type = LLVMFunctionType(c->void_type, NULL, 0, false);
    const char* fn_name = genname_program_fn(c->filename, "primitives_init");
    c->primitives_init = LLVMAddFunction(c->module, fn_name, fn_type);

    codegen_startfun(c, c->primitives_init, NULL, NULL, NULL, false);
    primitive_call(c, c->str__init);
    genfun_build_ret_void(c);
    codegen_finishfun(c);
  }

  if(need_primitive_call(c, c->str__final))
  {
    if(fn_type == NULL)
      fn_type = LLVMFunctionType(c->void_type, NULL, 0, false);
    const char* fn_name = genname_program_fn(c->filename, "primitives_final");
    c->primitives_final = LLVMAddFunction(c->module, fn_name, fn_type);

    codegen_startfun(c, c->primitives_final, NULL, NULL, NULL, false);
    primitive_call(c, c->str__final);
    genfun_build_ret_void(c);
    codegen_finishfun(c);
  }
}
