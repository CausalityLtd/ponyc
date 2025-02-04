#include "genoperator.h"
#include "gencall.h"
#include "genexpr.h"
#include "genreference.h"
#include "genname.h"
#include "gentype.h"
#include "../pkg/platformfuns.h"
#include "../type/subtype.h"
#include "ponyassert.h"

typedef LLVMValueRef (*const_binop)(LLVMValueRef left, LLVMValueRef right);

typedef LLVMValueRef (*build_binop)(LLVMBuilderRef builder, LLVMValueRef left,
  LLVMValueRef right, const char *name);

static LLVMValueRef assign_rvalue(compile_t* c, ast_t* left, ast_t* r_type,
  LLVMValueRef r_value);

static bool is_always_true(LLVMValueRef val)
{
  if(!LLVMIsAConstantInt(val))
    return false;

  return LLVMConstIntGetZExtValue(val) == 1;
}

static bool is_always_false(LLVMValueRef val)
{
  if(!LLVMIsAConstantInt(val))
    return false;

  return LLVMConstIntGetZExtValue(val) == 0;
}

static bool is_fp(LLVMValueRef val)
{
  LLVMTypeRef type = LLVMTypeOf(val);

  switch(LLVMGetTypeKind(type))
  {
    case LLVMHalfTypeKind:
    case LLVMFloatTypeKind:
    case LLVMDoubleTypeKind:
      return true;

    default: {}
  }

  return false;
}

static LLVMValueRef make_binop(compile_t* c, ast_t* left, ast_t* right,
  const_binop const_f, const_binop const_i,
  build_binop build_f, build_binop build_i)
{
  LLVMValueRef l_value = gen_expr(c, left);
  LLVMValueRef r_value = gen_expr(c, right);

  if((l_value == NULL) || (r_value == NULL))
    return NULL;

  if(LLVMIsConstant(l_value) && LLVMIsConstant(r_value))
  {
    if(is_fp(l_value)) {
      if (const_f)
        return const_f(l_value, r_value);
    } else {
      if (const_i)
        return const_i(l_value, r_value);
    }
  }

  if(is_fp(l_value))
    return build_f(c->builder, l_value, r_value, "");

  return build_i(c->builder, l_value, r_value, "");
}

static LLVMValueRef make_divrem(compile_t* c, ast_t* left, ast_t* right,
  const_binop const_f, const_binop const_ui, const_binop const_si,
  build_binop build_f, build_binop build_ui, build_binop build_si,
  bool safe)
{
  ast_t* type = deferred_reify(c->frame->reify, ast_type(left), c->opt);
  bool sign = is_signed(type);
  ast_free_unattached(type);

  LLVMValueRef l_value = gen_expr(c, left);
  LLVMValueRef r_value = gen_expr(c, right);

  if((l_value == NULL) || (r_value == NULL))
    return NULL;

  LLVMTypeRef r_type = LLVMTypeOf(r_value);

  // This doesn't pick up `x / 0` for 128 bit numbers on platforms without
  // native 128 bit support.
  if(!is_fp(r_value) && LLVMIsConstant(r_value))
  {
    long long r_const = LLVMConstIntGetSExtValue(r_value);
    if (r_const == 0)
    {
      ast_error(c->opt->check.errors, right, "constant divide or rem by zero");
      return NULL;
    }

    if((r_const == -1) && LLVMIsConstant(l_value) && sign)
    {
      uint64_t width = LLVMGetIntTypeWidth(r_type);
      LLVMValueRef v_min = LLVMConstInt(r_type, 0, false);
      v_min = LLVMConstNot(v_min);
      v_min = LLVMConstShl(v_min, LLVMConstInt(r_type, width - 1, false));
      long long min = LLVMConstIntGetSExtValue(v_min);
      if(LLVMConstIntGetSExtValue(l_value) == min)
      {
        ast_error(c->opt->check.errors, ast_parent(left),
          "constant divide or rem overflow");
        return NULL;
      }
    }
  }

  if(LLVMIsConstant(l_value) && LLVMIsConstant(r_value))
  {
    if(is_fp(l_value)) {
      if (const_f)
        return const_f(l_value, r_value);
    } else if(sign) {
      if (const_si)
        return const_si(l_value, r_value);
    } else {
      if (const_ui)
        return const_ui(l_value, r_value);
    }
  }

  if(is_fp(l_value))
    return build_f(c->builder, l_value, r_value, "");

  LLVMBasicBlockRef insert;
  LLVMBasicBlockRef nonzero_block;
  LLVMBasicBlockRef no_overflow_block;
  LLVMBasicBlockRef post_block = NULL;
  LLVMValueRef zero;

  if(safe)
  {
    // Setup additional blocks.
    insert = LLVMGetInsertBlock(c->builder);
    nonzero_block = codegen_block(c, "div_nonzero");
    if(sign)
      no_overflow_block = codegen_block(c, "div_no_overflow");
    post_block = codegen_block(c, "div_post");

    // Check for div by zero.
    zero = LLVMConstInt(r_type, 0, false);
    LLVMValueRef cmp = LLVMBuildICmp(c->builder, LLVMIntNE, r_value, zero, "");
    LLVMBuildCondBr(c->builder, cmp, nonzero_block, post_block);

    // Divisor is not zero.
    LLVMPositionBuilderAtEnd(c->builder, nonzero_block);

    if(sign)
    {
      // Check for overflow, e.g. I8(-128) / -1
      uint64_t width = LLVMGetIntTypeWidth(r_type);
      LLVMValueRef v_min = LLVMConstInt(r_type, 0, false);
      v_min = LLVMConstNot(v_min);
      LLVMValueRef denom_good = LLVMBuildICmp(c->builder, LLVMIntNE, r_value,
        v_min, "");
      v_min = LLVMConstShl(v_min, LLVMConstInt(r_type, width - 1, false));
      LLVMValueRef numer_good = LLVMBuildICmp(c->builder, LLVMIntNE, l_value,
        v_min, "");
      LLVMValueRef no_overflow = LLVMBuildOr(c->builder, numer_good, denom_good,
        "");
      LLVMBuildCondBr(c->builder, no_overflow, no_overflow_block, post_block);

      // No overflow.
      LLVMPositionBuilderAtEnd(c->builder, no_overflow_block);
    }
  }

  LLVMValueRef result;

  if(sign)
    result = build_si(c->builder, l_value, r_value, "");
  else
    result = build_ui(c->builder, l_value, r_value, "");

  if(safe)
  {
    LLVMBuildBr(c->builder, post_block);

    // Phi node.
    LLVMPositionBuilderAtEnd(c->builder, post_block);
    LLVMValueRef phi = LLVMBuildPhi(c->builder, r_type, "");
    LLVMAddIncoming(phi, &zero, &insert, 1);
    if(sign)
    {
      LLVMAddIncoming(phi, &zero, &nonzero_block, 1);
      LLVMAddIncoming(phi, &result, &no_overflow_block, 1);
    } else {
      LLVMAddIncoming(phi, &result, &nonzero_block, 1);
    }

    return phi;
  }

  return result;
}

static LLVMValueRef make_cmp_value(compile_t* c, bool sign,
  LLVMValueRef l_value, LLVMValueRef r_value, LLVMRealPredicate cmp_f,
  LLVMIntPredicate cmp_si, LLVMIntPredicate cmp_ui, bool safe)
{
  if((l_value == NULL) || (r_value == NULL))
    return NULL;

  if(LLVMIsConstant(l_value) && LLVMIsConstant(r_value))
  {
    if(is_fp(l_value))
      return LLVMConstFCmp(cmp_f, l_value, r_value);

    if(sign)
      return LLVMConstICmp(cmp_si, l_value, r_value);

    return LLVMConstICmp(cmp_ui, l_value, r_value);
  }

  if(is_fp(l_value))
  {
    LLVMValueRef result = LLVMBuildFCmp(c->builder, cmp_f, l_value, r_value,
      "");
    if(!safe)
      LLVMSetUnsafeAlgebra(result);
    return result;
  }

  if(sign)
    return LLVMBuildICmp(c->builder, cmp_si, l_value, r_value, "");

  return LLVMBuildICmp(c->builder, cmp_ui, l_value, r_value, "");
}

static LLVMValueRef make_cmp(compile_t* c, ast_t* left, ast_t* right,
  LLVMRealPredicate cmp_f, LLVMIntPredicate cmp_si, LLVMIntPredicate cmp_ui,
  bool safe)
{
  ast_t* type = deferred_reify(c->frame->reify, ast_type(left), c->opt);
  bool sign = is_signed(type);
  ast_free_unattached(type);

  LLVMValueRef l_value = gen_expr(c, left);
  LLVMValueRef r_value = gen_expr(c, right);

  return make_cmp_value(c, sign, l_value, r_value, cmp_f, cmp_si, cmp_ui, safe);
}

static LLVMValueRef make_short_circuit(compile_t* c, ast_t* left, ast_t* right,
  bool is_and)
{
  LLVMBasicBlockRef entry_block = LLVMGetInsertBlock(c->builder);
  LLVMBasicBlockRef left_block = codegen_block(c, "sc_left");
  LLVMValueRef branch = LLVMBuildBr(c->builder, left_block);

  LLVMPositionBuilderAtEnd(c->builder, left_block);
  LLVMValueRef l_value = gen_expr(c, left);

  if(l_value == NULL)
    return NULL;

  if(LLVMIsAConstantInt(l_value))
  {
    LLVMInstructionEraseFromParent(branch);
    LLVMDeleteBasicBlock(left_block);
    LLVMPositionBuilderAtEnd(c->builder, entry_block);

    if(is_and)
    {
      if(is_always_false(l_value))
        return gen_expr(c, left);
    } else {
      if(is_always_true(l_value))
        return gen_expr(c, left);
    }

    return gen_expr(c, right);
  }

  LLVMBasicBlockRef left_exit_block = LLVMGetInsertBlock(c->builder);

  LLVMBasicBlockRef right_block = codegen_block(c, "sc_right");
  LLVMBasicBlockRef post_block = codegen_block(c, "sc_post");

  if(is_and)
    LLVMBuildCondBr(c->builder, l_value, right_block, post_block);
  else
    LLVMBuildCondBr(c->builder, l_value, post_block, right_block);

  LLVMPositionBuilderAtEnd(c->builder, right_block);
  LLVMValueRef r_value = gen_expr(c, right);

  if(r_value == NULL)
    return NULL;

  LLVMBasicBlockRef right_exit_block = LLVMGetInsertBlock(c->builder);
  LLVMBuildBr(c->builder, post_block);

  LLVMPositionBuilderAtEnd(c->builder, post_block);
  LLVMValueRef phi = LLVMBuildPhi(c->builder, c->i1, "");

  LLVMAddIncoming(phi, &l_value, &left_exit_block, 1);
  LLVMAddIncoming(phi, &r_value, &right_exit_block, 1);

  if(LLVMIsAConstantInt(r_value))
  {
    if(is_and)
    {
      if(is_always_false(r_value))
        return r_value;
    } else {
      if(is_always_true(r_value))
        return r_value;
    }

    return l_value;
  }

  return phi;
}

static LLVMValueRef assign_local(compile_t* c, LLVMValueRef l_value,
  LLVMValueRef r_value, ast_t* l_type, ast_t* r_type)
{
  reach_type_t* t = reach_type(c->reach, l_type);
  compile_type_t* c_t = (compile_type_t*)t->c_type;

  LLVMValueRef result = LLVMBuildLoad2(c->builder,
    LLVMGetAllocatedType(l_value), l_value, "");
  result = gen_assign_cast(c, c_t->use_type, result, l_type);

  // Cast the rvalue appropriately.
  r_value = gen_assign_cast(c, c_t->mem_type, r_value, r_type);

  if(r_value == NULL)
    return NULL;

  // Store to the local.
  LLVMBuildStore(c->builder, r_value, l_value);

  return result;
}

static LLVMValueRef assign_field(compile_t* c, LLVMValueRef l_value,
  LLVMValueRef r_value, ast_t* l_type, ast_t* r_type)
{
  reach_type_t* t = reach_type(c->reach, l_type);
  pony_assert(t != NULL);
  compile_type_t* c_t = (compile_type_t*)t->c_type;

  LLVMValueRef result = LLVMBuildLoad2(c->builder, c_t->mem_type, l_value, "");

  // Cast the rvalue appropriately.
  r_value = gen_assign_cast(c, c_t->mem_type, r_value, r_type);

  if(r_value == NULL)
    return NULL;

  // Store to the field.
  LLVMBuildStore(c->builder, r_value, l_value);

  return gen_assign_cast(c, c_t->use_type, result, l_type);
}

static bool assign_tuple(compile_t* c, ast_t* left, ast_t* r_type,
  LLVMValueRef r_value)
{
  // Handle assignment for each component.
  ast_t* child = ast_child(left);
  ast_t* rtype_child = ast_child(r_type);
  int i = 0;

  while(child != NULL)
  {
    ast_t* expr = NULL;

    switch(ast_id(child))
    {
      case TK_SEQ:
        // The actual tuple expression is inside a sequence node.
        expr = ast_child(child);
        break;

      case TK_LET:
      case TK_VAR:
      case TK_TUPLE:
        expr = child;
        break;

      case TK_DONTCAREREF:
        // Ignore this element.
        break;

      default:
        pony_assert(0);
    }

    if(expr != NULL)
    {
      // Extract the tuple value.
      LLVMValueRef rvalue_child = LLVMBuildExtractValue(c->builder, r_value, i,
        "");

      // Recurse, assigning pairwise.
      if(assign_rvalue(c, expr, rtype_child, rvalue_child) == NULL)
        return false;
    }

    i++;
    child = ast_sibling(child);
    rtype_child = ast_sibling(rtype_child);
  }

  pony_assert(rtype_child == NULL);
  return true;
}

static LLVMValueRef assign_rvalue(compile_t* c, ast_t* left, ast_t* r_type,
  LLVMValueRef r_value)
{
  deferred_reification_t* reify = c->frame->reify;

  switch(ast_id(left))
  {
    case TK_SEQ:
      // The actual expression is inside a sequence node.
      while(ast_id(left) == TK_SEQ)
      {
        pony_assert(ast_childcount(left) == 1);
        left = ast_child(left);
      }
      return assign_rvalue(c, left, r_type, r_value);

    case TK_VAR:
    case TK_LET:
    {
      // Generate the locals.
      if(gen_localdecl(c, left) == NULL)
        return NULL;

      return assign_rvalue(c, ast_child(left), r_type, r_value);
    }

    case TK_MATCH_CAPTURE:
      // The alloca has already been generated.
      return assign_rvalue(c, ast_child(left), r_type, r_value);

    case TK_FVARREF:
    case TK_FLETREF:
    {
      // The result is the previous value of the field.
      LLVMValueRef l_value = gen_fieldptr(c, left);
      ast_t* p_type = deferred_reify(reify, ast_type(ast_child(left)), c->opt);
      ast_t* l_type = deferred_reify(reify, ast_type(left), c->opt);

      LLVMValueRef ret = assign_field(c, l_value, r_value, l_type, r_type);

      ast_free_unattached(p_type);
      ast_free_unattached(l_type);
      return ret;
    }

    case TK_TUPLEELEMREF:
    {
      // The result is the previous value of the tuple element.
      LLVMValueRef l_value = gen_tupleelemptr(c, left);
      ast_t* p_type = deferred_reify(reify, ast_type(ast_child(left)), c->opt);
      ast_t* l_type = deferred_reify(reify, ast_type(left), c->opt);

      LLVMValueRef r = assign_field(c, l_value, r_value, l_type, r_type);

      ast_free_unattached(p_type);
      ast_free_unattached(l_type);
      return r;
    }

    case TK_EMBEDREF:
    {
      // Do nothing. The embed field was already passed as the receiver.
      // But we can't return GEN_NOVALUE - that's reserved only for branches
      // that "jump away". So we just return the r_value we were given.
      // The type system ensures that nobody will be able to use this value.
      return r_value;
    }

    case TK_VARREF:
    {
      // The result is the previous value of the local.
      const char* name = ast_name(ast_child(left));
      codegen_local_lifetime_start(c, name);
      LLVMValueRef l_value = codegen_getlocal(c, name);
      ast_t* l_type = deferred_reify(reify, ast_type(left), c->opt);

      LLVMValueRef ret = assign_local(c, l_value, r_value, l_type, r_type);

      ast_free_unattached(l_type);
      return ret;
    }

    case TK_DONTCARE:
    case TK_DONTCAREREF:
    case TK_MATCH_DONTCARE:
    {
      // Do nothing.
      // But we can't return GEN_NOVALUE - that's reserved only for branches
      // that "jump away". So we just return the r_value we were given.
      // The type system ensures that nobody will be able to use this value.
      return r_value;
    }

    case TK_TUPLE:
    {
      // If the l_value is a tuple, assemble it as the result.
      LLVMValueRef result = gen_expr(c, left);

      if(result == NULL)
        return NULL;

      if(!assign_tuple(c, left, r_type, r_value))
        return NULL;

      // Return the original tuple.
      return result;
    }

    case TK_ID:
    {
      // We have recursed here from a VAR, LET or MATCH_CAPTURE.
      const char* name = ast_name(left);
      codegen_local_lifetime_start(c, name);
      LLVMValueRef l_value = codegen_getlocal(c, name);
      ast_t* l_type = deferred_reify(reify, ast_type(ast_parent(left)), c->opt);

      LLVMValueRef ret = assign_local(c, l_value, r_value, l_type, r_type);

      ast_free_unattached(l_type);
      return ret;
    }

    default: {}
  }

  pony_assert(0);
  return NULL;
}

static LLVMValueRef make_unsafe_fadd(LLVMBuilderRef builder, LLVMValueRef left,
  LLVMValueRef right, const char *name)
{
  LLVMValueRef result = LLVMBuildFAdd(builder, left, right, name);
  LLVMSetUnsafeAlgebra(result);
  return result;
}

static LLVMValueRef make_unsafe_fsub(LLVMBuilderRef builder, LLVMValueRef left,
  LLVMValueRef right, const char *name)
{
  LLVMValueRef result = LLVMBuildFSub(builder, left, right, name);
  LLVMSetUnsafeAlgebra(result);
  return result;
}

static LLVMValueRef make_unsafe_fmul(LLVMBuilderRef builder, LLVMValueRef left,
  LLVMValueRef right, const char *name)
{
  LLVMValueRef result = LLVMBuildFMul(builder, left, right, name);
  LLVMSetUnsafeAlgebra(result);
  return result;
}

static LLVMValueRef make_unsafe_fdiv(LLVMBuilderRef builder, LLVMValueRef left,
  LLVMValueRef right, const char *name)
{
  LLVMValueRef result = LLVMBuildFDiv(builder, left, right, name);
  LLVMSetUnsafeAlgebra(result);
  return result;
}

static LLVMValueRef make_unsafe_frem(LLVMBuilderRef builder, LLVMValueRef left,
  LLVMValueRef right, const char *name)
{
  LLVMValueRef result = LLVMBuildFRem(builder, left, right, name);
  LLVMSetUnsafeAlgebra(result);
  return result;
}

static LLVMValueRef make_unsafe_fneg(LLVMBuilderRef builder, LLVMValueRef op,
  const char *name)
{
  LLVMValueRef result = LLVMBuildFNeg(builder, op, name);
  LLVMSetUnsafeAlgebra(result);
  return result;
}

LLVMValueRef gen_add(compile_t* c, ast_t* left, ast_t* right, bool safe)
{
  if(safe)
    return make_binop(c, left, right, NULL, LLVMConstAdd, LLVMBuildFAdd,
      LLVMBuildAdd);

  ast_t* type = deferred_reify(c->frame->reify, ast_type(left), c->opt);
  bool sign = is_signed(type);
  ast_free_unattached(type);

  return make_binop(c, left, right, NULL, LLVMConstAdd, make_unsafe_fadd,
    sign ? LLVMBuildNSWAdd : LLVMBuildNUWAdd);
}

LLVMValueRef gen_sub(compile_t* c, ast_t* left, ast_t* right, bool safe)
{
  if(safe)
    return make_binop(c, left, right, NULL, LLVMConstSub, LLVMBuildFSub,
      LLVMBuildSub);

  ast_t* type = deferred_reify(c->frame->reify, ast_type(left), c->opt);
  bool sign = is_signed(type);
  ast_free_unattached(type);

  return make_binop(c, left, right, NULL, LLVMConstSub, make_unsafe_fsub,
    sign ? LLVMBuildNSWSub : LLVMBuildNUWSub);
}

LLVMValueRef gen_mul(compile_t* c, ast_t* left, ast_t* right, bool safe)
{
  if(safe)
    return make_binop(c, left, right, NULL, LLVMConstMul, LLVMBuildFMul,
      LLVMBuildMul);

  ast_t* type = deferred_reify(c->frame->reify, ast_type(left), c->opt);
  bool sign = is_signed(type);
  ast_free_unattached(type);

  return make_binop(c, left, right, NULL, LLVMConstMul, make_unsafe_fmul,
    sign ? LLVMBuildNSWMul : LLVMBuildNUWMul);
}

LLVMValueRef gen_div(compile_t* c, ast_t* left, ast_t* right, bool safe)
{
  return make_divrem(c, left, right, NULL, NULL, NULL,
    safe ? LLVMBuildFDiv : make_unsafe_fdiv, LLVMBuildUDiv, LLVMBuildSDiv,
    safe);
}

LLVMValueRef gen_rem(compile_t* c, ast_t* left, ast_t* right, bool safe)
{
  return make_divrem(c, left, right, NULL, NULL, NULL,
    safe ? LLVMBuildFRem : make_unsafe_frem, LLVMBuildURem, LLVMBuildSRem,
    safe);
}

LLVMValueRef gen_neg(compile_t* c, ast_t* ast, bool safe)
{
  LLVMValueRef value = gen_expr(c, ast);

  if(value == NULL)
    return NULL;

  if(LLVMIsAConstantFP(value))
    return LLVMBuildFNeg(c->builder, value, "");

  if(LLVMIsAConstantInt(value))
    return LLVMConstNeg(value);

  if(is_fp(value))
    return (safe ? LLVMBuildFNeg : make_unsafe_fneg)(c->builder, value, "");

  if(safe)
    return LLVMBuildNeg(c->builder, value, "");

  ast_t* type = deferred_reify(c->frame->reify, ast_type(ast), c->opt);
  bool sign = is_signed(type);
  ast_free_unattached(type);

  if(sign)
    return LLVMBuildNSWNeg(c->builder, value, "");
  return LLVMBuildNUWNeg(c->builder, value, "");
}

LLVMValueRef gen_shl(compile_t* c, ast_t* left, ast_t* right, bool safe)
{
  LLVMValueRef l_value = gen_expr(c, left);
  LLVMValueRef r_value = gen_expr(c, right);

  if((l_value == NULL) || (r_value == NULL))
    return NULL;

  LLVMTypeRef l_type = LLVMTypeOf(l_value);
  pony_assert(LLVMGetTypeKind(l_type) == LLVMIntegerTypeKind);
  unsigned width = LLVMGetIntTypeWidth(l_type);

  if(LLVMIsConstant(r_value))
  {
    if(LLVMConstIntGetZExtValue(r_value) >= width)
    {
      ast_error(c->opt->check.errors, right,
        "shift amount greater than type width");
      return NULL;
    }

    if(LLVMIsConstant(l_value))
      return LLVMConstShl(l_value, r_value);
  }

  if(safe)
  {
    LLVMTypeRef r_type = LLVMTypeOf(r_value);
    LLVMValueRef clamp = LLVMConstInt(r_type, width - 1, false);
    LLVMValueRef cmp = LLVMBuildICmp(c->builder, LLVMIntULE, r_value, clamp,
      "");
    LLVMValueRef select = LLVMBuildSelect(c->builder, cmp, r_value, clamp, "");

    return LLVMBuildShl(c->builder, l_value, select, "");
  }

  ast_t* type = deferred_reify(c->frame->reify, ast_type(left), c->opt);
  bool sign = is_signed(type);
  ast_free_unattached(type);

  LLVMValueRef result = LLVMBuildShl(c->builder, l_value, r_value, "");

  if(sign)
    LLVMSetNoSignedWrap(result);
  else
    LLVMSetNoUnsignedWrap(result);

  return result;
}

LLVMValueRef gen_shr(compile_t* c, ast_t* left, ast_t* right, bool safe)
{
  (void)safe;
  ast_t* type = deferred_reify(c->frame->reify, ast_type(left), c->opt);
  bool sign = is_signed(type);
  ast_free_unattached(type);

  LLVMValueRef l_value = gen_expr(c, left);
  LLVMValueRef r_value = gen_expr(c, right);

  if((l_value == NULL) || (r_value == NULL))
    return NULL;

  LLVMTypeRef l_type = LLVMTypeOf(l_value);
  pony_assert(LLVMGetTypeKind(l_type) == LLVMIntegerTypeKind);
  unsigned width = LLVMGetIntTypeWidth(l_type);

  if(LLVMIsConstant(r_value))
  {
    if(LLVMConstIntGetZExtValue(r_value) >= width)
    {
      ast_error(c->opt->check.errors, right,
        "shift amount greater than type width");
      return NULL;
    }

    if(LLVMIsConstant(l_value))
    {
      if(sign)
        return LLVMConstAShr(l_value, r_value);

      return LLVMConstLShr(l_value, r_value);
    }
  }

  if(safe)
  {
    LLVMTypeRef r_type = LLVMTypeOf(r_value);
    LLVMValueRef clamp = LLVMConstInt(r_type, width - 1, false);
    LLVMValueRef cmp = LLVMBuildICmp(c->builder, LLVMIntULE, r_value, clamp,
      "");
    LLVMValueRef select = LLVMBuildSelect(c->builder, cmp, r_value, clamp, "");

    if(sign)
      return LLVMBuildAShr(c->builder, l_value, select, "");

    return LLVMBuildLShr(c->builder, l_value, select, "");
  }

  if(sign)
  {
    LLVMValueRef result = LLVMBuildAShr(c->builder, l_value, r_value, "");
    LLVMSetIsExact(result);
    return result;
  }

  LLVMValueRef result = LLVMBuildLShr(c->builder, l_value, r_value, "");
  LLVMSetIsExact(result);
  return result;
}

LLVMValueRef gen_and_sc(compile_t* c, ast_t* left, ast_t* right)
{
  return make_short_circuit(c, left, right, true);
}

LLVMValueRef gen_or_sc(compile_t* c, ast_t* left, ast_t* right)
{
  return make_short_circuit(c, left, right, false);
}

LLVMValueRef gen_and(compile_t* c, ast_t* left, ast_t* right)
{
  LLVMValueRef l_value = gen_expr(c, left);
  LLVMValueRef r_value = gen_expr(c, right);

  if((l_value == NULL) || (r_value == NULL))
    return NULL;

  return LLVMBuildAnd(c->builder, l_value, r_value, "");
}

LLVMValueRef gen_or(compile_t* c, ast_t* left, ast_t* right)
{
  LLVMValueRef l_value = gen_expr(c, left);
  LLVMValueRef r_value = gen_expr(c, right);

  if((l_value == NULL) || (r_value == NULL))
    return NULL;

  if(LLVMIsConstant(l_value) && LLVMIsConstant(r_value))
    return LLVMConstOr(l_value, r_value);

  return LLVMBuildOr(c->builder, l_value, r_value, "");
}

LLVMValueRef gen_xor(compile_t* c, ast_t* left, ast_t* right)
{
  LLVMValueRef l_value = gen_expr(c, left);
  LLVMValueRef r_value = gen_expr(c, right);

  if((l_value == NULL) || (r_value == NULL))
    return NULL;

  if(LLVMIsConstant(l_value) && LLVMIsConstant(r_value))
    return LLVMConstXor(l_value, r_value);

  return LLVMBuildXor(c->builder, l_value, r_value, "");
}

LLVMValueRef gen_not(compile_t* c, ast_t* ast)
{
  LLVMValueRef value = gen_expr(c, ast);

  if(value == NULL)
    return NULL;

  ast_t* type = deferred_reify(c->frame->reify, ast_type(ast), c->opt);
  bool bool_type = is_bool(type);
  ast_free_unattached(type);

  if(bool_type)
  {
    if(LLVMIsAConstantInt(value))
    {
      if(is_always_true(value))
        return LLVMConstInt(c->i1, 0, false);

      return LLVMConstInt(c->i1, 1, false);
    }

    return LLVMBuildICmp(c->builder, LLVMIntEQ, value,
      LLVMConstInt(c->i1, 0, false), "");
  }

  if(LLVMIsAConstantInt(value))
    return LLVMConstNot(value);

  return LLVMBuildNot(c->builder, value, "");
}

LLVMValueRef gen_eq(compile_t* c, ast_t* left, ast_t* right, bool safe)
{
  return make_cmp(c, left, right, LLVMRealOEQ, LLVMIntEQ, LLVMIntEQ, safe);
}

LLVMValueRef gen_eq_rvalue(compile_t* c, ast_t* left, LLVMValueRef r_value,
  bool safe)
{
  ast_t* type = deferred_reify(c->frame->reify, ast_type(left), c->opt);
  bool sign = is_signed(type);
  ast_free_unattached(type);
  LLVMValueRef l_value = gen_expr(c, left);

  return make_cmp_value(c, sign, l_value, r_value, LLVMRealOEQ, LLVMIntEQ,
    LLVMIntEQ, safe);
}

LLVMValueRef gen_ne(compile_t* c, ast_t* left, ast_t* right, bool safe)
{
  return make_cmp(c, left, right, LLVMRealONE, LLVMIntNE, LLVMIntNE, safe);
}

LLVMValueRef gen_lt(compile_t* c, ast_t* left, ast_t* right, bool safe)
{
  return make_cmp(c, left, right, LLVMRealOLT, LLVMIntSLT, LLVMIntULT, safe);
}

LLVMValueRef gen_le(compile_t* c, ast_t* left, ast_t* right, bool safe)
{
  return make_cmp(c, left, right, LLVMRealOLE, LLVMIntSLE, LLVMIntULE, safe);
}

LLVMValueRef gen_ge(compile_t* c, ast_t* left, ast_t* right, bool safe)
{
  return make_cmp(c, left, right, LLVMRealOGE, LLVMIntSGE, LLVMIntUGE, safe);
}

LLVMValueRef gen_gt(compile_t* c, ast_t* left, ast_t* right, bool safe)
{
  return make_cmp(c, left, right, LLVMRealOGT, LLVMIntSGT, LLVMIntUGT, safe);
}

LLVMValueRef gen_assign(compile_t* c, ast_t* ast)
{
  // Must generate the right hand side before the left hand side.
  AST_GET_CHILDREN(ast, left, right);
  LLVMValueRef r_value = gen_expr(c, right);

  if(r_value == NULL)
    return NULL;

  codegen_debugloc(c, ast);
  ast_t* type = deferred_reify(c->frame->reify, ast_type(right), c->opt);
  LLVMValueRef result = assign_rvalue(c, left, type, r_value);
  ast_free_unattached(type);
  codegen_debugloc(c, NULL);
  return result;
}

LLVMValueRef gen_assign_value(compile_t* c, ast_t* left, LLVMValueRef right,
  ast_t* right_type)
{
  // This is from pattern matching.
  // Must generate the right hand side before the left hand side.
  if(right == NULL)
    return NULL;

  return assign_rvalue(c, left, right_type, right);
}
