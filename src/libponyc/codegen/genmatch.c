#include "genmatch.h"
#include "gencall.h"
#include "gencontrol.h"
#include "gendesc.h"
#include "genfun.h"
#include "genexpr.h"
#include "genoperator.h"
#include "genreference.h"
#include "../pass/expr.h"
#include "../type/subtype.h"
#include "../type/matchtype.h"
#include "../type/alias.h"
#include "../type/viewpoint.h"
#include "../type/lookup.h"
#include "ponyassert.h"

typedef enum
{
  WEIGHT_NONE,
  WEIGHT_LIKELY,
  WEIGHT_UNLIKELY
} match_weight_t;

static bool check_type(compile_t* c, LLVMValueRef ptr, LLVMValueRef desc,
  ast_t* pattern_type, LLVMBasicBlockRef next_block, match_weight_t weight);

static bool dynamic_match_ptr(compile_t* c, LLVMValueRef ptr,
  LLVMValueRef desc, ast_t* pattern, LLVMBasicBlockRef next_block);

static bool dynamic_match_object(compile_t* c, LLVMValueRef object,
  LLVMValueRef desc, ast_t* pattern, LLVMBasicBlockRef next_block);

static bool static_match(compile_t* c, LLVMValueRef value, ast_t* type,
  ast_t* pattern, LLVMBasicBlockRef next_block);

static void make_branchweights(compile_t* c, LLVMValueRef br,
  match_weight_t weight)
{
  switch(weight)
  {
    case WEIGHT_LIKELY:
    {
      unsigned int weights[] =
        {PONY_BRANCHWEIGHT_LIKELY, PONY_BRANCHWEIGHT_UNLIKELY};
      attach_branchweights_metadata(c->context, br, weights, 2);
      break;
    }

    case WEIGHT_UNLIKELY:
    {
      unsigned int weights[] =
        {PONY_BRANCHWEIGHT_UNLIKELY, PONY_BRANCHWEIGHT_LIKELY};
      attach_branchweights_metadata(c->context, br, weights, 2);
      break;
    }

    default: {}
  }
}

static ast_t* eq_param_type(compile_t* c, ast_t* pattern)
{
  ast_t* pattern_type = deferred_reify(c->frame->reify, ast_type(pattern),
    c->opt);
  deferred_reification_t* fun = lookup(NULL, pattern, pattern_type, c->str_eq);

  AST_GET_CHILDREN(fun->ast, cap, id, typeparams, params, result, partial);
  ast_t* param = ast_child(params);
  ast_t* type = ast_childidx(param, 1);

  ast_t* r_type = deferred_reify(fun, type, c->opt);

  ast_free_unattached(pattern_type);
  deferred_reify_free(fun);
  return r_type;
}

static bool check_nominal(compile_t* c, LLVMValueRef desc, ast_t* pattern_type,
  LLVMBasicBlockRef next_block, match_weight_t weight)
{
  LLVMValueRef test = gendesc_isnominal(c, desc, pattern_type);

  if(test == GEN_NOVALUE)
    return false;

  LLVMBasicBlockRef continue_block = codegen_block(c, "pattern_continue");
  if(next_block == NULL)
    next_block = continue_block;

  LLVMValueRef br = LLVMBuildCondBr(c->builder, test, continue_block,
    next_block);

  make_branchweights(c, br, weight);

  LLVMPositionBuilderAtEnd(c->builder, continue_block);
  return true;
}

static void check_cardinality(compile_t* c, LLVMValueRef desc, size_t size,
  LLVMBasicBlockRef next_block)
{
  LLVMValueRef field_count = gendesc_fieldcount(c, desc);
  LLVMValueRef count = LLVMConstInt(c->i32, size, false);
  LLVMValueRef test = LLVMBuildICmp(c->builder, LLVMIntEQ, count, field_count,
    "");

  LLVMBasicBlockRef continue_block = codegen_block(c, "pattern_continue");
  if(next_block == NULL)
    next_block = continue_block;

  LLVMBuildCondBr(c->builder, test, continue_block, next_block);
  LLVMPositionBuilderAtEnd(c->builder, continue_block);
}

static bool check_tuple(compile_t* c, LLVMValueRef ptr, LLVMValueRef desc,
  ast_t* pattern_type, LLVMBasicBlockRef next_block, match_weight_t weight)
{
  // First check cardinality.
  size_t size = ast_childcount(pattern_type);
  check_cardinality(c, desc, size, next_block);

  // If we get here, the match expression has the right cardinality.
  ast_t* pattern_child = ast_child(pattern_type);

  for(int i = 0; pattern_child != NULL; i++)
  {
    // Get the field offset and field descriptor from the tuple descriptor.
    LLVMValueRef field_info = gendesc_fieldinfo(c, desc, i);
    LLVMValueRef field_ptr = gendesc_fieldptr(c, ptr, field_info);
    LLVMValueRef field_desc = gendesc_fielddesc(c, field_info);

    // If we have a null descriptor, load the object.
    LLVMBasicBlockRef null_block = codegen_block(c, "null_desc");
    LLVMBasicBlockRef nonnull_block = codegen_block(c, "nonnull_desc");
    LLVMBasicBlockRef continue_block = codegen_block(c, "merge_desc");
    LLVMValueRef test = LLVMBuildIsNull(c->builder, field_desc, "");
    LLVMBuildCondBr(c->builder, test, null_block, nonnull_block);

    // Load the object, load its descriptor, and continue from there.
    LLVMPositionBuilderAtEnd(c->builder, null_block);
    LLVMValueRef object_ptr = field_ptr;
    LLVMValueRef object = LLVMBuildLoad2(c->builder, c->ptr, object_ptr,
      "");
    LLVMValueRef object_desc = gendesc_fetch(c, object);
    object_ptr = gendesc_ptr_to_fields(c, object, object_desc);

    if(!check_type(c, object_ptr, object_desc, pattern_child, next_block,
      weight))
      return false;

    LLVMBuildBr(c->builder, continue_block);

    // Continue with the pointer and descriptor.
    LLVMMoveBasicBlockAfter(nonnull_block, LLVMGetInsertBlock(c->builder));
    LLVMPositionBuilderAtEnd(c->builder, nonnull_block);

    if(!check_type(c, field_ptr, field_desc, pattern_child, next_block,
      weight))
      return false;

    LLVMBuildBr(c->builder, continue_block);

    // Merge the two branches.
    LLVMMoveBasicBlockAfter(continue_block, LLVMGetInsertBlock(c->builder));
    LLVMPositionBuilderAtEnd(c->builder, continue_block);
    pattern_child = ast_sibling(pattern_child);
  }

  return true;
}

static bool check_union_or_isect(compile_t* c, LLVMValueRef desc,
  ast_t* pattern_type, LLVMBasicBlockRef next_block, match_weight_t weight)
{
  LLVMValueRef test = gendesc_istrait(c, desc, pattern_type);

  if(test == GEN_NOVALUE)
    return false;

  LLVMBasicBlockRef continue_block = codegen_block(c, "pattern_continue");
  if(next_block == NULL)
    next_block = continue_block;

  LLVMValueRef br = LLVMBuildCondBr(c->builder, test, continue_block,
    next_block);

  make_branchweights(c, br, weight);

  LLVMPositionBuilderAtEnd(c->builder, continue_block);
  return true;
}

static bool check_type(compile_t* c, LLVMValueRef ptr, LLVMValueRef desc,
  ast_t* pattern_type, LLVMBasicBlockRef next_block, match_weight_t weight)
{
  switch(ast_id(pattern_type))
  {
    case TK_NOMINAL:
      // We are trying to capture the match expression as a nominal.
      return check_nominal(c, desc, pattern_type, next_block, weight);

    case TK_TUPLETYPE:
      // We are trying to capture the match expression as a tuple.
      return check_tuple(c, ptr, desc, pattern_type, next_block, weight);

    case TK_UNIONTYPE:
    case TK_ISECTTYPE:
      // We are trying to capture the match expression as a union or intersection.
      return check_union_or_isect(c, desc, pattern_type, next_block, weight);

    case TK_ARROW:
      // We are trying to capture the match expression as a viewpoint type, so
      // try again with the right-hand side of the arrow.
      return check_type(c, ptr, desc, ast_childidx(pattern_type, 1),
        next_block, weight);

    default: {}
  }

  pony_assert(0);
  return false;
}

static bool check_value(compile_t* c, ast_t* pattern, ast_t* param_type,
  LLVMValueRef value, LLVMBasicBlockRef next_block)
{
  reach_type_t* t = reach_type(c->reach, param_type);
  LLVMValueRef r_value = gen_assign_cast(c,
    ((compile_type_t*)t->c_type)->use_type, value, param_type);

  if(r_value == NULL)
    return false;

  LLVMValueRef result = gen_pattern_eq(c, pattern, r_value);

  if(result == NULL)
    return false;

  LLVMBasicBlockRef continue_block = codegen_block(c, "pattern_continue");
  if(next_block == NULL)
    next_block = continue_block;

  LLVMValueRef br = LLVMBuildCondBr(c->builder, result, continue_block,
    next_block);

  handle_branch_prediction_default(c->context, br, ast_parent(pattern));

  LLVMPositionBuilderAtEnd(c->builder, continue_block);
  return true;
}

static bool dynamic_tuple_element(compile_t* c, LLVMValueRef ptr,
  LLVMValueRef desc, ast_t* pattern, LLVMBasicBlockRef next_block, int elem)
{
  // If we have a capture, generate the alloca now.
  switch(ast_id(pattern))
  {
    case TK_MATCH_CAPTURE:
      if(gen_localdecl(c, pattern) == NULL)
        return false;
      break;

    case TK_DONTCAREREF:
      // Ignore the element.
      return true;

    default: {}
  }

  // Get the field offset and field descriptor from the tuple descriptor.
  LLVMValueRef field_info = gendesc_fieldinfo(c, desc, elem);
  LLVMValueRef field_ptr = gendesc_fieldptr(c, ptr, field_info);
  LLVMValueRef field_desc = gendesc_fielddesc(c, field_info);

  // If we have a null descriptor, load the object.
  LLVMBasicBlockRef null_block = codegen_block(c, "null_desc");
  LLVMBasicBlockRef nonnull_block = codegen_block(c, "nonnull_desc");
  LLVMBasicBlockRef continue_block = codegen_block(c, "merge_desc");
  LLVMValueRef test = LLVMBuildIsNull(c->builder, field_desc, "");
  LLVMBuildCondBr(c->builder, test, null_block, nonnull_block);

  // Load the object, load its descriptor, and continue from there.
  LLVMPositionBuilderAtEnd(c->builder, null_block);
  LLVMValueRef object_ptr = field_ptr;
  LLVMValueRef object = LLVMBuildLoad2(c->builder, c->ptr, object_ptr, "");
  LLVMValueRef object_desc = gendesc_fetch(c, object);

  if(!dynamic_match_object(c, object, object_desc, pattern, next_block))
    return false;

  LLVMBuildBr(c->builder, continue_block);

  // Continue with the pointer and descriptor.
  LLVMMoveBasicBlockAfter(nonnull_block, LLVMGetInsertBlock(c->builder));
  LLVMPositionBuilderAtEnd(c->builder, nonnull_block);

  if(!dynamic_match_ptr(c, field_ptr, field_desc, pattern, next_block))
    return false;

  LLVMBuildBr(c->builder, continue_block);

  // Merge the two branches.
  LLVMMoveBasicBlockAfter(continue_block, LLVMGetInsertBlock(c->builder));
  LLVMPositionBuilderAtEnd(c->builder, continue_block);
  return true;
}

static bool dynamic_tuple_ptr(compile_t* c, LLVMValueRef ptr,
  LLVMValueRef desc, ast_t* pattern, LLVMBasicBlockRef next_block)
{
  // First check cardinality.
  size_t size = ast_childcount(pattern);
  check_cardinality(c, desc, size, next_block);

  // If we get here, the match expression has the right cardinality.
  ast_t* pattern_child = ast_child(pattern);

  for(int i = 0; pattern_child != NULL; i++)
  {
    pony_assert(ast_id(pattern_child) == TK_SEQ);

    // Skip over the SEQ node.
    ast_t* pattern_expr = ast_child(pattern_child);

    if(!dynamic_tuple_element(c, ptr, desc, pattern_expr, next_block, i))
      return false;

    pattern_child = ast_sibling(pattern_child);
  }

  return true;
}

static bool dynamic_value_ptr(compile_t* c, LLVMValueRef ptr,
  LLVMValueRef desc, ast_t* pattern, LLVMBasicBlockRef next_block)
{
  // Get the type of the right-hand side of the pattern's eq() function.
  ast_t* param_type = eq_param_type(c, pattern);

  ast_t* the_case = ast_parent(pattern);
  match_weight_t weight;
  if(ast_has_annotation(the_case, "likely"))
    weight = WEIGHT_LIKELY;
  else if(ast_has_annotation(the_case, "unlikely"))
    weight = WEIGHT_UNLIKELY;
  else
    weight = WEIGHT_NONE;

  // Check the runtime type. We pass a pointer to the fields because we may
  // still need to match a tuple type inside a type expression.
  if(!check_type(c, ptr, desc, param_type, next_block, weight))
    return false;

  // We now know that ptr points to something of type pattern_type, and that
  // it isn't a boxed primitive, as that would go through the other path, ie
  // dynamic_match_object(). We also know it isn't an unboxed tuple. We can
  // load from ptr with a type based on the static type of the pattern.
  reach_type_t* t = reach_type(c->reach, param_type);
  LLVMTypeRef use_type = ((compile_type_t*)t->c_type)->use_type;
  LLVMValueRef value = LLVMBuildLoad2(c->builder, use_type, ptr, "");

  return check_value(c, pattern, param_type, value, next_block);
}

static bool dynamic_capture_ptr(compile_t* c, LLVMValueRef ptr,
  LLVMValueRef desc, ast_t* pattern, LLVMBasicBlockRef next_block)
{
  // Here, ptr is a pointer to a tuple field. It could be a primitive, an
  // object, or a nested tuple.
  ast_t* pattern_type = deferred_reify(c->frame->reify, ast_type(pattern),
    c->opt);

  ast_t* the_case = ast_parent(pattern);
  match_weight_t weight;
  if(ast_has_annotation(the_case, "likely"))
    weight = WEIGHT_LIKELY;
  else if(ast_has_annotation(the_case, "unlikely"))
    weight = WEIGHT_UNLIKELY;
  else
    weight = WEIGHT_NONE;

  // Check the runtime type. We pass a pointer to the fields because we may
  // still need to match a tuple type inside a type expression.
  if(!check_type(c, ptr, desc, pattern_type, next_block, weight))
  {
    ast_free_unattached(pattern_type);
    return false;
  }

  // We now know that ptr points to something of type pattern_type, and that
  // it isn't a boxed primitive or tuple, as that would go through the other
  // path, ie dynamic_match_object(). We also know it isn't an unboxed tuple.
  // We can load from ptr with a type based on the static type of the pattern.
  reach_type_t* t = reach_type(c->reach, pattern_type);
  LLVMTypeRef use_type = ((compile_type_t*)t->c_type)->use_type;
  LLVMValueRef value = LLVMBuildLoad2(c->builder, use_type, ptr, "");

  LLVMValueRef r = gen_assign_value(c, pattern, value, pattern_type);

  ast_free_unattached(pattern_type);
  return r != NULL;
}

static bool dynamic_match_ptr(compile_t* c, LLVMValueRef ptr,
  LLVMValueRef desc, ast_t* pattern, LLVMBasicBlockRef next_block)
{
  switch(ast_id(pattern))
  {
    case TK_NONE:
      return true;

    case TK_MATCH_CAPTURE:
    case TK_MATCH_DONTCARE:
      // Capture the match expression (or element thereof).
      return dynamic_capture_ptr(c, ptr, desc, pattern, next_block);

    case TK_TUPLE:
    {
      // Treat a one element tuple as its component expression.
      ast_t* child = ast_child(pattern);

      if(ast_sibling(child) == NULL)
        return dynamic_match_ptr(c, ptr, desc, child, next_block);

      // Destructure the match expression (or element thereof).
      return dynamic_tuple_ptr(c, ptr, desc, pattern, next_block);
    }

    default:
      // Test the match expression (or element thereof).
      return dynamic_value_ptr(c, ptr, desc, pattern, next_block);
  }

  return true;
}

static bool dynamic_value_object(compile_t* c, LLVMValueRef object,
  LLVMValueRef desc, ast_t* pattern, LLVMBasicBlockRef next_block)
{
  // Get the type of the right-hand side of the pattern's eq() function.
  ast_t* param_type = eq_param_type(c, pattern);

  // Build a base pointer that skips the object header.
  LLVMValueRef ptr = gendesc_ptr_to_fields(c, object, desc);

  ast_t* the_case = ast_parent(pattern);
  match_weight_t weight;
  if(ast_has_annotation(the_case, "likely"))
    weight = WEIGHT_LIKELY;
  else if(ast_has_annotation(the_case, "unlikely"))
    weight = WEIGHT_UNLIKELY;
  else
    weight = WEIGHT_NONE;

  // Check the runtime type. We pass a pointer to the fields because we may
  // still need to match a tuple type inside a type expression.
  if(!check_type(c, ptr, desc, param_type, next_block, weight))
    return false;

  return check_value(c, pattern, param_type, object, next_block);
}

static bool dynamic_capture_object(compile_t* c, LLVMValueRef object,
  LLVMValueRef desc, ast_t* pattern, LLVMBasicBlockRef next_block)
{
  ast_t* pattern_type = deferred_reify(c->frame->reify, ast_type(pattern),
    c->opt);

  // Build a base pointer that skips the object header.
  LLVMValueRef ptr = gendesc_ptr_to_fields(c, object, desc);

  ast_t* the_case = ast_parent(pattern);
  match_weight_t weight;
  if(ast_has_annotation(the_case, "likely"))
    weight = WEIGHT_LIKELY;
  else if(ast_has_annotation(the_case, "unlikely"))
    weight = WEIGHT_UNLIKELY;
  else
    weight = WEIGHT_NONE;

  // Check the runtime type. We pass a pointer to the fields because we may
  // still need to match a tuple type inside a type expression.
  if(!check_type(c, ptr, desc, pattern_type, next_block, weight))
  {
    ast_free_unattached(pattern_type);
    return false;
  }

  // As long as the type is correct, we can assign it, with gen_assign_value()
  // handling boxing and unboxing. We pass the type of the pattern as the type
  // of the object.
  LLVMValueRef r = gen_assign_value(c, pattern, object, pattern_type);

  ast_free_unattached(pattern_type);
  return r != NULL;
}

static bool dynamic_match_object(compile_t* c, LLVMValueRef object,
  LLVMValueRef desc, ast_t* pattern, LLVMBasicBlockRef next_block)
{
  switch(ast_id(pattern))
  {
    case TK_NONE:
    case TK_DONTCAREREF:
      return true;

    case TK_MATCH_CAPTURE:
    case TK_MATCH_DONTCARE:
      // Capture the match expression (or element thereof).
      return dynamic_capture_object(c, object, desc, pattern, next_block);

    case TK_TUPLE:
    {
      // Treat a one element tuple as its component expression.
      ast_t* child = ast_child(pattern);

      if(ast_sibling(child) == NULL)
        return dynamic_match_object(c, object, desc, child, next_block);

      // Build a base pointer that skips the object header.
      LLVMValueRef ptr = gendesc_ptr_to_fields(c, object, desc);

      // Destructure the match expression (or element thereof).
      return dynamic_tuple_ptr(c, ptr, desc, pattern, next_block);
    }

    default:
      // Test the match expression (or element thereof).
      return dynamic_value_object(c, object, desc, pattern, next_block);
  }

  return true;
}

static bool static_tuple_from_tuple(compile_t* c, LLVMValueRef value,
  ast_t* type, ast_t* pattern, LLVMBasicBlockRef next_block)
{
  // The match expression is a tuple. The type checker will have made sure
  // it's the right cardinality and each element has a useful type relation
  // with the pattern.
  pony_assert(LLVMGetTypeKind(LLVMTypeOf(value)) == LLVMStructTypeKind);

  // We have a static type.
  ast_t* type_child = ast_child(type);
  ast_t* pattern_child = ast_child(pattern);

  // Destructure the tuple and continue pattern matching on each element.
  for(int i = 0; pattern_child != NULL; i++)
  {
    pony_assert(ast_id(pattern_child) == TK_SEQ);

    // Skip over the SEQ node.
    ast_t* pattern_expr = ast_child(pattern_child);
    LLVMValueRef elem = LLVMBuildExtractValue(c->builder, value, i, "");

    if(!static_match(c, elem, type_child, pattern_expr, next_block))
      return false;

    type_child = ast_sibling(type_child);
    pattern_child = ast_sibling(pattern_child);
  }

  pony_assert(type_child == NULL);
  return true;
}

static bool static_tuple(compile_t* c, LLVMValueRef value, ast_t* type,
  ast_t* pattern, LLVMBasicBlockRef next_block)
{
  switch(ast_id(type))
  {
    case TK_UNIONTYPE:
    case TK_ISECTTYPE:
    case TK_NOMINAL:
    {
      pony_assert((ast_id(type) != TK_NOMINAL) || is_top_type(type, true));

      // Read the dynamic type and get a base pointer.
      LLVMValueRef desc = gendesc_fetch(c, value);
      LLVMValueRef ptr = gendesc_ptr_to_fields(c, value, desc);
      return dynamic_tuple_ptr(c, ptr, desc, pattern, next_block);
    }

    case TK_TUPLETYPE:
      return static_tuple_from_tuple(c, value, type, pattern, next_block);

    case TK_ARROW:
      return static_tuple(c, value, ast_childidx(type, 1), pattern,
        next_block);

    default: {}
  }

  // Can't match.
  LLVMBuildBr(c->builder, next_block);
  return true;
}

static bool static_value(compile_t* c, LLVMValueRef value, ast_t* type,
  ast_t* pattern, LLVMBasicBlockRef next_block)
{
  // Get the type of the right-hand side of the pattern's eq() function.
  ast_t* param_type = eq_param_type(c, pattern);

  if(!is_subtype_ignore_cap(type, param_type, NULL, c->opt))
  {
    // Switch to dynamic value checking.
    pony_assert(LLVMTypeOf(value) == c->ptr);
    LLVMValueRef desc = gendesc_fetch(c, value);
    return dynamic_value_object(c, value, desc, pattern, next_block);
  }

  return check_value(c, pattern, param_type, value, next_block);
}

static bool static_capture(compile_t* c, LLVMValueRef value, ast_t* type,
  ast_t* pattern, LLVMBasicBlockRef next_block)
{
  // The pattern is a capture. Make sure we are the right type, then assign.

  if(ast_id(pattern) == TK_MATCH_CAPTURE)
  {
    // Generate the alloca
    if(gen_localdecl(c, pattern) == NULL)
      return false;
  }

  ast_t* pattern_type = deferred_reify(c->frame->reify, ast_type(pattern),
    c->opt);

  bool is_sub = is_subtype_ignore_cap(type, pattern_type, NULL, c->opt);

  ast_free_unattached(pattern_type);

  if(!is_sub)
  {
    // Switch to dynamic capture.
    pony_assert(LLVMTypeOf(value) == c->ptr);
    LLVMValueRef desc = gendesc_fetch(c, value);
    return dynamic_capture_object(c, value, desc, pattern, next_block);
  }

  return gen_assign_value(c, pattern, value, type) != NULL;
}

static bool static_match(compile_t* c, LLVMValueRef value, ast_t* type,
  ast_t* pattern, LLVMBasicBlockRef next_block)
{
  switch(ast_id(pattern))
  {
    case TK_NONE:
    case TK_DONTCAREREF:
      return true;

    case TK_MATCH_CAPTURE:
    case TK_MATCH_DONTCARE:
      // Capture the match expression (or element thereof).
      return static_capture(c, value, type, pattern, next_block);

    case TK_SEQ:
    {
      // Treat a one element sequence as its component expression.
      // We already checked that the sequence doesn't have multiple elements
      // during type checking.
      ast_t* child = ast_child(pattern);

      pony_assert(child != NULL);
      pony_assert(ast_sibling(child) == NULL);

      // Pass on the element in the seq instead of the seq.
      return static_match(c, value, type, child, next_block);
    }

    case TK_TUPLE:
    {
      // Tuples must have multiple elements, or they aren't tuples.
      pony_assert(ast_child(pattern) != NULL);
      pony_assert(ast_sibling(ast_child(pattern)) != NULL);

      // Destructure the match expression (or element thereof).
      return static_tuple(c, value, type, pattern, next_block);
    }

    default:
      // Test the match expression (or element thereof).
      return static_value(c, value, type, pattern, next_block);
  }

  return true;
}

static bool guard_match(compile_t* c, ast_t* guard,
  LLVMBasicBlockRef next_block)
{
  if(ast_id(guard) == TK_NONE)
    return true;

  LLVMValueRef value = gen_expr(c, guard);

  if(value == NULL)
    return false;

  LLVMBasicBlockRef continue_block = codegen_block(c, "pattern_continue");
  if(next_block == NULL)
    next_block = continue_block;

  LLVMBuildCondBr(c->builder, value, continue_block, next_block);
  LLVMPositionBuilderAtEnd(c->builder, continue_block);
  return true;
}

static bool case_body(compile_t* c, ast_t* body,
  LLVMBasicBlockRef post_block, LLVMValueRef phi, LLVMTypeRef phi_type)
{
  LLVMValueRef body_value = gen_expr(c, body);

  // If it returns, we don't branch to the post block.
  if(body_value == GEN_NOVALUE)
    return true;

  if(is_result_needed(body))
  {
    ast_t* body_type = deferred_reify(c->frame->reify, ast_type(body), c->opt);
    body_value = gen_assign_cast(c, phi_type, body_value, body_type);
    ast_free_unattached(body_type);

    if(body_value == NULL)
      return false;

    LLVMBasicBlockRef block = LLVMGetInsertBlock(c->builder);
    LLVMAddIncoming(phi, &body_value, &block, 1);
  }

  codegen_scope_lifetime_end(c);
  LLVMBuildBr(c->builder, post_block);
  return true;
}

LLVMValueRef gen_match(compile_t* c, ast_t* ast)
{
  bool needed = is_result_needed(ast);
  AST_GET_CHILDREN(ast, match_expr, cases, else_expr);

  // We will have no type if all cases jump away.
  LLVMTypeRef phi_type = NULL;

  deferred_reification_t* reify = c->frame->reify;

  if(needed && !ast_checkflag(ast, AST_FLAG_JUMPS_AWAY))
  {
    ast_t* type = deferred_reify(reify, ast_type(ast), c->opt);
    reach_type_t* t_phi = reach_type(c->reach, type);
    phi_type = ((compile_type_t*)t_phi->c_type)->use_type;
    ast_free_unattached(type);
  }

  ast_t* match_type = deferred_reify(reify, ast_type(match_expr), c->opt);

  LLVMValueRef match_value = gen_expr(c, match_expr);

  LLVMBasicBlockRef pattern_block = codegen_block(c, "case_pattern");
  LLVMBasicBlockRef else_block = NULL;
  LLVMBasicBlockRef post_block = NULL;
  LLVMBasicBlockRef next_block = NULL;

  // Jump to the first case.
  LLVMBuildBr(c->builder, pattern_block);

  LLVMValueRef phi = GEN_NOVALUE;

  if(!ast_checkflag(ast, AST_FLAG_JUMPS_AWAY))
  {
    // Start the post block so that a case can modify the phi node.
    post_block = codegen_block(c, "match_post");
    LLVMPositionBuilderAtEnd(c->builder, post_block);

    if(needed)
      phi = LLVMBuildPhi(c->builder, phi_type, "");
    else
      phi = GEN_NOTNEEDED;
  }

  // Create else block only if this isn't an exhaustive match
  // (a match with no else expression).
  if(ast_id(else_expr) != TK_NONE)
    else_block = codegen_block(c, "match_else");

  // Iterate over the cases.
  ast_t* the_case = ast_child(cases);

  while(the_case != NULL)
  {
    ast_t* next_case = ast_sibling(the_case);

    if(next_case != NULL)
      next_block = codegen_block(c, "case_pattern");
    else
      next_block = else_block;

    AST_GET_CHILDREN(the_case, pattern, guard, body);
    LLVMPositionBuilderAtEnd(c->builder, pattern_block);
    codegen_pushscope(c, the_case);

    ast_t* pattern_type = deferred_reify(reify, ast_type(the_case), c->opt);
    bool ok = true;

    matchtype_t match = is_matchtype(match_type, pattern_type, NULL, c->opt);

    ast_free_unattached(pattern_type);

    if(match != MATCHTYPE_ACCEPT)
    {
      if (next_block != NULL)
      {
        // If there's no possible match, jump directly to the next block.
        LLVMBuildBr(c->builder, next_block);
      } else {
        LLVMBuildUnreachable(c->builder);
      }
    } else {
      // Check the pattern.
      ok = static_match(c, match_value, match_type, pattern, next_block);

      // Check the guard.
      ok = ok && guard_match(c, guard, next_block);

      // Case body.
      ok = ok && case_body(c, body, post_block, phi, phi_type);

      if(next_block != NULL)
        LLVMMoveBasicBlockAfter(next_block, LLVMGetInsertBlock(c->builder));
    }

    codegen_popscope(c);

    if(!ok)
    {
      ast_free_unattached(match_type);
      return NULL;
    }

    the_case = next_case;
    pattern_block = next_block;
  }

  ast_free_unattached(match_type);

  // Else body.
  if(else_block != NULL)
  {
    LLVMMoveBasicBlockAfter(else_block, LLVMGetInsertBlock(c->builder));
    LLVMPositionBuilderAtEnd(c->builder, else_block);
    codegen_pushscope(c, else_expr);
    bool ok = case_body(c, else_expr, post_block, phi, phi_type);
    codegen_scope_lifetime_end(c);
    codegen_popscope(c);

    if(!ok)
      return NULL;
  }

  if(post_block != NULL)
  {
    LLVMMoveBasicBlockAfter(post_block, LLVMGetInsertBlock(c->builder));
    LLVMPositionBuilderAtEnd(c->builder, post_block);
  }

  return phi;
}
