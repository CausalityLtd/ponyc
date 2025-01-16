#include "names.h"
#include "../ast/astbuild.h"
#include "../ast/id.h"
#include "../type/reify.h"
#include "../pkg/package.h"
#include "ponyassert.h"

static bool names_applycap(pass_opt_t* opt, ast_t* ast, ast_t* cap,
  ast_t* ephemeral)
{
  switch(ast_id(ast))
  {
    case TK_UNIONTYPE:
    case TK_ISECTTYPE:
    case TK_TUPLETYPE:
    {
      // Apply the capability and ephemerality to each element of the type
      // expression.
      for(ast_t* child = ast_child(ast);
        child != NULL;
        child = ast_sibling(child))
      {
        names_applycap(opt, child, cap, ephemeral);
      }

      return true;
    }

    case TK_TYPEPARAMREF:
    {
      AST_GET_CHILDREN(ast, id, tcap, teph);

      if(ast_id(cap) != TK_NONE)
      {
        ast_error(opt->check.errors, cap,
          "can't specify a capability for an alias to a type parameter");
        return false;
      }

      if(ast_id(ephemeral) != TK_NONE)
        ast_replace(&teph, ephemeral);

      return true;
    }

    case TK_NOMINAL:
    {
      AST_GET_CHILDREN(ast, pkg, id, typeargs, tcap, teph);

      if(ast_id(cap) != TK_NONE)
        ast_replace(&tcap, cap);

      if(ast_id(ephemeral) != TK_NONE)
        ast_replace(&teph, ephemeral);

      return true;
    }

    case TK_ARROW:
      return names_applycap(opt, ast_childidx(ast, 1), cap, ephemeral);

    default: {}
  }

  pony_assert(0);
  return false;
}

static bool names_resolvealias(pass_opt_t* opt, ast_t* def, ast_t** type)
{
  int state = ast_checkflag(def,
    AST_FLAG_RECURSE_1 | AST_FLAG_DONE_1 | AST_FLAG_ERROR_1);

  switch(state)
  {
    case 0:
      ast_setflag(def, AST_FLAG_RECURSE_1);
      break;

    case AST_FLAG_RECURSE_1:
      ast_error(opt->check.errors, def, "type aliases can't be recursive");
      ast_clearflag(def, AST_FLAG_RECURSE_1);
      ast_setflag(def, AST_FLAG_ERROR_1);
      return false;

    case AST_FLAG_DONE_1:
      return true;

    case AST_FLAG_ERROR_1:
      return false;

    default:
      pony_assert(0);
      return false;
  }

  if(ast_visit_scope(type, NULL, pass_names, opt,
    PASS_NAME_RESOLUTION) != AST_OK)
    return false;

  ast_clearflag(def, AST_FLAG_RECURSE_1);
  ast_setflag(def, AST_FLAG_DONE_1);
  return true;
}

static bool names_typeargs(pass_opt_t* opt, ast_t* typeargs)
{
  for(ast_t* typearg = ast_child(typeargs);
    typearg != NULL;
    typearg = ast_sibling(typearg))
  {
    if(ast_visit_scope(&typearg, NULL, pass_names, opt,
      PASS_NAME_RESOLUTION) != AST_OK)
      return false;
  }

  return true;
}

static bool names_typealias(pass_opt_t* opt, ast_t** astp, ast_t* def,
  bool expr)
{
  ast_t* ast = *astp;
  AST_GET_CHILDREN(ast, pkg, id, typeargs, cap, eph);

  // Make sure the alias is resolved,
  AST_GET_CHILDREN(def, alias_id, typeparams, def_cap, provides);
  ast_t* alias = ast_child(provides);

  if(!names_resolvealias(opt, def, &alias))
    return false;

  if(!reify_defaults(typeparams, typeargs, true, opt))
    return false;

  if(!names_typeargs(opt, typeargs))
    return false;

  if(expr)
  {
    if(!check_constraints(typeargs, typeparams, typeargs, true, opt))
      return false;
  }

  // Reify the alias.
  ast_t* r_alias = reify(alias, typeparams, typeargs, opt, true);

  if(r_alias == NULL)
    return false;

  // Apply our cap and ephemeral to the result.
  if(!names_applycap(opt, r_alias, cap, eph))
  {
    ast_free_unattached(r_alias);
    return false;
  }

  // Maintain the position info of the original reference to aid error
  // reporting.
  ast_setpos(r_alias, ast_source(ast), ast_line(ast), ast_pos(ast));

  // Replace this with the alias.
  ast_replace(astp, r_alias);

  if(!expr)
    ast_resetpass(*astp, PASS_NAME_RESOLUTION);

  return true;
}

static bool names_typeparam(pass_opt_t* opt, ast_t** astp, ast_t* def)
{
  ast_t* ast = *astp;
  AST_GET_CHILDREN(ast, package, id, typeargs, cap, ephemeral);
  pony_assert(ast_id(package) == TK_NONE);

  if(ast_id(typeargs) != TK_NONE)
  {
    ast_error(opt->check.errors, typeargs,
      "can't qualify a type parameter with type arguments");
    return false;
  }

  if(ast_id(ast) == TK_NOMINAL) {
    ast_t* module = ast_nearest(ast, TK_MODULE);
    if(module && ast_get(module, ast_name(id), 0)) {
      ast_error(opt->check.errors, def,
                "type parameter shadows existing type");
      return false;
    }
  }

  const char* name = ast_name(id);

  // Change to a typeparamref.
  REPLACE(astp,
    NODE(TK_TYPEPARAMREF,
      TREE(id)
      TREE(cap)
      TREE(ephemeral)));

  ast = *astp;

  if(opt->check.frame->iftype_body != NULL)
    def = ast_get(ast, name, NULL);

  ast_setdata(*astp, def);
  return true;
}

static bool names_type(pass_opt_t* opt, ast_t** astp, ast_t* def)
{
  ast_t* ast = *astp;
  AST_GET_CHILDREN(ast, package, id, typeargs, cap, eph);
  AST_GET_CHILDREN(def, def_id, typeparams, def_cap);
  token_id tcap = ast_id(cap);

  if(tcap == TK_NONE)
  {
    // Use the default capability.
    tcap = ast_id(def_cap);
  }

  ast_setid(cap, tcap);

  // We keep the actual package id for printing out in errors, etc.
  ast_append(ast, package);
  ast_replace(&package, package_id(def));

  // Store our definition for later use.
  ast_setdata(ast, def);

  if(!reify_defaults(typeparams, typeargs, true, opt))
    return false;

  if(!names_typeargs(opt, typeargs))
    return false;

  return true;
}

static ast_t* get_package_scope(pass_opt_t* opt, ast_t* scope, ast_t* ast)
{
  pony_assert(ast_id(ast) == TK_NOMINAL);
  ast_t* package_id = ast_child(ast);

  // Find our actual package.
  if(ast_id(package_id) != TK_NONE)
  {
    const char* name = ast_name(package_id);

    if(is_name_internal_test(name))
      scope = ast_get(ast_nearest(scope, TK_PROGRAM), name, NULL);
    else
      scope = ast_get(scope, name, NULL);

    if((scope == NULL) || (ast_id(scope) != TK_PACKAGE))
    {
      ast_error(opt->check.errors, package_id, "can't find package '%s'", name);
      return NULL;
    }
  }

  return scope;
}

ast_t* names_def(pass_opt_t* opt, ast_t* ast)
{
  ast_t* def = (ast_t*)ast_data(ast);

  if(def != NULL)
    return def;

  AST_GET_CHILDREN(ast, package_id, type_id, typeparams, cap, eph);
  ast_t* scope = get_package_scope(opt, ast, ast);

  return ast_get(scope, ast_name(type_id), NULL);
}

bool names_nominal(pass_opt_t* opt, ast_t* scope, ast_t** astp, bool expr)
{
  ast_t* ast = *astp;

  if(ast_data(ast) != NULL)
    return true;

  AST_GET_CHILDREN(ast, package_id, type_id, typeparams, cap, eph);

  // Keep some stats.
  opt->check.stats.names_count++;

  if(ast_id(cap) == TK_NONE)
    opt->check.stats.default_caps_count++;

  ast_t* r_scope = get_package_scope(opt, scope, ast);

  if(r_scope == NULL)
    return false;

  bool local_package =
    (r_scope == scope) || (r_scope == ast_nearest(ast, TK_PACKAGE));

  // Find our definition.
  const char* name = ast_name(type_id);

  if(is_name_dontcare(name))
  {
    ast_error(opt->check.errors, type_id,
      "can't use '_' as a type name");
    return false;
  }

  ast_t* def = ast_get(r_scope, name, NULL);
  if(def == NULL)
    def = ast_get_provided_symbol_definition(ast, name, NULL);

  bool r = true;

  if(def == NULL)
  {
    ast_error(opt->check.errors, type_id,
      "can't find definition of '%s'", name);
    r = false;
  } else {
    // Check for a private type.
    if(!local_package && is_name_private(name))
    {
      ast_error(opt->check.errors, type_id,
        "can't access a private type from another package");
      r = false;
    }

    switch(ast_id(def))
    {
      case TK_TYPE:
        r = names_typealias(opt, astp, def, expr);
        break;

      case TK_TYPEPARAM:
        r = names_typeparam(opt, astp, def);
        break;

      case TK_INTERFACE:
      case TK_TRAIT:
      case TK_PRIMITIVE:
      case TK_STRUCT:
      case TK_CLASS:
      case TK_ACTOR:
        r = names_type(opt, astp, def);
        break;

      default:
        ast_error(opt->check.errors, type_id,
          "definition of '%s' is not a type", name);
        r = false;
        break;
    }
  }

  return r;
}

ast_result_t pass_names(ast_t** astp, pass_opt_t* options)
{
  (void)options;

  switch(ast_id(*astp))
  {
    case TK_NOMINAL:
      if(!names_nominal(options, *astp, astp, false))
        return AST_FATAL;
      break;

    default: {}
  }

  return AST_OK;
}
