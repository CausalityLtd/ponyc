use "ponytest"

actor Main is TestList
  new create(env: Env) => PonyTest(env, this)
  new make() => None

  fun tag tests(test: PonyTest) =>
    test(_TestLongOptions)
    test(_TestShortOptions)
    test(_TestCombineShortOptions)
    test(_TestCombineShortArg)
    test(_TestArgEquals)
    test(_TestArgSpace)
    test(_TestArgLeadingDash)

class iso _TestLongOptions is UnitTest
  """
  Long options start with two leading dashes.
  """
  //let _env: Env

  fun name(): String => "options/Options.longOptions"

  fun apply(h: TestHelper): TestResult =>
    true

class iso _TestShortOptions is UnitTest
  """
  Short options start with a single leading dash.
  """
  //let _env: Env

  fun name(): String => "options/Options.shortOptions"

  fun apply(h: TestHelper): TestResult =>
    true

class iso _TestCombineShortOptions is UnitTest
  """
  Short options can be combined to one string with a single leading dash. 
  """
  //let _env: Env

  fun name(): String => "options/Options.combineShort"

  fun apply(h: TestHelper): TestResult =>
    true

class iso _TestCombineShortArg is UnitTest
  """
  Short options can be combined up to the first option that takes an argument.
  """
  //let _env: Env

  fun name(): String => "options/Options.combineShortArg"

  fun apply(h: TestHelper): TestResult =>
    true

class iso _TestArgEquals is UnitTest
  """
  Arguments can be distinguished from long and short options using '='.
  """
  //let _env: Env

  fun name(): String => "options/Options.testArgEquals"

  fun apply(h: TestHelper): TestResult =>
    true

class iso _TestArgSpace is UnitTest
  """
  Arguments can be distinguished from long and short options using space.
  """
  //let _env: Env

  fun name(): String => "options/Options.testArgSpace"

  fun apply(h: TestHelper): TestResult =>
    true

class iso _TestArgLeadingDash is UnitTest
  """
  Arguments can only start with a leading dash, if they are seperated from
  the option using '=', otherwise they will be interpreted as named options.
  """
  //let _env: Env

  fun name(): String => "options/Options.testArgLeadingDash"

  fun apply(h: TestHelper): TestResult =>
    true