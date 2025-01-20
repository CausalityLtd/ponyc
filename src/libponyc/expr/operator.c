#include "operator.h"
#include "literal.h"
#include "postfix.h"
#include "control.h"
#include "reference.h"
#include "call.h"
#include "../ast/astbuild.h"
#include "../ast/id.h"
#include "../ast/lexer.h"
#include "../pass/expr.h"
#include "../pass/syntax.h"
#include "../pkg/package.h"
#include "../type/alias.h"
#include "../type/assemble.h"
#include "../type/matchtype.h"
#include "../type/safeto.h"
#include "../type/subtype.h"
#include "ponyassert.h"

bool expr_identity(pass_opt_t* opt, ast_t* ast)
{
  ast_settype(ast, type_builtin(opt, ast, "Bool"));
  return literal_is(ast, opt);
}

typedef struct infer_path_t
{
  size_t index;
  struct infer_path_t* next; // Next node
  struct infer_path_t* root; // Root node in list
} infer_path_t;

typedef enum infer_ret_t
{
  INFER_OK,
  INFER_NOP,
  INFER_ERROR
} infer_ret_t;

static ast_t* find_infer_type(pass_opt_t* opt, ast_t* type, infer_path_t* path)
{
  pony_assert(type != NULL);

  switch(ast_id(type))
  {
    case TK_TUPLETYPE:
      if(path == NULL)  // End of path, infer the whole tuple
        return type;

      if(path->index >= ast_childcount(type)) // Cardinality mismatch
        return NULL;

      return find_infer_type(opt, ast_childidx(type, path->index), path->next);

    case TK_UNIONTYPE:
    {
      // Infer all children
      ast_t* u_type = NULL;

      for(ast_t* p = ast_child(type); p != NULL; p = ast_sibling(p))
      {
        ast_t* t = find_infer_type(opt, p, path);

        if(t == NULL)
        {
          // Propagate error
          ast_free_unattached(u_type);
          return NULL;
        }

        u_type = type_union(opt, u_type, t);
      }

      return u_type;
    }

    case TK_ISECTTYPE:
    {
      // Infer all children
      ast_t* i_type = NULL;

      for(ast_t* p = ast_child(type); p != NULL; p = ast_sibling(p))
      {
        ast_t* t = find_infer_type(opt, p, path);

        if(t == NULL)
        {
          // Propagate error
          ast_free_unattached(i_type);
          return NULL;
        }

        i_type = type_isect(opt, i_type, t);
      }

      return i_type;
    }

    default:
      if(path != NULL)  // Type doesn't match path
        return NULL;

      // Just return whatever this type is
      return type;
  }
}

static infer_ret_t infer_local_inner(pass_opt_t* opt, ast_t* left,
  ast_t* r_type, infer_path_t* path)
{
  pony_assert(left != NULL);
  pony_assert(r_type != NULL);
  pony_assert(path != NULL);
  pony_assert(path->root != NULL);

  infer_ret_t ret_val = INFER_NOP;

  switch(ast_id(left))
  {
    case TK_SEQ:
    {
      pony_assert(ast_childcount(left) == 1);
      infer_ret_t r = infer_local_inner(opt, ast_child(left), r_type, path);

      if(r == INFER_OK) // Update seq type
        ast_settype(left, ast_type(ast_child(left)));

      return r;
    }

    case TK_TUPLE:
    {
      // Add a new node to the end of the path
      infer_path_t path_node = { 0, NULL, path->root };
      path->next = &path_node;

      for(ast_t* p = ast_child(left); p != NULL; p = ast_sibling(p))
      {
        infer_ret_t r = infer_local_inner(opt, p, r_type, &path_node);

        if(r == INFER_ERROR)
          return INFER_ERROR;

        if(r == INFER_OK)
        {
          // Update tuple type element to remove infer type
          ast_t* old_ele = ast_childidx(ast_type(left), path_node.index);
          ast_replace(&old_ele, ast_type(p));
          ret_val = INFER_OK;
        }

        path_node.index++;
      }

      // Pop our node off the path
      path->next = NULL;
      return ret_val;
    }

    case TK_VAR:
    case TK_LET:
    {
      ast_t* var_type = ast_type(left);
      pony_assert(var_type != NULL);

      if(ast_id(var_type) != TK_INFERTYPE)  // No inferring needed
        return INFER_NOP;

      ast_t* infer_type = find_infer_type(opt, r_type, path->root->next);

      if(infer_type == NULL)
      {
        ast_error(opt->check.errors, left, "could not infer type of local");
        ast_settype(left, ast_from(left, TK_ERRORTYPE));
        return INFER_ERROR;
      }

      // Variable type is the alias of the inferred type
      ast_t* a_type = alias(infer_type);
      ast_settype(left, a_type);
      ast_settype(ast_child(left), a_type);

      // Add the type to the var / let AST as if it had been specified by the
      // user. Not really needed, but makes testing easier
      ast_t* speced_type = ast_childidx(left, 1);
      pony_assert(ast_id(speced_type) == TK_NONE);
      ast_replace(&speced_type, a_type);

      ast_free_unattached(infer_type);
      return INFER_OK;
    }

    default:
      // No locals to infer here
      return INFER_NOP;
  }
}

static bool infer_locals(pass_opt_t* opt, ast_t* left, ast_t* r_type)
{
  infer_path_t path_root = { 0, NULL, NULL };
  path_root.root = &path_root;

  if(infer_local_inner(opt, left, r_type, &path_root) == INFER_ERROR)
    return false;

  pony_assert(path_root.next == NULL);
  pony_assert(path_root.root = &path_root);
  return true;
}

static bool is_expr_constructor(ast_t* ast)
{
  pony_assert(ast != NULL);
  switch(ast_id(ast))
  {
    case TK_CALL:
      return ast_id(ast_child(ast)) == TK_NEWREF;
    case TK_SEQ:
      return is_expr_constructor(ast_childlast(ast));
    case TK_IF:
    case TK_WHILE:
    case TK_REPEAT:
    {
      ast_t* body = ast_childidx(ast, 1);
      ast_t* else_expr = ast_childidx(ast, 2);
      return is_expr_constructor(body) && is_expr_constructor(else_expr);
    }
    case TK_TRY:
    {
      ast_t* body = ast_childidx(ast, 0);
      ast_t* else_expr = ast_childidx(ast, 1);
      return is_expr_constructor(body) && is_expr_constructor(else_expr);
    }
    case TK_DISPOSING_BLOCK:
    {
      ast_t* body = ast_childidx(ast, 0);
      return is_expr_constructor(body);
    }
    case TK_MATCH:
    {
      ast_t* cases = ast_childidx(ast, 1);
      ast_t* else_expr = ast_childidx(ast, 2);
      ast_t* the_case = ast_child(cases);

      while(the_case != NULL)
      {
        if(!is_expr_constructor(ast_childidx(the_case, 2)))
          return false;
        the_case = ast_sibling(the_case);
      }
      return is_expr_constructor(else_expr);
    }
    case TK_RECOVER:
      return is_expr_constructor(ast_childidx(ast, 1));
    default:
      return false;
  }
}

static bool tuple_contains_embed(ast_t* ast)
{
  pony_assert(ast_id(ast) == TK_TUPLE);
  ast_t* child = ast_child(ast);
  while(child != NULL)
  {
    pony_assert(ast_id(child) == TK_SEQ);
    ast_t* member = ast_childlast(child);
    if(ast_id(member) == TK_EMBEDREF)
    {
      return true;
    } else if(ast_id(member) == TK_TUPLE) {
      if(tuple_contains_embed(member))
        return true;
    }
    child = ast_sibling(child);
  }
  return false;
}

static bool check_embed_construction(pass_opt_t* opt, ast_t* left, ast_t* right)
{
  bool result = true;
  if(ast_id(left) == TK_EMBEDREF)
  {
    if(!is_expr_constructor(right))
    {
      ast_error(opt->check.errors, left,
        "an embedded field must be assigned using a constructor");
      ast_error_continue(opt->check.errors, right,
        "the assigned expression is here");
      return false;
    }
  } else if(ast_id(left) == TK_TUPLE) {
    if(ast_id(right) == TK_TUPLE)
    {
      ast_t* l_child = ast_child(left);
      ast_t* r_child = ast_child(right);
      while(l_child != NULL)
      {
        pony_assert((ast_id(l_child) == TK_SEQ) && (ast_id(r_child) == TK_SEQ));
        ast_t* l_member = ast_childlast(l_child);
        ast_t* r_member = ast_childlast(r_child);
        if(!check_embed_construction(opt, l_member, r_member))
          result = false;

        l_child = ast_sibling(l_child);
        r_child = ast_sibling(r_child);
      }
      pony_assert(r_child == NULL);
    } else if(tuple_contains_embed(left)) {
      ast_error(opt->check.errors, left,
        "an embedded field must be assigned using a constructor");
      ast_error_continue(opt->check.errors, right,
        "the assigned expression isn't a tuple literal");
    }
  }
  return result;
}

bool expr_assign(pass_opt_t* opt, ast_t* ast)
{
  // Left and right are swapped in the AST to make sure we type check the
  // right side before the left. Fetch them in the opposite order.
  pony_assert(ast_id(ast) == TK_ASSIGN);
  AST_GET_CHILDREN(ast, left, right);
  ast_t* l_type = ast_type(left);

  if(l_type == NULL)
  {
    ast_error(opt->check.errors, ast,
      "left side must be something that can be assigned to");
    return false;
  }

  if(!coerce_literals(&right, l_type, opt))
    return false;

  ast_t* r_type = ast_type(right);

  if(is_typecheck_error(r_type))
  {
    if(errors_get_count(opt->check.errors) == 0){
      ast_error(opt->check.errors, right,
        "right side must be something that can be assigned");
    }
    return false;
  }

  if(ast_checkflag(right, AST_FLAG_JUMPS_AWAY))
  {
    ast_error(opt->check.errors, ast,
      "the right hand side jumps away without a value");
    return false;
  }

  if(!infer_locals(opt, left, r_type))
    return false;

  // Inferring locals may have changed the left type.
  l_type = ast_type(left);
  ast_t* fl_type = l_type;

  switch(ast_id(left))
  {
    case TK_FVARREF:
    case TK_FLETREF:
    {
      // Must be a subtype of the field type, not the viewpoint adapted type.
      AST_GET_CHILDREN(left, origin, field);
      fl_type = ast_type(field);
      break;
    }

    default: {}
  }

  ast_t* wl_type = consume_type(fl_type, TK_NONE, false);
  if(check_auto_recover_newref(fl_type, right))
  {
    token_id left_cap = ast_id(cap_fetch(wl_type));
    ast_t* recovered_left_type = recover_type(r_type, left_cap);
    if (recovered_left_type)
      r_type = recovered_left_type;
  }

  // Assignment is based on the alias of the right hand side.
  errorframe_t info = NULL;
  errorframe_t frame = NULL;
  if(wl_type == NULL)
  {
    ast_error_frame(&frame, ast, "Invalid type for field of assignment: %s",
        ast_print_type(fl_type));

    if(ast_checkflag(ast_type(right), AST_FLAG_INCOMPLETE))
      ast_error_frame(&frame, right,
        "this might be possible if all fields were already defined");

    errorframe_append(&frame, &info);
    errorframe_report(&frame, opt->check.errors);
    return false;
  }
  else if(!is_subtype(r_type, wl_type, &info, opt))
  {
    ast_error_frame(&frame, ast, "right side must be a subtype of left side");

    if(ast_checkflag(ast_type(right), AST_FLAG_INCOMPLETE))
      ast_error_frame(&frame, right,
        "this might be possible if all fields were already defined");

    errorframe_append(&frame, &info);
    errorframe_report(&frame, opt->check.errors);
    ast_free_unattached(wl_type);
    return false;
  }

  if((ast_id(left) == TK_TUPLE) && (ast_id(r_type) != TK_TUPLETYPE))
  {
    switch(ast_id(r_type))
    {
      case TK_UNIONTYPE:
        ast_error(opt->check.errors, ast,
          "can't destructure a union using assignment, use pattern matching "
          "instead");
        ast_error_continue(opt->check.errors, right,
          "inferred type of expression: %s", ast_print_type(r_type));
        break;

      case TK_ISECTTYPE:
        ast_error(opt->check.errors, ast,
          "can't destructure an intersection using assignment, use pattern "
          "matching instead");
        ast_error_continue(opt->check.errors, right,
          "inferred type of expression: %s", ast_print_type(r_type));
        break;

      default:
        pony_assert(0);
        break;
    }

    ast_free_unattached(wl_type);
    return false;
  }

  bool ok_mutable = safe_to_mutate(left);
  if(!ok_mutable)
  {
    ast_error(opt->check.errors, ast,
      "left side is immutable");
    ast_free_unattached(wl_type);
    return false;
  }

  bool ok_safe = safe_to_move(left, r_type, WRITE);

  if(!ok_safe)
  {
    if(ast_id(left) == TK_FVARREF && ast_child(left) != NULL &&
      ast_id(ast_child(left)) == TK_THIS)
    {
      // We are writing to a field in this
      ast_t* fn = ast_nearest(left, TK_FUN);

      if(fn != NULL)
      {
        ast_t* iso = ast_child(fn);
        pony_assert(iso != NULL);
        token_id iso_id = ast_id(iso);

        if(iso_id == TK_BOX || iso_id == TK_VAL || iso_id == TK_TAG)
        {
          ast_error(opt->check.errors, ast,
            "cannot write to a field in a %s function. If you are trying to "
            "change state in a function use fun ref",
            lexer_print(iso_id));
          ast_free_unattached(wl_type);
          return false;
        }
      }
    }

    ast_error(opt->check.errors, ast,
      "not safe to write right side to left side");
    ast_error_continue(opt->check.errors, wl_type, "right side type: %s",
      ast_print_type(wl_type));
    if(ast_child(left) != NULL)
    {
      ast_error_continue(opt->check.errors, ast_child(left), "left side type: %s",
      ast_print_type(ast_type(ast_child(left))));
    }
    ast_free_unattached(wl_type);
    return false;
  }

  ast_free_unattached(wl_type);

  if(!check_embed_construction(opt, left, right))
    return false;

  ast_settype(ast, consume_type(l_type, TK_NONE, false));
  return true;
}

static bool add_as_type(pass_opt_t* opt, ast_t* ast, ast_t* expr,
  ast_t* type, ast_t* pattern, ast_t* body)
{
  pony_assert(type != NULL);

  switch(ast_id(type))
  {
    case TK_TUPLETYPE:
    {
      BUILD(tuple_pattern, pattern, NODE(TK_SEQ, NODE(TK_TUPLE)));
      ast_append(pattern, tuple_pattern);
      ast_t* pattern_child = ast_child(tuple_pattern);

      BUILD(tuple_body, body, NODE(TK_SEQ, NODE(TK_TUPLE)));
      ast_t* body_child = ast_child(tuple_body);

      for(ast_t* p = ast_child(type); p != NULL; p = ast_sibling(p))
      {
        if(!add_as_type(opt, ast, expr, p, pattern_child, body_child))
          return false;
      }

      if(ast_childcount(body_child) == 1)
      {
        // Only one child, not actually a tuple
        ast_t* t = ast_pop(body_child);
        ast_free(tuple_body);
        tuple_body = t;
      }

      ast_append(body, tuple_body);
      break;
    }

    case TK_DONTCARETYPE:
    {
      BUILD(dontcare, pattern,
        NODE(TK_SEQ,
          NODE(TK_REFERENCE, ID("_"))));
      ast_append(pattern, dontcare);
      break;
    }

    default:
    {
      const char* name = package_hygienic_id(&opt->check);

      ast_t* expr_type = ast_type(expr);
      errorframe_t info = NULL;
      matchtype_t is_match = is_astype(expr_type, type, &info, opt);
      if(is_match == MATCHTYPE_DENY_CAP)
      {
        errorframe_t frame = NULL;
        ast_error_frame(&frame, ast,
          "this capture violates capabilities");
        ast_error_frame(&frame, type,
          "match type: %s", ast_print_type(type));
        ast_error_frame(&frame, expr,
          "pattern type: %s", ast_print_type(expr_type));
        errorframe_append(&frame, &info);
        errorframe_report(&frame, opt->check.errors);

        return false;
      } else if(is_match == MATCHTYPE_DENY_NODESC){
        errorframe_t frame = NULL;
        ast_error_frame(&frame, ast,
          "matching variable of type %s with %s is not possible, "
          "since a struct lacks a type descriptor",
          ast_print_type(expr_type), ast_print_type(type));
        ast_error_frame(&frame, type,
          "match type: %s", ast_print_type(type));
        ast_error_frame(&frame, expr,
          "a struct cannot be part of a union type. "
          "pattern type: %s", ast_print_type(expr_type));
        errorframe_append(&frame, &info);
        errorframe_report(&frame, opt->check.errors);

        return false;
      }

      ast_t* a_type = alias(type);

      BUILD(pattern_elem, pattern,
        NODE(TK_SEQ,
          NODE(TK_LET, ID(name) TREE(a_type))));

      BUILD(body_elem, body,
        NODE(TK_SEQ,
          NODE(TK_CONSUME, NODE(TK_ALIASED) NODE(TK_REFERENCE, ID(name)))));

      ast_append(pattern, pattern_elem);
      ast_append(body, body_elem);
      break;
    }
  }

  return true;
}

bool expr_as(pass_opt_t* opt, ast_t** astp)
{
  pony_assert(astp != NULL);
  ast_t* ast = *astp;
  pony_assert(ast != NULL);

  pony_assert(ast_id(ast) == TK_AS);
  AST_GET_CHILDREN(ast, expr, type);

  ast_t* expr_type = ast_type(expr);
  if(is_typecheck_error(expr_type))
    return false;

  if(ast_id(expr_type) == TK_LITERAL || ast_id(expr_type) == TK_OPERATORLITERAL)
  {
    ast_error(opt->check.errors, expr, "Cannot cast uninferred literal");
    return false;
  }

  if(is_subtype(expr_type, type, NULL, opt))
  {
    if(is_subtype(type, expr_type, NULL, opt))
    {
      ast_error(opt->check.errors, ast, "Cannot cast to same type");
      ast_error_continue(opt->check.errors, expr,
        "Expression is already of type %s", ast_print_type(type));
    }
    else
    {
      ast_error(opt->check.errors, ast, "Cannot cast to subtype");
      ast_error_continue(opt->check.errors, expr,
        "%s is a subtype of this Expression. 'as' is not needed here.", ast_print_type(type));
    }
    return false;
  }

  ast_t* pattern_root = ast_from(type, TK_LEX_ERROR);
  ast_t* body_root = ast_from(type, TK_LEX_ERROR);
  if(!add_as_type(opt, ast, expr, type, pattern_root, body_root))
    return false;

  ast_t* body = ast_pop(body_root);
  ast_free(body_root);

  if(body == NULL)
  {
    // No body implies all types are "don't care"
    ast_error(opt->check.errors, ast, "Cannot treat value as \"don't care\"");
    ast_free(pattern_root);
    return false;
  }

  // Don't need top sequence in pattern
  pony_assert(ast_id(ast_child(pattern_root)) == TK_SEQ);
  ast_t* pattern = ast_pop(ast_child(pattern_root));
  ast_free(pattern_root);

  REPLACE(astp,
    NODE(TK_MATCH, AST_SCOPE
      NODE(TK_SEQ, TREE(expr))
      NODE(TK_CASES, AST_SCOPE
        NODE(TK_CASE, AST_SCOPE
          TREE(pattern)
          NONE
          TREE(body)))
      NODE(TK_SEQ, AST_SCOPE NODE(TK_ERROR, NONE))));

  if(!ast_passes_subtree(astp, opt, PASS_EXPR))
    return false;

  return true;
}

bool expr_consume(pass_opt_t* opt, ast_t* ast)
{
  pony_assert(ast_id(ast) == TK_CONSUME);
  AST_GET_CHILDREN(ast, cap, term);

  if( (ast_id(term) != TK_NONE)
    && (ast_id(term) != TK_FVARREF)
    && (ast_id(term) != TK_LETREF)
    && (ast_id(term) != TK_VARREF)
    && (ast_id(term) != TK_THIS)
    && (ast_id(term) != TK_DOT)
    && (ast_id(term) != TK_PARAMREF))
  {
    ast_error(opt->check.errors, ast, "can't consume let or embed fields");
    return false;
  }

  ast_t* type = ast_type(term);

  if(is_typecheck_error(type))
    return false;

  token_id tcap = ast_id(cap);
  ast_t* c_type = consume_type(type, tcap, false);

  if(c_type == NULL)
  {
    ast_error(opt->check.errors, ast, "can't consume to this capability");
    ast_error_continue(opt->check.errors, term, "expression type is %s",
      ast_print_type(type));
    return false;
  }

  ast_settype(ast, c_type);
  return true;
}
