"""
# Pony Standard Library

This package represents the test suite for the Pony standard library.

For every new package, please add a Main actor and tests to the package in a
file called 'test.pony'. Then add a corresponding use directive and a line to
the main actor constructor of this package to invoke those tests.

All tests can be run by compiling and running packages/stdlib.
"""

// Include ALL standard library packages here, even if they don't have tests.
// That way stdlib can be used to type check the whole standard library,
// generate docs for it, etc.
use "ponytest"
use assert = "assert"
use backpressure= "backpressure"
use base64 = "encode/base64"
use buffered = "buffered"
use builtin_test = "builtin_test"
use bureaucracy = "bureaucracy"
use capsicum = "capsicum"
use cli = "cli"
use collections = "collections"
use collections_persistent = "collections/persistent"
use crypto = "crypto"
use debug = "debug"
use files = "files"
use format = "format"
use glob = "glob"
use ini = "ini"
use itertools = "itertools"
use json = "json"
use logger = "logger"
use math = "math"
use net = "net"
use options = "options"
use ponybench = "ponybench"
use process = "process"
use promises = "promises"
use random = "random"
use regex = "regex"
use serialise = "serialise"
use signals = "signals"
use ssl = "net/ssl"
use strings = "strings"
use term = "term"
use time = "time"


actor Main is TestList
  new create(env: Env) => PonyTest(env, this)
  new make() => None

  fun tag tests(test: PonyTest) =>
    base64.Main.make().tests(test)
    builtin_test.Main.make().tests(test)
    buffered.Main.make().tests(test)
    bureaucracy.Main.make().tests(test)
    cli.Main.make().tests(test)
    collections.Main.make().tests(test)
    collections_persistent.Main.make().tests(test)
    crypto.Main.make().tests(test)
    files.Main.make().tests(test)
    format.Main.make().tests(test)

    ifdef not windows then
      // The glob tests are currently very broken on Windows, so ignore them.
      glob.Main.make().tests(test)
    end

    ini.Main.make().tests(test)
    json.Main.make().tests(test)
    logger.Main.make().tests(test)
    net.Main.make().tests(test)
    options.Main.make().tests(test)
    promises.Main.make().tests(test)

    ifdef posix then
      // The process package currently only supports posix
      process.Main.make().tests(test)
    end

    regex.Main.make().tests(test)

    ifdef not windows then
      // The signals tests currently abort the process on Windows, so ignore
      // them.
      signals.Main.make().tests(test)
    end

    random.Main.make().tests(test)
    strings.Main.make().tests(test)
    time.Main.make().tests(test)
    itertools.Main.make().tests(test)
    serialise.Main.make().tests(test)
