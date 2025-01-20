#ifndef MATCHTYPE_H
#define MATCHTYPE_H

#include <platform.h>
#include "../ast/ast.h"
#include "../pass/pass.h"

PONY_EXTERN_C_BEGIN

/// See comment for is_matchtype() for a description of these values
typedef enum
{
  MATCHTYPE_ACCEPT,
  MATCHTYPE_REJECT,
  MATCHTYPE_DENY_CAP,
  MATCHTYPE_DENY_NODESC
} matchtype_t;

/**
 * Determine if we can match on operand_type and extract a pattern_type.
 *
 * Return ACCEPT if there exists a type which is all of:
 * 1. A subtype of operand_type.
 * 2. A type that an object of type operand_type can be safely used as. This is
 *    an important restriction to maintain capability safety. If operand_type
 *    is "Foo box" then "Foo ref" is a subtype of that, but not one that we can
 *    safely use.
 *    This complication exists because capabilities cannot be recovered at
 *    runtime and must be handled entirely at compile time.
 * 3. A subtype of pattern_type.
 *
 * Return DENY_CAP if no such type can exist, but one could if capabilities were
 * ignored. For example an operand_type of "Foo box" and a pattern_type of
 * "Foo ref". This is to prevent a match that could be detected with runtime
 * information but would actually violate the capability guarantees.
 * When DENY_CAP is returned, no matching is allowed, even if some other
 * is_matchtype() relationship exists.
 *
 * Return DENY_NODESC if no such type can exist, due to either type lacking
 * a type descriptor, like a struct.
 *
 * Return REJECT if no such type can exist.
 */
matchtype_t is_matchtype(ast_t* operand, ast_t* pattern, errorframe_t* errorf,
  pass_opt_t* opt);

/**
 * Same as is_matchtype() but specifically designed to work with `as` which
 * under the steed model, needs to act differently than matchwith respect
 * to iso.
 */
matchtype_t is_astype(ast_t* operand, ast_t* pattern, errorframe_t* errorf,
  pass_opt_t* opt);

PONY_EXTERN_C_END

#endif
