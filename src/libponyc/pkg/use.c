#include "use.h"
#include "../ast/ast.h"
#include "package.h"
#include "ifdef.h"
#include "program.h"
#include "../pass/pass.h"
#include "../pass/scope.h"
#include "../../libponyrt/mem/pool.h"
#include "ponyassert.h"
#include <string.h>


/** Use commands are of the form:
 *   use "[scheme:]locator" [as name] [if guard]
 *
 * Each scheme has a handler function. The first handler ("package:") is used
 * as the default for commands that do not specify a scheme.
 *
 * An extra scheme handler for "test:" can be setup for testing.
 */


typedef struct use_scheme_t
{
  const char* scheme; // Interned textual identifier including :
  size_t scheme_len;  // Number of characters in scheme, including :
  bool allow_name;    // Is the name clause allowed
  bool allow_guard;   // Is the guard expression allowed
  use_handler_t handler;
} use_scheme_t;

static __pony_thread_local use_scheme_t handlers[] =
{
  {"package:", 8, true, false, use_package},
  {"lib:", 4, false, true, use_library},
  {"path:", 5, false, true, use_path},
  {"llvm:", 5, false, true, use_llvm},

  {"test:", 5, false, false, NULL},  // For testing
  {NULL, 0, false, false, NULL}  // Terminator
};


void use_test_handler(use_handler_t handler, bool allow_alias,
  bool allow_guard)
{
  for(int i = 0; handlers[i].scheme != NULL; i++)
  {
    if(strcmp(handlers[i].scheme, "test:") == 0)
    {
      handlers[i].allow_name = allow_alias;
      handlers[i].allow_guard = allow_guard;
      handlers[i].handler = handler;
      return;
    }
  }
}


// Find the index of the handler for the given URI
static int find_handler(pass_opt_t* opt, ast_t* uri, const char** out_locator)
{
  pony_assert(uri != NULL);
  pony_assert(out_locator != NULL);
  pony_assert(ast_id(uri) == TK_STRING);

  const char* text = ast_name(uri);
  const char* colon = strchr(text, ':');

  if(colon == NULL)
  {
    // No scheme specified, use default
    *out_locator = stringtab(text);
    return 0;
  }

  size_t scheme_len = colon - text + 1;  // +1 for colon

  // Search for matching handler
  for(int i = 0; handlers[i].scheme != NULL; i++)
  {
    if(handlers[i].scheme_len == scheme_len &&
      strncmp(handlers[i].scheme, text, scheme_len) == 0)
    {
      if(handlers[i].handler == NULL) // No handler provided (probably test:)
        break;

      // Matching scheme found
      *out_locator = stringtab(colon + 1);
      return i;
    }
  }

  // No match found
#ifdef PLATFORM_IS_WINDOWS
  if(colon == text + 1)
  {
    // Special case error message
    ast_error(opt->check.errors, uri, "Use scheme %c: not found. "
      "If this is an absolute path use prefix \"package:\"", text[0]);
    return -1;
  }
#endif

  ast_error(opt->check.errors, uri, "Use scheme %.*s not found",
    (int)scheme_len, text);
  return -1;
}


// Handle a uri command
static bool uri_command(pass_opt_t* opt, ast_t* ast, ast_t* uri, ast_t* alias,
  ast_t* guard)
{
  pony_assert(uri != NULL);
  pony_assert(alias != NULL);
  pony_assert(guard != NULL);

  const char* locator;
  int index = find_handler(opt, uri, &locator);

  if(index < 0) // Scheme not found
    return false;

  if(ast_id(alias) != TK_NONE && !handlers[index].allow_name)
  {
    ast_error(opt->check.errors, alias, "Use scheme %s may not have an alias",
      handlers[index].scheme);
    return false;
  }

  if(ast_id(guard) != TK_NONE && !handlers[index].allow_guard)
  {
    ast_error(opt->check.errors, alias, "Use scheme %s may not have a guard",
      handlers[index].scheme);
    return false;
  }

  if(ifdef_cond_normalise(&guard, opt) && !ifdef_cond_eval(guard, opt))
    return true;

  pony_assert(handlers[index].handler != NULL);
  return handlers[index].handler(ast, locator, alias, opt);
}


// Handle an ffi command
static bool ffi_command(pass_opt_t* opt, ast_t* alias)
{
  pony_assert(alias != NULL);

  if(ast_id(alias) != TK_NONE)
  {
    ast_error(opt->check.errors, alias, "Use FFI may not have an alias");
    return false;
  }

  return true;
}


ast_result_t use_command(ast_t* ast, pass_opt_t* options)
{
  pony_assert(ast != NULL);
  pony_assert(options != NULL);

  AST_GET_CHILDREN(ast, alias, spec, guard);
  pony_assert(spec != NULL);
  pony_assert(guard != NULL);

  switch(ast_id(spec))
  {
  case TK_STRING:
    if(!uri_command(options, ast, spec, alias, guard))
      return AST_ERROR;

    break;

  case TK_FFIDECL:
    if(!ffi_command(options, alias))
      return AST_ERROR;

    break;

  default:
    pony_assert(0);
    return AST_ERROR;
  }

  return AST_OK;
}
