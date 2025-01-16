#ifndef PROGRAM_H
#define PROGRAM_H

#include <platform.h>
#include "../ast/ast.h"
#include "../pass/pass.h"

PONY_EXTERN_C_BEGIN

typedef struct program_t program_t;

/// Create a program_t to add to a TK_PROGRAM AST node.
program_t* program_create();

/// Free the given program_t.
void program_free(program_t* program);

/// Assign the next package ID from the program at the root of the given AST.
/// The actual AST node passed in may be anywhere in the tree.
uint32_t program_assign_pkg_id(ast_t* ast);


/// Process a "lib:" scheme use command.
bool use_library(ast_t* use, const char* locator, ast_t* name,
  pass_opt_t* options);

/// Process an "llvm:" scheme use command.
bool use_llvm(ast_t* use, const char* locator, ast_t* name,
  pass_opt_t* options);

/// Process a "path:" scheme use command.
bool use_path(ast_t* use, const char* locator, ast_t* name,
  pass_opt_t* options);

/** Assert that the provided ast_t* and the required field
 * llvm_irs are not NULL before returning the field
 */
strlist_t* program_llvm_irs(ast_t* program);

/** Build the required linker arguments based on the libraries we're using.
 * Once this has been called no more calls to use_library() are permitted.
 */
void program_lib_build_args(ast_t* program, pass_opt_t* opt,
  const char* path_preamble, const char* rpath_preamble,
  const char* global_preamble, const char* global_postamble,
  const char* lib_premable, const char* lib_postamble);

/** Report the command line options for the user specified libraries, if any.
 * May only be called after program_lib_build_args().
 * The returned string is valid for the lifetime of the associated AST and
 * should not be freed by the caller.
 */
const char* program_lib_args(ast_t* program);

const char* program_signature(ast_t* program);

void program_dump(ast_t* program);

pony_type_t* program_pony_type();

PONY_EXTERN_C_END

#endif
