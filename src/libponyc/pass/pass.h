#ifndef PASS_H
#define PASS_H

#include <platform.h>
#include "../ast/ast.h"
#include "../ast/frame.h"
#include "../ast/source.h"


PONY_EXTERN_C_BEGIN


/** Passes

Throughout, the global AST is routed at TK_PROGRAM.
The symbol table at TK_PROGRAM and dataref at TK_PACKAGE are populated.


1. Parse

Turns a source file in an AST. Deliberately allows some illegal syntax to
enable better error reporting.

Expects Pony source file or string.

Adds AST routed at TK_MODULE to global tree.
Dataref at TK_MODULE is populated.


2. Parse fix

Checks for specific illegal syntax cases that the BNF allows. This allows for
better error reporting.

Expects AST subtree routed at TK_MODULE with type aliases unresolved and symbol
tables not yet populated.

Only modifies the AST by removing temporary nodes left in by the parser purely
to enable checking in this pass. If this passes then the AST is fully
syntactically correct.


3. Sugar

Expands an AST tree to put in the code we've let the programmer miss out. This
includes default capabilities, method return values, else blocks, "create" and
"apply" names. There are also some code rewrites, such as assignment to update
call and for loop to while loop. Note that some further sugar may be required
but cannot be performed until type checking occurs.

Expects AST subtree routed at TK_MODULE with type aliases unresolved and symbol
tables not yet populated.

Produces an AST with all defaults, etc filled in.

*/


typedef enum pass_id
{
  PASS_PARSE,
  PASS_SYNTAX,
  PASS_SUGAR,
  PASS_SCOPE,
  PASS_IMPORT,
  PASS_NAME_RESOLUTION,
  PASS_FLATTEN,
  PASS_TRAITS,
  PASS_EXPR,
  PASS_AST,
  PASS_LLVM_IR,
  PASS_BITCODE,
  PASS_ASM,
  PASS_OBJ,
  PASS_ALL
} pass_id;

/** Pass options.
 */
typedef struct pass_opt_t
{
  pass_id limit;
  bool release;
  bool library;
  bool ieee_math;
  bool no_restrict;
  bool print_stats;
  bool verify;
  bool strip_debug;
  const char* output;

  char* triple;
  char* cpu;
  char* features;

  typecheck_t check;
} pass_opt_t;

/** Limit processing to the specified pass. All passes up to and including the
 * specified pass will occur.
 * Returns true on success, false on invalid pass name.
 */
bool limit_passes(pass_opt_t* opt, const char* pass);

/** Report the name of the specified pass.
 * The returned string is a literal and should not be freed.
 */
const char* pass_name(pass_id pass);

/** Initialise pass options.
 */
void pass_opt_init(pass_opt_t* options);

/** Finish with pass options.
 */
void pass_opt_done(pass_opt_t* options);

/** Apply the per package passes to the given type definition AST.
 * This is used only for anonymous types.
 */
bool type_passes(ast_t* type, pass_opt_t* options);

/** Apply the per module passes to the given source.
 * Returns true on success, false on failure.
 * The given source is attached to the resulting AST on success and closed on
 * failure.
 */
bool module_passes(ast_t* package, pass_opt_t* options, source_t* source);

/** Apply the per package passes to the given package AST.
 * Returns true on success, false on failure.
 */
bool package_passes(ast_t* package, pass_opt_t* options);

/** Apply the per program passes to the given AST.
 * Returns true on success, false on failure.
 */
bool program_passes(ast_t* program, pass_opt_t* options);

/** Perform the code generation passes based on the given AST.
 * Returns true on success, false on failure.
 */
bool generate_passes(ast_t* program, pass_opt_t* options);


PONY_EXTERN_C_END


#ifdef PLATFORM_IS_VISUAL_STUDIO
inline pass_id& operator++(pass_id& current)
{
  current = static_cast<pass_id>(current + 1);
  return current;
};

inline pass_id operator++(pass_id& current, int)
{
  pass_id rvalue = current;
  ++current;
  return rvalue;
};
#endif

#endif
