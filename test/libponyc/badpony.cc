#include <gtest/gtest.h>
#include <platform.h>

#include "util.h"


/** Pony code that parses, but is erroneous. Typically type check errors and
 * things used in invalid contexts.
 *
 * We build all the way up to and including code gen and check that we do not
 * assert, segfault, etc but that the build fails and at least one error is
 * reported.
 *
 * There is definite potential for overlap with other tests but this is also a
 * suitable location for tests which don't obviously belong anywhere else.
 */

#define TEST_COMPILE(src) DO(test_compile(src, "ir"))

#define TEST_ERRORS_1(src, err1) \
  { const char* errs[] = {err1, NULL}; \
    DO(test_expected_errors(src, "ir", errs)); }

#define TEST_ERRORS_2(src, err1, err2) \
  { const char* errs[] = {err1, err2, NULL}; \
    DO(test_expected_errors(src, "ir", errs)); }

#define TEST_ERRORS_3(src, err1, err2, err3) \
  { const char* errs[] = {err1, err2, err3, NULL}; \
    DO(test_expected_errors(src, "ir", errs)); }


class BadPonyTest : public PassTest
{};


// Cases from reported issues

TEST_F(BadPonyTest, ClassInOtherClassProvidesList)
{
  // From issue #218
  const char* src =
    "class Named\n"
    "class Dog is Named\n"

    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    None";

  TEST_ERRORS_1(src, "invalid provides type. Can only be interfaces, traits and intersects of those.");
}

TEST_F(BadPonyTest, TypeParamMissingForTypeInProvidesList)
{
  // From issue #219
  const char* src =
    "trait Bar[A]\n"
    "  fun bar(a: A) =>\n"
    "    None\n"

    "trait Foo is Bar // here also should be a type argument, like Bar[U8]\n"
    "  fun foo() =>\n"
    "    None\n"

    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    None";

  TEST_ERRORS_1(src, "not enough type arguments");
}

TEST_F(BadPonyTest, TupleIndexIsZero)
{
  // From issue #397
  const char* src =
    "primitive Foo\n"
    "  fun bar(): None =>\n"
    "    (None, None)._0";

  TEST_ERRORS_1(src, "Did you mean _1?");
}

TEST_F(BadPonyTest, TupleIndexIsOutOfRange)
{
  // From issue #397
  const char* src =
    "primitive Foo\n"
    "  fun bar(): None =>\n"
    "    (None, None)._3";

  TEST_ERRORS_1(src, "Valid range is [1, 2]");
}

TEST_F(BadPonyTest, InvalidLambdaReturnType)
{
  // From issue #828
  const char* src =
    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    {(): tag => this }\n";

  TEST_ERRORS_1(src, "lambda return type: tag");
}

TEST_F(BadPonyTest, InvalidMethodReturnType)
{
  // From issue #828
  const char* src =
    "primitive Foo\n"
    "  fun bar(): iso =>\n"
    "    U32(1)\n";

  TEST_ERRORS_1(src, "function return type: iso");
}

TEST_F(BadPonyTest, TypeErrorInFFIArguments)
{
  // From issue #2114
  const char *src =
    "use @foo[None](i: Pointer[U32])\n"
    "actor Main\n"
    "  fun create(env: Env) =>\n"
    "    @foo(addressof U32(0))";

  TEST_ERRORS_1(src, "can only take the address of a local, field or method");
}

TEST_F(BadPonyTest, ObjectLiteralUninitializedField)
{
  // From issue #879
  const char* src =
    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    object\n"
    "      let x: I32\n"
    "    end";

  TEST_ERRORS_1(src, "object literal fields must be initialized");
}

TEST_F(BadPonyTest, LambdaCaptureVariableBeforeDeclarationWithTypeInferenceExpressionFail)
{
  // From issue #1018
  const char* src =
    "class Foo\n"
    "  fun f() =>\n"
    "    {()(x) => None }\n"
    "    let x = 0";

   TEST_ERRORS_1(src, "declaration of 'x' appears after use");
}

// TODO: This test is not correct because it does not fail without the fix.
// I do not know how to generate a test that calls genheader().
// Comments are welcomed.
/*TEST_F(BadPonyTest, ExportedActorWithVariadicReturnTypeContainingNone)
{
  // From issue #891
  const char* src =
    "primitive T\n"
    "\n"
    "actor @A\n"
    "  fun f(a: T): (T | None) =>\n"
    "    a\n";

  TEST_COMPILE(src);
}*/

TEST_F(BadPonyTest, TypeAliasRecursionThroughTypeParameterInTuple)
{
  // From issue #901
  const char* src =
    "type Foo is (Map[Foo, Foo], None)\n"
    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    None";

  TEST_ERRORS_1(src, "type aliases can't be recursive");
}

TEST_F(BadPonyTest, ParenthesisedReturn)
{
  // From issue #1050
  const char* src =
    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    (return)";

  TEST_ERRORS_1(src, "use return only to exit early from a method");
}

TEST_F(BadPonyTest, ParenthesisedReturn2)
{
  // From issue #1050
  const char* src =
    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    foo()\n"

    "  fun foo(): U64 =>\n"
    "    (return 0)\n"
    "    2";

  TEST_ERRORS_1(src, "Unreachable code");
}

TEST_F(BadPonyTest, MatchUncalledMethod)
{
  // From issue #903
  const char* src =
    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    match foo\n"
    "    | None => None\n"
    "    end\n"

    "  fun foo() =>\n"
    "    None";

  TEST_ERRORS_2(src, "can't reference a method without calling it",
                     "this pattern can never match");
}

TEST_F(BadPonyTest, TupleFieldReassign)
{
  // From issue #1101
  const char* src =
    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    var foo: (U64, String) = (42, \"foo\")\n"
    "    foo._2 = \"bar\"";

  TEST_ERRORS_1(src, "can't assign to an element of a tuple");
}

TEST_F(BadPonyTest, WithBlockTypeInference)
{
  // From issue #1135
  const char* src =
    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    with x = 1 do None end";

  TEST_ERRORS_3(src, "could not infer literal type, no valid types found",
                     "cannot infer type of $1$0",
                     "cannot infer type of x");
}

TEST_F(BadPonyTest, EmbedNestedTuple)
{
  // From issue #1136
  const char* src =
    "class Foo\n"
    "  fun get_foo(): Foo => Foo\n"

    "actor Main\n"
    "  embed foo: Foo\n"
    "  let x: U64\n"

    "  new create(env: Env) =>\n"
    "    (foo, x) = (Foo.get_foo(), 42)";

  TEST_ERRORS_1(src, "an embedded field must be assigned using a constructor");
}

TEST_F(BadPonyTest, CircularTypeInfer)
{
  // From issue #1334
  const char* src =
    "actor Main\n"
      "new create(env: Env) =>\n"
        "let x = x.create()\n"
        "let y = y.create()";

  TEST_ERRORS_2(src,
    "can't use an undefined variable in an expression",
    "can't use an undefined variable in an expression");
}

TEST_F(BadPonyTest, CallConstructorOnTypeIntersection)
{
  // From issue #1398
  const char* src =
    "interface Foo\n"

    "type Isect is (None & Foo)\n"

    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    Isect.create()";

  TEST_ERRORS_1(src, "can't call a constructor on a type intersection");
}

TEST_F(BadPonyTest, AssignToFieldOfIso)
{
  // From issue #1469
  const char* src =
    "class Foo\n"
    "  var x: String ref = String\n"
    "  fun iso bar(): String iso^ =>\n"
    "    let s = recover String end\n"
    "    x = s\n"
    "   consume s\n"

    "  fun ref foo(): String iso^ =>\n"
    "    let s = recover String end\n"
    "    let y: Foo iso = Foo\n"
    "    y.x = s\n"
    "    consume s";

  TEST_ERRORS_2(src,
    "right side must be a subtype of left side",
    "right side must be a subtype of left side"
    );
}

TEST_F(BadPonyTest, IndexArrayWithBrackets)
{
  // From issue #1493
  const char* src =
    "actor Main\n"
      "new create(env: Env) =>\n"
        "let xs = [as I64: 1; 2; 3]\n"
        "xs[1]";

  TEST_ERRORS_1(src, "Value formal parameters not yet supported");
}

TEST_F(BadPonyTest, ShadowingBuiltinTypeParameter)
{
  const char* src =
    "class A[I8]\n"
      "let b: U8 = 0";

  TEST_ERRORS_1(src, "type parameter shadows existing type");
}

TEST_F(BadPonyTest, ShadowingTypeParameterInSameFile)
{
  const char* src =
    "trait B\n"
    "class A[B]";

  TEST_ERRORS_1(src, "can't reuse name 'B'");
}

TEST_F(BadPonyTest, TupleToUnionGentrace)
{
  // From issue #1561
  const char* src =
    "primitive X\n"
    "primitive Y\n"

    "class iso T\n"

    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    this((T, Y))\n"

    "  be apply(m: (X | (T, Y))) => None";

  TEST_COMPILE(src);
}

TEST_F(BadPonyTest, RefCapViolationViaCapReadTypeParameter)
{
  // From issue #1328
  const char* src =
    "class Foo\n"
      "var i: USize = 0\n"
      "fun ref boom() => i = 3\n"

    "actor Main\n"
      "new create(env: Env) =>\n"
        "let a: Foo val = Foo\n"
        "call_boom[Foo val](a)\n"

      "fun call_boom[A: Foo #read](x: A) =>\n"
        "x.boom()";

  TEST_ERRORS_1(src, "receiver type is not a subtype of target type");
}

TEST_F(BadPonyTest, RefCapViolationViaCapAnyTypeParameter)
{
  // From issue #1328
  const char* src =
    "class Foo\n"
      "var i: USize = 0\n"
      "fun ref boom() => i = 3\n"

    "actor Main\n"
      "new create(env: Env) =>\n"
        "let a: Foo val = Foo\n"
        "call_boom[Foo val](a)\n"

      "fun call_boom[A: Foo #any](x: A) =>\n"
        "x.boom()";

  TEST_ERRORS_1(src, "receiver type is not a subtype of target type");
}

TEST_F(BadPonyTest, AliasAny)
{
  // Safeguard ensuring #alias <= #any not allowed by
  // is_cap_sub_cap_bound, related to PR #3643
  const char* src =
    "class Foo\n"
    "actor Main\n"
      "new create(env: Env) =>\n"
        "let a: Foo val = Foo\n"
        "alias_bound[Foo val](a)\n"

      "fun alias_bound[A](x: A) =>\n"
        "let x': A = x";

  TEST_ERRORS_1(src, "right side must be a subtype of left side");
}

TEST_F(BadPonyTest, ReturnUnmovedIso)
{
  // Issue #1964
  const char* src =
    "class Bar\n"
    "class Foo\n"
      "let x: Bar iso = Bar\n"
      "fun get_bad(): Bar val =>\n"
        "x\n"

    "actor Main\n"
      "new create(env: Env) =>\n"
        "None\n";

  TEST_ERRORS_1(src, "function body isn't the result type");
}

TEST_F(BadPonyTest, TypeParamArrowClass)
{
  // From issue #1687
  const char* src =
    "class C1\n"

    "trait Test[A]\n"
    "  fun foo(a: A): A->C1\n"

    "actor Main\n"
    "  new create(env: Env) => None";

  TEST_COMPILE(src);
}

TEST_F(BadPonyTest, ArrowTypeParamInTypeConstraint)
{
  // From issue #1694
  const char* src =
    "trait T1[A: B->A, B]\n"
    "trait T2[A: box->B, B]";

  TEST_ERRORS_2(src,
    "arrow types can't be used as type constraints",
    "arrow types can't be used as type constraints");
}

TEST_F(BadPonyTest, ArrowTypeParamInMethodConstraint)
{
  // From issue #1809
  const char* src =
    "class Foo\n"
    "  fun foo[X: box->Y, Y](x: X) => None";

  TEST_ERRORS_1(src,
    "arrow types can't be used as type constraints");
}

TEST_F(BadPonyTest, AnnotatedIfClause)
{
  // From issue #1751
  const char* src =
    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    if \\likely\\ U32(1) == 1 then\n"
    "      None\n"
    "    end\n";

  TEST_COMPILE(src);
}

TEST_F(BadPonyTest, CapSubtypeInConstrainSubtyping)
{
  // From PR #1816
  const char* src =
    "trait T\n"
    "  fun alias[X: Any iso](x: X!): X^\n"
    "class C is T\n"
    "  fun alias[X: Any tag](x: X!): X^ => x\n";

  TEST_ERRORS_1(src,
    "type does not implement its provides list");
}

TEST_F(BadPonyTest, ObjectInheritsLaterTraitMethodWithParameter)
{
  // From issue #1715
  const char* src =
    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    object is T end\n"

    "trait T\n"
    "  fun apply(n: I32): Bool =>\n"
    "    n == 0\n";

  TEST_COMPILE(src);
}

TEST_F(BadPonyTest, AddressofMissingTypearg)
{
  const char* src =
    "use @foo[None](fn: Pointer[None] tag)\n"

    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    @foo(addressof fn)\n"

    "  fun fn[A]() => None";

  TEST_ERRORS_1(src,
    "not enough type arguments");
}

TEST_F(BadPonyTest, ThisDotFieldRef)
{
  // From issue #1865
  const char* src =
    "actor Main\n"
    "  let f: U8\n"
    "  new create(env: Env) =>\n"
    "    this.f = 1\n";

  TEST_COMPILE(src);
}

TEST_F(BadPonyTest, CapSetInConstraintTypeParam)
{
  const char* src =
    "class A[X]\n"
    "class B[X: A[Any #read]]\n";

  TEST_ERRORS_1(src,
    "a capability set can only appear in a type constraint");
}

TEST_F(BadPonyTest, MatchCasePatternConstructorTooFewArguments)
{
  const char* src =
    "class C\n"
    "  new create(key: String) => None\n"

    "primitive Foo\n"
    "  fun apply(c: (C | None)) =>\n"
    "    match c\n"
    "    | C => None\n"
    "    end";

  TEST_ERRORS_1(src, "not enough arguments");
}

TEST_F(BadPonyTest, ThisDotWhereDefIsntInTheTrait)
{
  // From issue #1878
  const char* src =
    "trait T\n"
    "  fun foo(): USize => this.u\n"

    "class C is T\n"
    "  var u: USize = 0\n";

  TEST_ERRORS_1(src,
    "can't find declaration of 'u'");
}

TEST_F(BadPonyTest, DontCareTypeInTupleTypeOfIfBlockValueUnused)
{
  // From issue #1896
  const char* src =
    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    if true then\n"
    "      (var a, let _) = test()\n"
    "    end\n"
    "  fun test(): (U32, U32) =>\n"
    "    (1, 2)\n";

  TEST_COMPILE(src);
}

TEST_F(BadPonyTest, ExhaustiveMatchCasesJumpAway)
{
  // From issue #1898
  const char* src =
    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    if true then\n"
    "      match env\n"
    "      | let env': Env => return\n"
    "      end\n"
    "    end";

  TEST_COMPILE(src);
}

TEST_F(BadPonyTest, CallArgTypeErrorInsideTuple)
{
  // From issue #1895
  const char* src =
    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    (\"\", foo([\"\"]))\n"
    "  fun foo(x: Array[USize]) => None";

  TEST_ERRORS_1(src, "array element not a subtype of specified array type");
}

TEST_F(BadPonyTest, NonExistFieldReferenceInConstructor)
{
  // From issue #1932
  const char* src =
    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    this.x = None";

  TEST_ERRORS_2(src,
    "can't find declaration of 'x'",
    "left side must be something that can be assigned to");
}

TEST_F(BadPonyTest, TypeArgErrorInsideReturn)
{
  const char* src =
    "primitive P[A]\n"

    "primitive Foo\n"
    "  fun apply(): (P[None], U8) =>\n"
    "    if true then\n"
    "      return (P, 0)\n"
    "    end\n"
    "    (P[None], 1)";

  TEST_ERRORS_1(src, "not enough type arguments");
}

TEST_F(BadPonyTest, FieldReferenceInDefaultArgument)
{
  const char* src =
    "actor Main\n"
    "  let _env: Env\n"
    "  new create(env: Env) =>\n"
    "    _env = env\n"
    "    foo()\n"
    "  fun foo(env: Env = _env) =>\n"
    "    None";

  TEST_ERRORS_1(src, "can't reference 'this' in a default argument");
}

TEST_F(BadPonyTest, DefaultArgScope)
{
  const char* src =
    "actor A\n"
    "  fun foo(x: None = (let y = None; y)) =>\n"
    "    y";

  TEST_ERRORS_1(src, "can't find declaration of 'y'");
}

TEST_F(BadPonyTest, GenericMain)
{
  const char* src =
    "actor Main[X]\n"
    "  new create(env: Env) => None";

  TEST_ERRORS_1(src, "the Main actor cannot have type parameters");
}

TEST_F(BadPonyTest, LambdaParamNoType)
{
  // From issue #2010
  const char* src =
    "actor Main\n"
    "  new create(e: Env) =>\n"
    "    {(x: USize, y): USize => x }";

  TEST_ERRORS_1(src, "a lambda parameter must specify a type");
}

TEST_F(BadPonyTest, AsBadCap)
{
  const char* src =
    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    let v: I64 = 3\n"
    "    try (v as I64 iso) end";

  TEST_ERRORS_1(src, "this capture violates capabilities");
}

TEST_F(BadPonyTest, AsUnaliased)
{
  const char* src =
    "class trn Foo\n"

    "actor Main\n"
    "  let foo: (Foo|None) = Foo\n"

    "  new create(env: Env) => None\n"

    "  fun ref apply() ? =>\n"
    "    (foo as Foo trn)";

  TEST_COMPILE(src);
}

TEST_F(BadPonyTest, CodegenMangledFunptr)
{
  // Test that we don't crash in codegen when generating the function pointer.
  const char* src =
    "use @foo[None](fn: Pointer[None] tag)\n"

    "interface I\n"
    "  fun foo(x: U32)\n"

    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    let i: I = this\n"
    "    @foo(addressof i.foo)\n"

    "  fun foo(x: Any) => None";

  TEST_COMPILE(src);
}

TEST_F(BadPonyTest, AsFromUninferredReference)
{
  // From issue #2035
  const char* src =
    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    let b = apply(true)\n"
    "    b as Bool\n"

    "  fun ref apply(s: String): (Bool | None) =>\n"
    "    true";

  TEST_ERRORS_2(src,
    "argument not assignable to parameter",
    "cannot infer type of b");
}

TEST_F(BadPonyTest, FFIDeclaredTupleArgument)
{
  const char* src =
    "use @foo[None](x: (U8, U8))\n"

    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    @foo((0, 0))";

  TEST_ERRORS_1(src, "cannot pass tuples as FFI arguments");
}

TEST_F(BadPonyTest, FFIDeclaredTupleReturn)
{
  const char* src =
    "use @foo[(U8, U8)]()\n"

    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    @foo()";

  TEST_ERRORS_1(src, "an FFI function cannot return a tuple");
}

TEST_F(BadPonyTest, FFICallInDefaultInterfaceFun)
{
  const char* src =
    "use @foo[None]()\n"

    "interface Foo\n"
    "  fun apply() =>\n"
    "    @foo()\n"

    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    None";

  TEST_ERRORS_1(src,
    "Can't call an FFI function in a default method or behavior");
}

TEST_F(BadPonyTest, FFICallInDefaultInterfaceBe)
{
  const char* src =
    "use @foo[None]()\n"

    "interface Foo\n"
    "  be apply() =>\n"
    "    @foo()\n"

    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    None";

  TEST_ERRORS_1(src,
    "Can't call an FFI function in a default method or behavior");
}

TEST_F(BadPonyTest, FFICallInDefaultTraitFun)
{
  const char* src =
    "use @foo[None]()\n"

    "trait Foo\n"
    "  fun apply() =>\n"
    "    @foo()\n"

    "actor Main is Foo\n"
    "  new create(env: Env) =>\n"
    "    None";

  TEST_ERRORS_1(src,
    "Can't call an FFI function in a default method or behavior");
}

TEST_F(BadPonyTest, FFICallInDefaultTraitBe)
{
  const char* src =
    "use @foo[None]()\n"

    "trait Foo\n"
    "  be apply() =>\n"
    "    @foo()\n"

    "actor Main is Foo\n"
    "  new create(env: Env) =>\n"
    "    None";

  TEST_ERRORS_1(src,
    "Can't call an FFI function in a default method or behavior");
}

TEST_F(BadPonyTest, FFIDeclaredTupleReturnAtCallSite)
{
  const char* src =
    "use @foo[None]()\n"

    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    @foo[(U8, U8)]()";

  TEST_ERRORS_1(src, "an FFI function cannot return a tuple");
}

TEST_F(BadPonyTest, MatchExhaustiveLastCaseUnionSubset)
{
  // From issue #2048
  const char* src =
    "primitive P1\n"
    "primitive P2\n"

    "actor Main\n"
    "  new create(env: Env) => apply(None)\n"

    "  fun apply(p': (P1 | P2 | None)) =>\n"
    "    match p'\n"
    "    | None => None\n"
    "    | let p: (P1 | P2) => None\n"
    "    end";

  TEST_COMPILE(src);
}

TEST_F(BadPonyTest, QuoteCommentsEmptyLine)
{
  // From issue #2027
  const char* src =
    "\"\"\"\n"
    "\"\"\"\n"
    "actor Main\n"
    "  new create(env: Env) => None\n";

  TEST_COMPILE(src);
}

TEST_F(BadPonyTest, AsUninferredNumericLiteral)
{
  // From issue #2037
  const char* src =
    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    0 as I64\n"
    "    0.0 as F64";

  TEST_ERRORS_2(src,
    "Cannot cast uninferred numeric literal",
    "Cannot cast uninferred numeric literal");
}

TEST_F(BadPonyTest, AsNestedUninferredNumericLiteral)
{
  // From issue #2037
  const char* src =
    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    (0, 1) as (I64, I64)";

  TEST_ERRORS_1(src, "Cannot cast uninferred literal");
}

TEST_F(BadPonyTest, DontCareUnusedBranchValue)
{
  // From issue #2073
  const char* src =
    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    if false then\n"
    "      None\n"
    "    else\n"
    "      _\n"
    "    end";

  TEST_COMPILE(src);
}

TEST_F(BadPonyTest, ForwardTuple)
{
  // From issue #2097
  const char* src =
    "class val X\n"

    "trait T\n"
    "  fun f(): Any val\n"

    "class C is T\n"
    "  fun f(): (X, USize) => (X, 0)\n"

    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    let t: T = C\n"
    "    t.f()";

  TEST_COMPILE(src);
}

TEST_F(BadPonyTest, CoerceUninferredNumericLiteralThroughTypeArgWithViewpoint)
{
  // From issue #2181
  const char* src =
    "class A[T]\n"
    "  new create(x: T) => None\n"

    "primitive B[T]\n"
    "  fun apply(): A[T] =>\n"
    "    A[this->T](1)\n";

  TEST_ERRORS_1(src, "could not infer literal type, no valid types found");
}

TEST_F(BadPonyTest, UnionTypeMethodsWithDifferentParamNamesNamedParams)
{
  // From issue #394
  // disallow calling a method on a union with named arguments
  // when the parameter names differ
  const char* src =
    "primitive A\n"
    "  fun foo(a: U64, b: U64): U64 => a\n"
    "  fun bar(x: U64, y: U64): U64 => x\n\n"

    "primitive B\n"
    "  fun foo(b: U64, c: U64): U64 => b\n"
    "  fun bar(x: U64, y: U64): U64 => x\n\n"

    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    let aba = add_ab(A)\n"
    "    let abb = add_ab(B)\n"
    "    let bca = add_bc(A)\n"
    "    let bcb = add_bc(B)\n"
    "    let xya = add_xy(A)\n"
    "    let xyb = add_xy(B)\n\n"

    "  fun add_ab(uni: (A | B)): U64 =>\n"
    "    uni.foo(where a = 80, b = 8)\n\n"

    "  fun add_bc(uni: (A | B)): U64 =>\n"
    "    uni.foo(where b = 80, c = 8)\n\n"

    "  fun add_xy(uni: (A | B)): U64 =>\n"
    "    uni.bar(where x = 80, y = 8)\n\n";

    TEST_ERRORS_2(src,
      "methods of this union type have different parameter names",
      "methods of this union type have different parameter names");
}

TEST_F(BadPonyTest, UnionTypeMethodsWithDifferentParamNamesPositional)
{
  // From issue #394
  // allow calling a method on a union with positional arguments
  // when the parameter names differ
  const char* src =
  "primitive A\n"
  "  fun foo(a: U64, b: U64): U64 => a\n"
  "  fun bar(x: U64, y: U64): U64 => x\n\n"

  "primitive B\n"
  "  fun foo(b: U64, c: U64): U64 => b\n"
  "  fun bar(x: U64, y: U64): U64 => x\n\n"

  "actor Main\n"
  "  new create(env: Env) =>\n"
  "    let aba = add_ab(A)\n"
  "    let abb = add_ab(B)\n"
  "    let bca = add_bc(A)\n"
  "    let bcb = add_bc(B)\n"
  "    let xya = add_xy(A)\n"
  "    let xyb = add_xy(B)\n\n"

  "  fun add_ab(uni: (A | B)): U64 =>\n"
  "    uni.foo(80, 8)\n\n"

  "  fun add_bc(uni: (A | B)): U64 =>\n"
  "    uni.foo(80, 8)\n\n"

  "  fun add_xy(uni: (A | B)): U64 =>\n"
  "    uni.bar(80, 8)\n\n";

  TEST_COMPILE(src);
}

TEST_F(BadPonyTest, ApplySugarInferredLambdaArgument)
{
  // From issue #2233
  const char* src =
    "primitive Callable\n"
    "  fun apply(x: U64, fn: {(U64): U64} val): U64 => fn(x)\n"

    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    let callable = Callable\n"
    "    callable(0, {(x) => x + 1 })";

  TEST_COMPILE(src);
}

TEST_F(BadPonyTest, IsComparingCreateSugar)
{
  // From issue #2024
  const char* src =
    "primitive P\n"
    "class C\n"
    "actor A\n"

    "actor Main\n"
    "  new create(env: Env) =>\n"
    // invalid
    "    C is C\n"
    "    A is A\n"
    "    C is P\n"
    "    P is C\n"
    "    P is (P, C, P)\n"
    "    P is (P; C)\n"
    "    P is (P, ((C), P))\n"
    "    C isnt C\n"
    "    C isnt P\n"
    "    P isnt C\n"
    "    P isnt (P, C, P)\n"
    "    P isnt (P; C)\n"
    "    P isnt (P, ((C), P))\n"
    // valid
    "    P is P\n"
    "    P is (C; P)\n"
    "    P isnt P\n"
    "    P isnt (C; P)";
  {
    const char* err = "identity comparison with a new object will always be false";
    const char* errs[] = {err, err, err, err, err, err, err, err, err, err, err, err, err, NULL};
    DO(test_expected_errors(src, "verify", errs));
  }
}

TEST_F(BadPonyTest, IsComparingCreate)
{
  // From issue #4162, this is just the desugared
  // version of the previous test.
  const char* src =
    "primitive P\n"
    "class C\n"
    "actor A\n"

    "actor Main\n"
    "  new create(env: Env) =>\n"
    // invalid
    "    C.create() is C.create()\n"
    "    A.create() is A.create()\n"
    "    C.create() is P.create()\n"
    "    P.create() is C.create()\n"
    "    P.create() is (P.create(), C.create(), P.create())\n"
    "    P.create() is (P.create(); C.create())\n"
    "    P.create() is (P.create(), ((C.create()), P.create()))\n"
    "    C.create() isnt C.create()\n"
    "    C.create() isnt P.create()\n"
    "    P.create() isnt C.create()\n"
    "    P.create() isnt (P.create(), C.create(), P.create())\n"
    "    P.create() isnt (P.create(); C.create())\n"
    "    P.create() isnt (P.create(), ((C.create()), P.create()))\n"
    // valid
    "    P.create() is P.create()\n"
    "    P.create() is (C.create(); P.create())\n"
    "    P.create() isnt P.create()\n"
    "    P.create() isnt (C.create(); P.create())";
  {
    const char* err = "identity comparison with a new object will always be false";
    const char* errs[] = {err, err, err, err, err, err, err, err, err, err, err, err, err, NULL};
    DO(test_expected_errors(src, "verify", errs));
  }
}

TEST_F(BadPonyTest, IsComparingNamedConstructor)
{
  // From issue #4162, this is about testing named
  // constructors, and not just the default one.
  const char* src =
    "primitive P\n"
    "\n"
    "class C\n"
    "  new create() => None\n"
    "  new testing() => None\n"
    "\n"
    "actor A\n"
    "  new create() => None\n"
    "  new testing() => None\n"

    "actor Main\n"
    "  new create(env: Env) =>\n"
    // invalid
    "    C.create() is C.testing()\n"
    "    A.create() is A.testing()\n"
    "    C.create() is P.create()\n"
    "    P.create() is C.testing()\n"
    "    P.create() is (P.create(), C.testing(), P.create())\n"
    "    P.create() is (P.create(); C.testing())\n"
    "    P.create() is (P.create(), ((C.testing()), P.create()))\n"
    "    C.create() isnt C.testing()\n"
    "    C.create() isnt P.create()\n"
    "    P.create() isnt C.testing()\n"
    "    P.create() isnt (P.create(), C.testing(), P.create())\n"
    "    P.create() isnt (P.create(); C.testing())\n"
    "    P.create() isnt (P.create(), ((C.testing()), P.create()))\n"
    // valid
    "    P.create() is P.create()\n"
    "    P.create() is (C.testing(); P.create())\n"
    "    P.create() isnt P.create()\n"
    "    P.create() isnt (C.testing(); P.create())";
  {
    const char* err = "identity comparison with a new object will always be false";
    const char* errs[] = {err, err, err, err, err, err, err, err, err, err, err, err, err, NULL};
    DO(test_expected_errors(src, "verify", errs));
  }
}

TEST_F(BadPonyTest, TypeErrorDuringArrayLiteralInference)
{
  // From issue 2602
  const char* src =
    "class C\n"
    "trait X\n"
    "class ExpectX[T: X]\n"
    "  fun trigger(arg: Iterator[T]) =>\n"
    "    None\n"

    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    ExpectX[C].trigger([C; C].values())";
  TEST_ERRORS_1(src, "type argument is outside its constraint");
}

TEST_F(BadPonyTest, NosupertypeAnnotationProvides)
{
  const char* src =
    "trait T\n"

    "primitive \\nosupertype\\ P is T";

  TEST_ERRORS_1(src, "a 'nosupertype' type cannot specify a provides list");
}

TEST_F(BadPonyTest, ThisViewpointWithIsoReceiver)
{
  // From issue #1887
  const char* src =
    "class A\n"
    "class Revealer\n"
    "  fun box reveal(x: this->A ref): A box => x\n"

    "actor Main\n"
    "new create(env: Env) =>\n"
    "  let revealer : Revealer iso = Revealer.create()\n"
    "  let opaque : A tag = A.create()\n"
    "  let not_opaque : A box = (consume revealer).reveal(opaque)\n";

  TEST_ERRORS_1(src, "argument not assignable to parameter");
}

TEST_F(BadPonyTest, DisallowPointerAndMaybePointerInEmbeddedType)
{
  // From issue #2596
  const char* src =
    "struct Ok\n"

    "class Whoops\n"
    "  embed ok: Ok = Ok\n"
    "  embed not_ok: Pointer[None] = Pointer[None]\n"
    "  embed also_not_ok: NullablePointer[Ok] = NullablePointer[Ok](Ok)\n"

    "actor Main\n"
    "new create(env: Env) =>\n"
    "  Whoops";

  TEST_ERRORS_2(src,
    "embedded fields must be classes or structs",
    "embedded fields must be classes or structs")
}

TEST_F(BadPonyTest, AllowAliasForNonEphemeralReturn)
{
  const char* src =
    "class iso Inner\n"
    "  new iso create() => None\n"

    "class Container[A: Inner #any]\n"
    "  var inner: A\n"
    "  new create(inner': A) => inner = consume inner'\n"
    "  fun get_1(): this->A => inner                        // works\n"
    "  fun get_2(): this->A => let tmp = inner; consume tmp // also works\n"

    "actor Main\n"
    "  new create(env: Env) =>\n"
    "    let o = Container[Inner iso](Inner)\n"
    "    let i_1 : Inner tag = o.get_1()\n"
    "    let i_2 : Inner tag = o.get_2()";

  TEST_COMPILE(src);
}

TEST_F(BadPonyTest, AllowNestedTupleAccess)
{
  // From issue #3354
  const char* src =
    "actor Main\n"
    "  new create(env: Env) =>\n"
    "        let x = (1, (2, 3))._2._1";

  TEST_ERRORS_1(src,
    "Cannot look up member _2 on a literal")
}

TEST_F(BadPonyTest, InterfacesCantHavePrivateMethods)
{
  // From issue #2287
  const char* src =
    "interface Foo\n"
    "  fun ref _set(i: USize, value: U8): U8";

  TEST_ERRORS_1(src,
    "interfaces can't have private methods, only traits can")
}

TEST_F(BadPonyTest, CantAssignErrorExpression)
{
  // From issue #3823
  const char* src =
    "actor Main\n"
      "new create(env: Env) =>\n"
        "let x: String = (error)";

  TEST_ERRORS_1(src,
    "right side must be something that can be assigned")
}

TEST_F(BadPonyTest, NotSafeToWrite)
{
  // From issue #4290
  const char* src =
    "class Foo\n"
    "class X\n"
      "var foo: Foo ref = Foo\n"

    "actor Main\n"
      "new create(env: Env) =>\n"
        "let x = X\n"
        "let foo: Foo ref = Foo\n"
        "x.foo = foo";

  {
      const char* errs[] =
        {"not safe to write right side to left side", NULL};
      const char* frame1[] = {
        "right side type: Foo ref^",
        "left side type: X iso", NULL};
      const char** frames[] = {frame1, NULL};
      DO(test_expected_error_frames(src, "badpony", errs, frames));
  }
}

TEST_F(BadPonyTest, MatchIsoFieldWithoutConsume)
{
  // From issue #4579
  const char* src =
    "class Bad\n"
    "  var _s: String iso\n"

    "  new iso create(s: String iso) =>\n"
    "    _s = consume s\n"

    "  fun ref take(): String iso^ =>\n"
    "    match _s\n"
    "    | let s': String iso => consume s'\n"
    "    end";

    TEST_ERRORS_1(src, "this capture violates capabilities");
}

TEST_F(BadPonyTest, MatchIsoLetWithoutConsume)
{
  // From issue #4579
  const char* src =
    "class Bad\n"
    "  fun bad() =>\n"
    "    let a: String iso = recover iso String end\n"

    "    match a\n"
    "    | let a': String iso => None\n"
    "    end";

    TEST_ERRORS_1(src, "this capture violates capabilities");
}
