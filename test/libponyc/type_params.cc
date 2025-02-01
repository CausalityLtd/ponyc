#include <gtest/gtest.h>
#include <platform.h>

#include <ast/ast.h>

#include "util.h"

#define TEST_COMPILE(src) DO(test_compile(src, "expr"))
#define TEST_ERROR(src) DO(test_error(src, "expr"))
#define TEST_EQUIV(src, expect) DO(test_equiv(src, "expr", expect, "expr"))

class TypeParamsTest : public PassTest
{};

TEST_F(TypeParamsTest, ReifySimultaneously)
{
  // From issue #1870
  const char* src =
    "interface State[S, I, O]\n"
    " fun val apply(state: S, input: I): (S, O)\n"
    " fun val bind[O2](next: State[S, O, O2]): State[S, I, O2]\n";

  TEST_COMPILE(src);
}
