"""
# Pony Standard Library

This package represents the test suite for the Pony standard library.

For every new package, please add a Main actor and tests to the package in a
file called 'test.pony'. Then add a corresponding use directive and a line to
the main actor constructor of this package to invoke those tests.

All tests can be run by compiling and running packages/stdlib.
 """

use "ponytest"
use capsicum = "capsicum" if freebsd or linux
use collections = "collections"
use base64 = "encode/base64"
use files = "files"
use json = "json"
use math = "math"
use net = "net"
use http = "net/http"
use ssl = "net/ssl"
use options = "options"
use random = "random"
use promises = "promises"
use regex = "regex"
use term = "term"
use time = "time"


actor Main is TestList
  new create(env: Env) => PonyTest(env, this)
  new make() => None

  fun tag tests(test: PonyTest) =>
    // Tests for the builtin package.
    test(_TestStringRunes)
    test(_TestIntToString)
    test(_TestStringToU8)
    test(_TestStringToI8)
    test(_TestStringToIntLarge)
    test(_TestStringLstrip)
    test(_TestStringRstrip)
    test(_TestStringStrip)
    test(_TestStringRemove)
    test(_TestStringReplace)
    test(_TestStringSplit)
    test(_TestStringCompare)
    test(_TestSpecialValuesF32)
    test(_TestSpecialValuesF64)

    // Tests for all other packages.
    collections.Main.make().tests(test)
    base64.Main.make().tests(test)
    net.Main.make().tests(test)
    http.Main.make().tests(test)
    options.Main.make().tests(test)
    json.Main.make().tests(test)


class _TestStringRunes iso is UnitTest
  """
  Test iterating over the unicode codepoints in a string.
  """
  fun name(): String => "builtin/String.runes"

  fun apply(h: TestHelper): TestResult ? =>
    let s = "\u16ddx\ufb04"
    let expect = [as U32: 0x16dd, 'x', 0xfb04]
    let result = Array[U32]

    for c in s.runes() do
      result.push(c)
    end

    h.expect_eq[U64](expect.size(), result.size())

    for i in collections.Range(0, expect.size()) do
      h.expect_eq[U32](expect(i), result(i))
    end

    true


class _TestIntToString iso is UnitTest
  """
  Test converting integers to strings.
  """
  fun name(): String => "builtin/U32.string"

  fun apply(h: TestHelper): TestResult =>
    h.expect_eq[String]("0", U32(0).string())
    h.expect_eq[String]("3", U32(3).string())
    h.expect_eq[String]("1234", U32(1234).string())
    h.expect_eq[String]("003", U32(3).string(where prec = 3))
    h.expect_eq[String]("  3", U32(3).string(where width = 3))
    h.expect_eq[String]("  003", U32(3).string(where prec = 3, width = 5))

    true


class _TestStringToU8 iso is UnitTest
  """
  Test converting strings to U8s.
  """
  fun name(): String => "builtin/String.u8"

  fun apply(h: TestHelper): TestResult ? =>
    h.expect_eq[U8](0, "0".u8())
    h.expect_eq[U8](123, "123".u8())
    h.expect_eq[U8](123, "0123".u8())
    h.expect_eq[U8](89, "089".u8())

    h.expect_error(lambda()? => "300".u8() end, "U8 300")
    h.expect_error(lambda()? => "30L".u8() end, "U8 30L")
    h.expect_error(lambda()? => "-10".u8() end, "U8 -10")

    h.expect_eq[U8](16, "0x10".u8())
    h.expect_eq[U8](31, "0x1F".u8())
    h.expect_eq[U8](31, "0x1f".u8())
    h.expect_eq[U8](31, "0X1F".u8())
    h.expect_eq[U8](2, "0b10".u8())
    h.expect_eq[U8](2, "0B10".u8())
    h.expect_eq[U8](0x8A, "0b1000_1010".u8())

    h.expect_error(lambda()? => "1F".u8() end, "U8 1F")
    h.expect_error(lambda()? => "0x".u8() end, "U8 0x")
    h.expect_error(lambda()? => "0b3".u8() end, "U8 0b3")
    h.expect_error(lambda()? => "0d4".u8() end, "U8 0d4")

    true


class _TestStringToI8 iso is UnitTest
  """
  Test converting strings to I8s.
  """
  fun name(): String => "builtin/String.i8"

  fun apply(h: TestHelper): TestResult ? =>
    h.expect_eq[I8](0, "0".i8())
    h.expect_eq[I8](123, "123".i8())
    h.expect_eq[I8](123, "0123".i8())
    h.expect_eq[I8](89, "089".i8())
    h.expect_eq[I8](-10, "-10".i8())

    h.expect_error(lambda()? => "200".i8() end, "I8 200")
    h.expect_error(lambda()? => "30L".i8() end, "I8 30L")

    h.expect_eq[I8](16, "0x10".i8())
    h.expect_eq[I8](31, "0x1F".i8())
    h.expect_eq[I8](31, "0x1f".i8())
    h.expect_eq[I8](31, "0X1F".i8())
    h.expect_eq[I8](2, "0b10".i8())
    h.expect_eq[I8](2, "0B10".i8())
    h.expect_eq[I8](0x4A, "0b100_1010".i8())
    h.expect_eq[I8](-0x4A, "-0b100_1010".i8())

    h.expect_error(lambda()? => "1F".i8() end, "U8 1F")
    h.expect_error(lambda()? => "0x".i8() end, "U8 0x")
    h.expect_error(lambda()? => "0b3".i8() end, "U8 0b3")
    h.expect_error(lambda()? => "0d4".i8() end, "U8 0d4")

    true


class _TestStringToIntLarge iso is UnitTest
  """
  Test converting strings to I* and U* types bigger than 8 bit.
  """
  fun name(): String => "builtin/String.toint"

  fun apply(h: TestHelper): TestResult ? =>
    h.expect_eq[U16](0, "0".u16())
    h.expect_eq[U16](123, "123".u16())
    h.expect_error(lambda()? => "-10".u16() end, "U16 -10")
    h.expect_error(lambda()? => "65536".u16() end, "U16 65536")
    h.expect_error(lambda()? => "30L".u16() end, "U16 30L")

    h.expect_eq[I16](0, "0".i16())
    h.expect_eq[I16](123, "123".i16())
    h.expect_eq[I16](-10, "-10".i16())
    h.expect_error(lambda()? => "65536".i16() end, "I16 65536")
    h.expect_error(lambda()? => "30L".i16() end, "I16 30L")

    h.expect_eq[U32](0, "0".u32())
    h.expect_eq[U32](123, "123".u32())
    h.expect_error(lambda()? => "-10".u32() end, "U32 -10")
    h.expect_error(lambda()? => "30L".u32() end, "U32 30L")

    h.expect_eq[I32](0, "0".i32())
    h.expect_eq[I32](123, "123".i32())
    h.expect_eq[I32](-10, "-10".i32())
    h.expect_error(lambda()? => "30L".i32() end, "I32 30L")

    h.expect_eq[U64](0, "0".u64())
    h.expect_eq[U64](123, "123".u64())
    h.expect_error(lambda()? => "-10".u64() end, "U64 -10")
    h.expect_error(lambda()? => "30L".u64() end, "U64 30L")

    h.expect_eq[I64](0, "0".i64())
    h.expect_eq[I64](123, "123".i64())
    h.expect_eq[I64](-10, "-10".i64())
    h.expect_error(lambda()? => "30L".i64() end, "I64 30L")

    h.expect_eq[U128](0, "0".u128())
    h.expect_eq[U128](123, "123".u128())
    h.expect_error(lambda()? => "-10".u128() end, "U128 -10")
    h.expect_error(lambda()? => "30L".u128() end, "U128 30L")

    h.expect_eq[I128](0, "0".i128())
    h.expect_eq[I128](123, "123".i128())
    h.expect_eq[I128](-10, "-10".i128())
    h.expect_error(lambda()? => "30L".i128() end, "I128 30L")

    true

class _TestStringLstrip iso is UnitTest
  """
  Test stripping leading characters from a string.
  """
  fun name(): String => "builtin/String.lstrip"

  fun apply(h: TestHelper): TestResult =>
    h.expect_eq[String](recover "foobar".clone().lstrip("foo") end, "bar")
    h.expect_eq[String](recover "fooooooobar".clone().lstrip("foo") end, "bar")
    h.expect_eq[String](recover "   foobar".clone().lstrip() end, "foobar")
    h.expect_eq[String](recover "  foobar  ".clone().lstrip() end, "foobar  ")

    true

class _TestStringRstrip iso is UnitTest
  """
  Test stripping trailing characters from a string.
  """
  fun name(): String => "builtin/String.rstrip"

  fun apply(h: TestHelper): TestResult =>
    h.expect_eq[String](recover "foobar".clone().rstrip("bar") end, "foo")
    h.expect_eq[String](recover "foobaaaaaar".clone().rstrip("bar") end, "foo")
    h.expect_eq[String](recover "foobar  ".clone().rstrip() end, "foobar")
    h.expect_eq[String](recover "  foobar  ".clone().rstrip() end, "  foobar")

    true

class _TestStringStrip iso is UnitTest
  """
  Test stripping leading and trailing characters from a string.
  """
  fun name(): String => "builtin/String.strip"

  fun apply(h: TestHelper): TestResult =>
    h.expect_eq[String](recover "  foobar  ".clone().strip() end, "foobar")
    h.expect_eq[String](recover "barfoobar".clone().strip("bar") end, "foo")
    h.expect_eq[String](recover "foobarfoo".clone().strip("foo") end, "bar")
    h.expect_eq[String](recover "foobarfoo".clone().strip("bar") end,
      "foobarfoo")

    true

class _TestStringRemove iso is UnitTest
  """
  Test removing characters from a string (independent of leading or trailing).
  """
  fun name(): String => "builtin/String.remove"

  fun apply(h: TestHelper): TestResult =>
    let s1 = recover "  foo   bar  ".clone() end
    let s2 = recover "barfoobar".clone() end
    let s3 = recover "f-o-o-b-a-r!".clone() end
    let s4 = recover "f-o-o-b-a-r!".clone() end

    let r1 = s1.remove(" ")
    let r2 = s2.remove("foo")
    let r3 = s3.remove("-")
    let r4 = s4.remove("-!")

    h.expect_eq[U64](r1, 7)
    h.expect_eq[U64](r2, 1)
    h.expect_eq[U64](r3, 5)
    h.expect_eq[U64](r4, 0)

    h.expect_eq[String](consume s1, "foobar")
    h.expect_eq[String](consume s2, "barbar")
    h.expect_eq[String](consume s3, "foobar!")
    h.expect_eq[String](consume s4, "f-o-o-b-a-r!")

    true

class _TestSpecialValuesF32 iso is UnitTest
  """
  Test whether a F32 is infinite or NaN.
  """
  fun name(): String => "builtin/F32.finite"

  fun apply(h: TestHelper): TestResult =>
    // 1
    h.expect_true(F32(1.0).finite())
    h.expect_false(F32(1.0).nan())

    // -1
    h.expect_true(F32(-1.0).finite())
    h.expect_false(F32(-1.0).nan())

    // Infinity
    h.expect_false(F32(1.0 / 0.0).finite())
    h.expect_false(F32(1.0 / 0.0).nan())

    // - infinity
    h.expect_false(F32(-1.0 / 0.0).finite())
    h.expect_false(F32(-1.0 / 0.0).nan())

    // NaN
    h.expect_false(F32(0.0 / 0.0).finite())
    h.expect_true(F32(0.0 / 0.0).nan())

    true


class _TestSpecialValuesF64 iso is UnitTest
  """
  Test whether a F64 is infinite or NaN.
  """
  fun name(): String => "builtin/F64.finite"

  fun apply(h: TestHelper): TestResult =>
    // 1
    h.expect_true(F64(1.0).finite())
    h.expect_false(F64(1.0).nan())

    // -1
    h.expect_true(F64(-1.0).finite())
    h.expect_false(F64(-1.0).nan())

    // Infinity
    h.expect_false(F64(1.0 / 0.0).finite())
    h.expect_false(F64(1.0 / 0.0).nan())

    // - infinity
    h.expect_false(F64(-1.0 / 0.0).finite())
    h.expect_false(F64(-1.0 / 0.0).nan())

    // NaN
    h.expect_false(F64(0.0 / 0.0).finite())
    h.expect_true(F64(0.0 / 0.0).nan())

    true

class _TestStringReplace iso is UnitTest
  """
  Test String.replace
  """
  fun name(): String => "builtin/String.replace"

  fun apply(h: TestHelper): TestResult =>
    let s = String.append("this is a robbery, this is a stickup")
    s.replace("is a", "is not a")
    h.expect_eq[String box](s, "this is not a robbery, this is not a stickup")
    true

class _TestStringSplit iso is UnitTest
  """
  Test String.split
  """
  fun name(): String => "builtin/String.split"

  fun apply(h: TestHelper): TestResult =>
    try
      var r = "1 2 3  4".split()
      h.expect_eq[U64](r.size(), 5)
      h.expect_eq[String](r(0), "1")
      h.expect_eq[String](r(1), "2")
      h.expect_eq[String](r(2), "3")
      h.expect_eq[String](r(3), "")
      h.expect_eq[String](r(4), "4")

      r = "1 2 3  4".split(where n = 3)
      h.expect_eq[U64](r.size(), 3)
      h.expect_eq[String](r(0), "1")
      h.expect_eq[String](r(1), "2")
      h.expect_eq[String](r(2), "3  4")

      true
    else
      false
    end

class _TestStringCompare iso is UnitTest
  """
  Test comparing strings.
  """
  fun name(): String => "builtin/String.compare"

  fun apply(h: TestHelper): TestResult =>
    test(h, Equal, "foo", "foo", 3)
    test(h, Greater, "foo", "bar", 3)
    test(h, Less, "bar", "foo", 3)

    test(h, Equal, "foobar", "foo", 3)
    test(h, Greater, "foobar", "foo", 4)

    test(h, Less, "foobar", "oob", 3, 0)
    test(h, Equal, "foobar", "oob", 3, 1)

    test(h, Equal, "ab", "ab", 2 where ignore_case = false)
    test(h, Greater, "ab", "Ab", 2 where ignore_case = false)
    test(h, Greater, "ab", "aB", 2 where ignore_case = false)
    test(h, Greater, "ab", "AB", 2 where ignore_case = false)
    test(h, Less, "AB", "ab", 2 where ignore_case = false)
    test(h, Equal, "AB", "AB", 2 where ignore_case = false)

    test(h, Equal, "ab", "ab", 2 where ignore_case = true)
    test(h, Equal, "ab", "Ab", 2 where ignore_case = true)
    test(h, Equal, "ab", "aB", 2 where ignore_case = true)
    test(h, Equal, "ab", "AB", 2 where ignore_case = true)
    test(h, Equal, "AB", "ab", 2 where ignore_case = true)
    test(h, Equal, "AB", "AB", 2 where ignore_case = true)

    true

  fun test(h: TestHelper, expect: Compare, receiver: String box,
    that: String box, n: U64, offset: I64 = 0, that_offset: I64 = 0,
    ignore_case: Bool = false)
  =>
    let actual = receiver.compare_sub(that, n, offset, that_offset,
      ignore_case)

    if expect is actual then
      return
    end

    h.assert_failed("Expected " + string(expect) + " got " + string(actual) +
      " for \"" + receiver + "\".compare_sub(\"" + that + "\", " + n.string() +
      ", " + offset.string() + ", " + that_offset.string() + ", " +
      ignore_case.string() + ")")

  fun string(x: Compare): String =>
    match x
    | Equal => "Equal"
    | Less => "Less"
    | Greater => "Greater"
    else
      // Needed until we have exhaustive match checking
      ""
    end
