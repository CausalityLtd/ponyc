#include "matchtype.h"
#include "cap.h"
#include "assemble.h"
#include "subtype.h"
#include "typeparam.h"
#include "viewpoint.h"
#include "ponyassert.h"
#include "alias.h"

static matchtype_t is_x_match_x(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt);

static matchtype_t is_union_match_x(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt)
{
  matchtype_t ok = MATCHTYPE_REJECT;

  for(ast_t* child = ast_child(operand);
    child != NULL;
    child = ast_sibling(child))
  {
    switch(is_x_match_x(child, pattern, NULL, false, opt))
    {
      case MATCHTYPE_ACCEPT:
        // If any type in the operand union accepts a match, then the entire
        // operand union accepts a match.
        ok = MATCHTYPE_ACCEPT;
        break;

      case MATCHTYPE_REJECT:
        break;

      // If any type in the operand union denies a match, then the entire
      // operand union is denied a match.
      case MATCHTYPE_DENY_CAP:
        ok = MATCHTYPE_DENY_CAP;
        break;

      case MATCHTYPE_DENY_NODESC:
        ok = MATCHTYPE_DENY_NODESC;
        break;
    }

    if((ok == MATCHTYPE_DENY_CAP) || (ok == MATCHTYPE_DENY_NODESC))
      break;
  }

  if((ok != MATCHTYPE_ACCEPT) && (errorf != NULL))
  {
    if((ok == MATCHTYPE_DENY_CAP) || (ok == MATCHTYPE_DENY_NODESC))
      report_reject = false;

    for(ast_t* child = ast_child(operand);
      child != NULL;
      child = ast_sibling(child))
    {
      is_x_match_x(child, pattern, errorf, report_reject, opt);
    }

    if(ok == MATCHTYPE_DENY_CAP)
    {
      ast_error_frame(errorf, pattern,
        "matching %s with %s could violate capabilities",
        ast_print_type(operand), ast_print_type(pattern));
    } else if(ok == MATCHTYPE_DENY_NODESC) {
      ast_error_frame(errorf, pattern,
        "matching %s with %s is not possible, since a struct lacks a type descriptor",
        ast_print_type(operand), ast_print_type(pattern));
    } else if(report_reject) {
      ast_error_frame(errorf, pattern, "no element of %s can match %s",
        ast_print_type(operand), ast_print_type(pattern));
    }
  }

  return ok;
}

static matchtype_t is_isect_match_x(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt)
{
  matchtype_t ok = MATCHTYPE_ACCEPT;

  for(ast_t* child = ast_child(operand);
    child != NULL;
    child = ast_sibling(child))
  {
    switch(is_x_match_x(child, pattern, NULL, false, opt))
    {
      case MATCHTYPE_ACCEPT:
        break;

      case MATCHTYPE_REJECT:
        // If any type in the operand isect rejects a match, then the entire
        // operand isect rejects match.
        ok = MATCHTYPE_REJECT;
        break;

      case MATCHTYPE_DENY_CAP:
        // If any type in the operand isect denies a match, then the entire
        // operand isect is denied a match.
        ok = MATCHTYPE_DENY_CAP;
        break;

      case MATCHTYPE_DENY_NODESC:
        ok = MATCHTYPE_DENY_NODESC;
        break;
    }

    if((ok == MATCHTYPE_DENY_CAP) || (ok == MATCHTYPE_DENY_NODESC))
      break;
  }

  if((ok != MATCHTYPE_ACCEPT) && (errorf != NULL))
  {
    if((ok == MATCHTYPE_DENY_CAP) || (ok == MATCHTYPE_DENY_NODESC))
      report_reject = false;

    for(ast_t* child = ast_child(operand);
      child != NULL;
      child = ast_sibling(child))
    {
      is_x_match_x(child, pattern, errorf, report_reject, opt);
    }

    if(ok == MATCHTYPE_DENY_CAP)
    {
      ast_error_frame(errorf, pattern,
        "matching %s with %s could violate capabilities",
        ast_print_type(operand), ast_print_type(pattern));
    } else if(ok == MATCHTYPE_DENY_NODESC) {
      ast_error_frame(errorf, pattern,
        "matching %s with %s is not possible, since a struct lacks a type descriptor",
        ast_print_type(operand), ast_print_type(pattern));
    } else if(report_reject) {
      ast_error_frame(errorf, pattern, "not every element of %s can match %s",
        ast_print_type(operand), ast_print_type(pattern));
    }
  }

  return ok;
}

static matchtype_t is_x_match_union(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt)
{
  matchtype_t ok = MATCHTYPE_REJECT;

  for(ast_t* child = ast_child(pattern);
    child != NULL;
    child = ast_sibling(child))
  {
    switch(is_x_match_x(operand, child, NULL, false, opt))
    {
      case MATCHTYPE_ACCEPT:
        // If any type in the pattern union accepts a match, the entire pattern
        // union accepts a match.
        ok = MATCHTYPE_ACCEPT;
        break;

      case MATCHTYPE_REJECT:
        break;

      case MATCHTYPE_DENY_CAP:
        // If any type in the pattern union denies a match, the entire pattern
        // union denies a match.
        ok = MATCHTYPE_DENY_CAP;
        break;

      case MATCHTYPE_DENY_NODESC:
        ok = MATCHTYPE_DENY_NODESC;
        break;
    }

    if((ok == MATCHTYPE_DENY_CAP) || (ok == MATCHTYPE_DENY_NODESC))
      break;
  }

  if((ok != MATCHTYPE_ACCEPT) && (errorf != NULL))
  {
    if((ok == MATCHTYPE_DENY_CAP) || (ok == MATCHTYPE_DENY_NODESC))
      report_reject = false;

    for(ast_t* child = ast_child(pattern);
      child != NULL;
      child = ast_sibling(child))
    {
      is_x_match_x(operand, child, errorf, report_reject, opt);
    }

    if(ok == MATCHTYPE_DENY_CAP)
    {
      ast_error_frame(errorf, pattern,
        "matching %s with %s could violate capabilities",
        ast_print_type(operand), ast_print_type(pattern));
    } else if(ok == MATCHTYPE_DENY_NODESC) {
      ast_error_frame(errorf, pattern,
        "matching %s with %s is not possible, since a struct lacks a type descriptor",
        ast_print_type(operand), ast_print_type(pattern));
    } else if(report_reject) {
      ast_error_frame(errorf, pattern, "%s cannot match any element of %s",
        ast_print_type(operand), ast_print_type(pattern));
    }
  }

  return ok;
}

static matchtype_t is_x_match_isect(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt)
{
  matchtype_t ok = MATCHTYPE_ACCEPT;

  for(ast_t* child = ast_child(pattern);
    child != NULL;
    child = ast_sibling(child))
  {
    switch(is_x_match_x(operand, child, NULL, false, opt))
    {
      case MATCHTYPE_ACCEPT:
        break;

      case MATCHTYPE_REJECT:
        // If any type in the pattern isect rejects a match, the entire pattern
        // isect rejects a match.
        ok = MATCHTYPE_REJECT;
        break;

      case MATCHTYPE_DENY_CAP:
        // If any type in the pattern isect denies a match, the entire pattern
        // isect denies a match.
        ok = MATCHTYPE_DENY_CAP;
        break;

      case MATCHTYPE_DENY_NODESC:
        ok = MATCHTYPE_DENY_NODESC;
        break;
    }

    if((ok == MATCHTYPE_DENY_CAP) || (ok == MATCHTYPE_DENY_NODESC))
      break;
  }

  if((ok != MATCHTYPE_ACCEPT) && (errorf != NULL))
  {
    if((ok == MATCHTYPE_DENY_CAP) || (ok == MATCHTYPE_DENY_NODESC))
      report_reject = false;

    for(ast_t* child = ast_child(pattern);
      child != NULL;
      child = ast_sibling(child))
    {
      is_x_match_x(operand, child, errorf, report_reject, opt);
    }

    if(ok == MATCHTYPE_DENY_CAP)
    {
      ast_error_frame(errorf, pattern,
        "matching %s with %s could violate capabilities",
        ast_print_type(operand), ast_print_type(pattern));
    } else if(ok == MATCHTYPE_DENY_NODESC) {
      ast_error_frame(errorf, pattern,
        "matching %s with %s is not possible, since a struct lacks a type descriptor",
        ast_print_type(operand), ast_print_type(pattern));
    } else if(report_reject) {
      ast_error_frame(errorf, pattern, "%s cannot match every element of %s",
        ast_print_type(operand), ast_print_type(pattern));
    }
  }

  return ok;
}

static matchtype_t is_tuple_match_tuple(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt)
{
  // Must be a pairwise match.
  if(ast_childcount(operand) != ast_childcount(pattern))
  {
    if((errorf != NULL) && report_reject)
    {
      ast_error_frame(errorf, pattern,
        "%s cannot match %s: they have a different number of elements",
        ast_print_type(operand), ast_print_type(pattern));
    }

    return MATCHTYPE_REJECT;
  }

  ast_t* operand_child = ast_child(operand);
  ast_t* pattern_child = ast_child(pattern);
  matchtype_t ok = MATCHTYPE_ACCEPT;

  while(operand_child != NULL)
  {
    switch(is_x_match_x(operand_child, pattern_child, NULL, false, opt))
    {
      case MATCHTYPE_ACCEPT:
        break;

      case MATCHTYPE_REJECT:
        ok = MATCHTYPE_REJECT;
        break;

      case MATCHTYPE_DENY_CAP:
        ok = MATCHTYPE_DENY_CAP;
        break;

      case MATCHTYPE_DENY_NODESC:
        ok = MATCHTYPE_DENY_NODESC;
        break;
    }

    if(ok != MATCHTYPE_ACCEPT)
      break;

    operand_child = ast_sibling(operand_child);
    pattern_child = ast_sibling(pattern_child);
  }

  if((ok != MATCHTYPE_ACCEPT) && (errorf != NULL))
  {
    if((ok == MATCHTYPE_DENY_CAP) || (ok == MATCHTYPE_DENY_NODESC))
      report_reject = false;

    operand_child = ast_child(operand);
    pattern_child = ast_child(pattern);

    while(operand_child != NULL)
    {
      is_x_match_x(operand_child, pattern_child, errorf, report_reject, opt);

      operand_child = ast_sibling(operand_child);
      pattern_child = ast_sibling(pattern_child);
    }

    if(ok == MATCHTYPE_DENY_CAP)
    {
      ast_error_frame(errorf, pattern,
        "matching %s with %s could violate capabilities",
        ast_print_type(operand), ast_print_type(pattern));
    } else if(ok == MATCHTYPE_DENY_NODESC) {
      ast_error_frame(errorf, pattern,
        "matching %s with %s is not possible, since a struct lacks a type descriptor",
        ast_print_type(operand), ast_print_type(pattern));
    } else if(report_reject) {
      ast_error_frame(errorf, pattern, "%s cannot pairwise match %s",
        ast_print_type(operand), ast_print_type(pattern));
    }
  }

  return ok;
}

static matchtype_t is_nominal_match_tuple(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt)
{
  if(!is_top_type(operand, true))
  {
    if((errorf != NULL) && report_reject)
    {
      ast_t* operand_def = (ast_t*)ast_data(operand);

      ast_error_frame(errorf, pattern,
        "%s cannot match %s: the pattern type is a tuple",
        ast_print_type(operand), ast_print_type(pattern));
      ast_error_frame(errorf, operand_def, "this might be possible if the "
        "match type were an empty interface, such as the Any type");
    }

    return MATCHTYPE_REJECT;
  }

  ast_t* child = ast_child(pattern);

  while(child != NULL)
  {
    matchtype_t r = is_x_match_x(operand, child, errorf, false, opt);
    pony_assert(r != MATCHTYPE_REJECT);

    if(r == MATCHTYPE_DENY_CAP)
    {
      if(errorf != NULL)
      {
        ast_error_frame(errorf, pattern,
          "matching %s with %s could violate capabilities",
          ast_print_type(operand), ast_print_type(pattern));
      }

      return r;
    } else if (r == MATCHTYPE_DENY_NODESC) {
      if(errorf != NULL)
      {
        ast_error_frame(errorf, pattern,
        "matching %s with %s is not possible, since a struct lacks a type descriptor",
        ast_print_type(operand), ast_print_type(pattern));
      }

      return r;
    }

    child = ast_sibling(child);
  }

  return MATCHTYPE_ACCEPT;
}

static matchtype_t is_typeparam_match_typeparam(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt)
{
  (void)opt;
  ast_t* operand_def = (ast_t*)ast_data(operand);
  ast_t* pattern_def = (ast_t*)ast_data(pattern);

  // Dig through defs if there are multiple layers of directly-bound
  // type params (created through the collect_type_params function).
  while((ast_data(operand_def) != NULL) && (operand_def != ast_data(operand_def)))
    operand_def = (ast_t*)ast_data(operand_def);
  while((ast_data(pattern_def) != NULL) && (pattern_def != ast_data(pattern_def)))
    pattern_def = (ast_t*)ast_data(pattern_def);

  ast_t* o_cap = cap_fetch(operand);
  ast_t* o_eph = ast_sibling(o_cap);
  ast_t* p_cap = cap_fetch(pattern);
  ast_t* p_eph = ast_sibling(p_cap);

  matchtype_t r = MATCHTYPE_REJECT;

  if(operand_def == pattern_def)
  {
    r = is_cap_sub_cap_bound(ast_id(o_cap), TK_EPHEMERAL,
      ast_id(p_cap), ast_id(p_eph)) ? MATCHTYPE_ACCEPT : MATCHTYPE_DENY_CAP;
  }

  if((r != MATCHTYPE_ACCEPT) && (errorf != NULL))
  {
    if(r == MATCHTYPE_DENY_CAP)
    {
      ast_error_frame(errorf, pattern,
        "matching %s with %s could violate capabilities: "
        "%s%s isn't a bound subcap of %s%s",
        ast_print_type(operand), ast_print_type(pattern),
        ast_print_type(o_cap), ast_print_type(o_eph),
        ast_print_type(p_cap), ast_print_type(p_eph));
    } else if (r == MATCHTYPE_DENY_NODESC) {
      ast_error_frame(errorf, pattern,
        "matching %s with %s is not possible, since a struct lacks a type descriptor",
        ast_print_type(operand), ast_print_type(pattern));
    } else if(report_reject) {
      ast_error_frame(errorf, pattern,
        "%s cannot match %s: they are different type parameters",
        ast_print_type(operand), ast_print_type(pattern));
    }
  }

  return r;
}

static matchtype_t is_typeparam_match_x(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt)
{
  if(ast_id(pattern) == TK_TYPEPARAMREF)
  {
    matchtype_t ok = is_typeparam_match_typeparam(operand, pattern, errorf,
      false, opt);

    if(ok != MATCHTYPE_REJECT)
      return ok;
  }

  ast_t* operand_upper = typeparam_upper(operand);

  // An unconstrained typeparam could match anything.
  if(operand_upper == NULL)
    return MATCHTYPE_ACCEPT;

  // Check if the constraint can match the pattern.
  matchtype_t ok = is_x_match_x(operand_upper, pattern, errorf, report_reject,
    opt);
  ast_free_unattached(operand_upper);
  return ok;
}

static matchtype_t is_arrow_match_x(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt)
{
  // upperbound(this->T1) match T2
  // ---
  // (this->T1) match T2

  // lowerbound(T1->T2) match T3
  // ---
  // (T1->T2) match T3

  ast_t* operand_view;

  AST_GET_CHILDREN(operand, left, right);

  if(ast_id(left) == TK_THISTYPE)
    operand_view = viewpoint_upper(operand);
  else
    operand_view = viewpoint_lower(operand);

  if(operand_view == NULL)
  {
    if(errorf != NULL)
    {
      // this->X always has an upper bound.
      pony_assert(ast_id(left) != TK_THISTYPE);

      ast_error_frame(errorf, pattern,
        "matching %s with %s could violate capabilities: "
        "the match type has no lower bounds",
        ast_print_type(operand), ast_print_type(pattern));
    }

    return MATCHTYPE_DENY_CAP;
  }

  matchtype_t ok = is_x_match_x(operand_view, pattern, errorf, report_reject,
    opt);
  ast_free_unattached(operand_view);
  return ok;
}

static matchtype_t is_x_match_tuple(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt)
{
  switch(ast_id(operand))
  {
    case TK_UNIONTYPE:
      return is_union_match_x(operand, pattern, errorf, report_reject, opt);

    case TK_ISECTTYPE:
      return is_isect_match_x(operand, pattern, errorf, report_reject, opt);

    case TK_TUPLETYPE:
      return is_tuple_match_tuple(operand, pattern, errorf, report_reject, opt);

    case TK_NOMINAL:
      return is_nominal_match_tuple(operand, pattern, errorf, report_reject,
        opt);

    case TK_TYPEPARAMREF:
      return is_typeparam_match_x(operand, pattern, errorf, report_reject, opt);

    case TK_ARROW:
      return is_arrow_match_x(operand, pattern, errorf, report_reject, opt);

    default: {}
  }

  pony_assert(0);
  return MATCHTYPE_DENY_CAP;
}

static matchtype_t is_nominal_match_entity(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt)
{
  AST_GET_CHILDREN(operand, o_pkg, o_id, o_typeargs, o_cap, o_eph);
  AST_GET_CHILDREN(pattern, p_pkg, p_id, p_typeargs, p_cap, p_eph);

  // We say the pattern provides the operand if it is a subtype without taking
  // capabilities into account.
  bool provides = is_subtype_ignore_cap(pattern, operand, NULL, opt);

  // If the pattern doesn't provide the operand, reject the match.
  if(!provides)
  {
    if((errorf != NULL) && report_reject)
    {
      ast_error_frame(errorf, pattern,
        "%s cannot match %s: %s isn't a subtype of %s",
        ast_print_type(operand), ast_print_type(pattern),
        ast_print_type_no_cap(pattern), ast_print_type_no_cap(operand));
    }

    return MATCHTYPE_REJECT;
  }

  // If the operand does provide the pattern, but the operand refcap can't
  // match the pattern refcap, deny the match.
  if(!is_cap_sub_cap(ast_id(o_cap), ast_id(o_eph),
    ast_id(p_cap), ast_id(p_eph)))
  {
    if(errorf != NULL)
    {
      ast_error_frame(errorf, pattern,
        "matching %s with %s could violate capabilities: "
        "%s%s isn't a subcap of %s%s",
        ast_print_type(operand), ast_print_type(pattern),
        ast_print_type(o_cap), ast_print_type(o_eph),
        ast_print_type(p_cap), ast_print_type(p_eph));

        if(is_cap_sub_cap(ast_id(o_cap), TK_EPHEMERAL, ast_id(p_cap),
          ast_id(p_eph)))
          ast_error_frame(errorf, o_cap,
            "this would be possible if the subcap were more ephemeral. "
            "Perhaps you meant to consume this variable");
  }

    return MATCHTYPE_DENY_CAP;
  }

  // Otherwise, accept the match.
  return MATCHTYPE_ACCEPT;
}

static matchtype_t is_nominal_match_struct(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt)
{
  // A struct pattern can only be used if the operand is the same struct.
  // Otherwise, since there is no type descriptor, there is no way to
  // determine a match at runtime.
  ast_t* operand_def = (ast_t*)ast_data(operand);
  ast_t* pattern_def = (ast_t*)ast_data(pattern);

  // This must be a deny to prevent a union or intersection type that includes
  // the same struct as the pattern from matching.
  if(operand_def != pattern_def)
  {
    if((errorf != NULL) && report_reject)
    {
      ast_error_frame(errorf, pattern,
        "%s cannot match %s: the pattern type is a struct",
        ast_print_type(operand), ast_print_type(pattern));
      ast_error_frame(errorf, pattern,
        "since a struct has no type descriptor, pattern matching at runtime "
        "would be impossible");
    }

    return MATCHTYPE_DENY_NODESC;
  }

  return is_nominal_match_entity(operand, pattern, errorf, report_reject, opt);
}

static matchtype_t is_entity_match_trait(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt)
{
  AST_GET_CHILDREN(operand, o_pkg, o_id, o_typeargs, o_cap, o_eph);
  AST_GET_CHILDREN(pattern, p_pkg, p_id, p_typeargs, p_cap, p_eph);

  bool provides = is_subtype_ignore_cap(operand, pattern, NULL, opt);

  // If the operand doesn't provide the pattern (trait or interface), reject
  // the match.
  if(!provides)
  {
    if((errorf != NULL) && report_reject)
    {
      ast_error_frame(errorf, pattern,
        "%s cannot match %s: %s isn't a subtype of %s",
        ast_print_type(operand), ast_print_type(pattern),
        ast_print_type_no_cap(operand), ast_print_type_no_cap(pattern));
    }

    return MATCHTYPE_REJECT;
  }

  // If the operand does provide the pattern, but the operand refcap can't
  // match the pattern refcap, deny the match.
  if(!is_cap_sub_cap(ast_id(o_cap), TK_EPHEMERAL,
    ast_id(p_cap), ast_id(p_eph)))
  {
    if(errorf != NULL)
    {
      ast_error_frame(errorf, pattern,
        "matching %s with %s could violate capabilities: "
        "%s%s isn't a subcap of %s%s",
        ast_print_type(operand), ast_print_type(pattern),
        ast_print_type(o_cap), ast_print_type(o_eph),
        ast_print_type(p_cap), ast_print_type(p_eph));
    }

    return MATCHTYPE_DENY_CAP;
  }

  // Otherwise, accept the match.
  return MATCHTYPE_ACCEPT;
}

static matchtype_t is_trait_match_trait(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt)
{
  (void)report_reject;
  (void)opt;
  AST_GET_CHILDREN(operand, o_pkg, o_id, o_typeargs, o_cap, o_eph);
  AST_GET_CHILDREN(pattern, p_pkg, p_id, p_typeargs, p_cap, p_eph);

  // If the operand refcap can't match the pattern refcap, deny the match.
  if(!is_cap_sub_cap(ast_id(o_cap), TK_EPHEMERAL,
    ast_id(p_cap), ast_id(p_eph)))
  {
    if(errorf != NULL)
    {
      ast_error_frame(errorf, pattern,
        "matching %s with %s could violate capabilities: "
        "%s%s isn't a subcap of %s%s",
        ast_print_type(operand), ast_print_type(pattern),
        ast_print_type(o_cap), ast_print_type(o_eph),
        ast_print_type(p_cap), ast_print_type(p_eph));
    }

    return MATCHTYPE_DENY_CAP;
  }

  // Otherwise, accept the match.
  return MATCHTYPE_ACCEPT;
}

static matchtype_t is_nominal_match_trait(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt)
{
  ast_t* operand_def = (ast_t*)ast_data(operand);

  switch(ast_id(operand_def))
  {
    case TK_PRIMITIVE:
    case TK_STRUCT:
    case TK_CLASS:
    case TK_ACTOR:
      return is_entity_match_trait(operand, pattern, errorf, report_reject,
        opt);

    case TK_TRAIT:
    case TK_INTERFACE:
      return is_trait_match_trait(operand, pattern, errorf, report_reject,
        opt);

    default: {}
  }

  pony_assert(0);
  return MATCHTYPE_DENY_CAP;
}

static matchtype_t is_nominal_match_nominal(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt)
{
  ast_t* pattern_def = (ast_t*)ast_data(pattern);

  switch(ast_id(pattern_def))
  {
    case TK_PRIMITIVE:
    case TK_CLASS:
    case TK_ACTOR:
      return is_nominal_match_entity(operand, pattern, errorf, report_reject,
        opt);

    case TK_STRUCT:
      return is_nominal_match_struct(operand, pattern, errorf, report_reject,
        opt);

    case TK_TRAIT:
    case TK_INTERFACE:
      return is_nominal_match_trait(operand, pattern, errorf, report_reject,
        opt);

    default: {}
  }

  pony_assert(0);
  return MATCHTYPE_DENY_CAP;
}

static matchtype_t is_tuple_match_nominal(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt)
{
  (void)opt;

  if((errorf != NULL) && report_reject)
  {
    ast_error_frame(errorf, pattern,
      "%s cannot match %s: the match type is a tuple",
      ast_print_type(operand), ast_print_type(pattern));
  }

  return MATCHTYPE_REJECT;
}

static matchtype_t is_x_match_nominal(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt)
{
  switch(ast_id(operand))
  {
    case TK_UNIONTYPE:
      return is_union_match_x(operand, pattern, errorf, report_reject, opt);

    case TK_ISECTTYPE:
      return is_isect_match_x(operand, pattern, errorf, report_reject, opt);

    case TK_TUPLETYPE:
      return is_tuple_match_nominal(operand, pattern, errorf, report_reject,
        opt);

    case TK_NOMINAL:
      return is_nominal_match_nominal(operand, pattern, errorf, report_reject,
        opt);

    case TK_TYPEPARAMREF:
      return is_typeparam_match_x(operand, pattern, errorf, report_reject, opt);

    case TK_ARROW:
      return is_arrow_match_x(operand, pattern, errorf, report_reject, opt);

    case TK_FUNTYPE:
      return MATCHTYPE_REJECT;

    default: {}
  }

  pony_assert(0);
  return MATCHTYPE_DENY_CAP;
}

static matchtype_t is_x_match_base_typeparam(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt)
{
  switch(ast_id(operand))
  {
    case TK_UNIONTYPE:
      return is_union_match_x(operand, pattern, errorf, report_reject, opt);

    case TK_ISECTTYPE:
      return is_isect_match_x(operand, pattern, errorf, report_reject, opt);

    case TK_TUPLETYPE:
    case TK_NOMINAL:
      return MATCHTYPE_REJECT;

    case TK_TYPEPARAMREF:
      return is_typeparam_match_typeparam(operand, pattern, errorf, false, opt);

    case TK_ARROW:
      return is_arrow_match_x(operand, pattern, errorf, report_reject, opt);

    case TK_FUNTYPE:
      return MATCHTYPE_REJECT;

    default: {}
  }

  pony_assert(0);
  return MATCHTYPE_DENY_CAP;
}

static matchtype_t is_x_match_typeparam(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt)
{
  matchtype_t ok = is_x_match_base_typeparam(operand, pattern, errorf,
    report_reject, opt);

  if(ok != MATCHTYPE_REJECT)
    return ok;

  ast_t* pattern_upper = typeparam_upper(pattern);

  // An unconstrained typeparam can match anything.
  if(pattern_upper == NULL)
    return MATCHTYPE_ACCEPT;

  // Otherwise, match the constraint.
  ok = is_x_match_x(operand, pattern_upper, errorf, report_reject, opt);
  ast_free_unattached(pattern_upper);
  return ok;
}

static matchtype_t is_x_match_arrow(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt)
{
  // T1 match upperbound(T2->T3)
  // ---
  // T1 match T2->T3
  ast_t* pattern_upper = viewpoint_upper(pattern);

  if(pattern_upper == NULL)
  {
    if((errorf != NULL) && report_reject)
    {
      ast_error_frame(errorf, pattern,
        "%s cannot match %s: the pattern type has no upper bounds",
        ast_print_type(operand), ast_print_type(pattern));
    }

    return MATCHTYPE_REJECT;
  }

  matchtype_t ok = is_x_match_x(operand, pattern_upper, errorf, report_reject,
    opt);
  ast_free_unattached(pattern_upper);
  return ok;
}

static matchtype_t is_x_match_x(ast_t* operand, ast_t* pattern,
  errorframe_t* errorf, bool report_reject, pass_opt_t* opt)
{
  if(ast_id(pattern) == TK_DONTCARETYPE)
    return MATCHTYPE_ACCEPT;

  switch(ast_id(pattern))
  {
    case TK_UNIONTYPE:
      return is_x_match_union(operand, pattern, errorf, report_reject, opt);

    case TK_ISECTTYPE:
      return is_x_match_isect(operand, pattern, errorf, report_reject, opt);

    case TK_TUPLETYPE:
      return is_x_match_tuple(operand, pattern, errorf, report_reject, opt);

    case TK_NOMINAL:
      return is_x_match_nominal(operand, pattern, errorf, report_reject, opt);

    case TK_TYPEPARAMREF:
      return is_x_match_typeparam(operand, pattern, errorf, report_reject, opt);

    case TK_ARROW:
      return is_x_match_arrow(operand, pattern, errorf, report_reject, opt);

    case TK_FUNTYPE:
      return MATCHTYPE_DENY_CAP;

    default: {}
  }

  pony_assert(0);
  return MATCHTYPE_DENY_CAP;
}

matchtype_t is_matchtype(ast_t* operand, ast_t* pattern, errorframe_t* errorf,
  pass_opt_t* opt)
{
  ast_t* consumed_pattern = consume_type(pattern, TK_NONE, false);
  if (consumed_pattern == NULL)
    return MATCHTYPE_REJECT;

  matchtype_t rslt = is_x_match_x(operand, consumed_pattern, errorf, true, opt);

  // TODO discuss with joe
  if (consumed_pattern != pattern)
    ast_free_unattached(consumed_pattern);

  return rslt;
}

matchtype_t is_astype(ast_t* operand, ast_t* pattern, errorframe_t* errorf,
  pass_opt_t* opt)
{
  return is_x_match_x(operand, pattern, errorf, true, opt);
}
