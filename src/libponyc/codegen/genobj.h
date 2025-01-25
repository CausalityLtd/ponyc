#ifndef CODEGEN_GENOBJ_H
#define CODEGEN_GENOBJ_H

#include <platform.h>
#include "codegen.h"

PONY_EXTERN_C_BEGIN

const char* genobj(compile_t* c, ast_t* program);

PONY_EXTERN_C_END

#endif
