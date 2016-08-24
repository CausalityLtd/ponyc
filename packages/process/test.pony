use "ponytest"
use "files"
use "capsicum"
use "collections"
use "time"

actor Main is TestList  
  new create(env: Env) => PonyTest(env, this)
  new make() => None

  fun tag tests(test: PonyTest) =>
    test(_TestStdinStdout)
    test(_TestStderr)
    test(_TestFileExec)
    test(_TestStdinWriteBuf)

class iso _TestStdinStdout is UnitTest
  var _pm: (ProcessMonitor | None) = None
  
  fun name(): String =>
    "process/STDIN-STDOUT"

  fun ref apply(h: TestHelper) =>
    let size: USize = 15 // length of "one, two, three" string
    let notifier: ProcessNotify iso = _ProcessClient(size, "", 0, h)
    try
      let path = FilePath(h.env.root as AmbientAuth, "/bin/cat")
      let args: Array[String] iso = recover Array[String](1) end
      args.push("cat")
      let vars: Array[String] iso = recover Array[String](2) end
      vars.push("HOME=/")
      vars.push("PATH=/bin")
    
      _pm = ProcessMonitor(consume notifier, path,
        consume args, consume vars)

      if _pm isnt None then // write to STDIN of the child process
        let pm = _pm as ProcessMonitor
        pm.write("one, two, three")
        pm.done_writing()  // closing stdin allows "cat" to terminate
      end
      h.long_test(5_000_000_000)
    else
      h.fail("Could not create FilePath!")
    end

  fun timed_out(h: TestHelper) =>
    try
      if _pm isnt None then // kill the child process and cleanup fd
        (_pm as ProcessMonitor).dispose()
      end
    else
      h.fail("Error disposing of forked process in STDIN-WriteBuf test")
    end
    h.complete(false)

class iso _TestStdinWriteBuf is UnitTest
  var _pm: (ProcessMonitor | None) = None
  let _test_start: U64 = Time.nanos()
  
  fun name(): String =>
    "process/STDIN-WriteBuf"

  fun ref apply(h: TestHelper) =>
    let pipe_cap: USize = 65536
    let notifier: ProcessNotify iso = _ProcessClient((pipe_cap + 1) * 4,
      "", 0, h)
    try
      let path = FilePath(h.env.root as AmbientAuth, "/bin/cat")
      let args: Array[String] iso = recover Array[String](1) end
      args.push("cat")
      let vars: Array[String] iso = recover Array[String](2) end
      vars.push("HOME=/")
      vars.push("PATH=/bin")

      // fork the child process and attach a ProcessMonitor
      _pm = ProcessMonitor(consume notifier, path, consume args, consume vars)

      // create a message larger than pipe_cap bytes 
      let message: Array[U8] val = recover Array[U8].undefined(pipe_cap + 1) end

      if _pm isnt None then // write to STDIN of the child process
        let pm = _pm as ProcessMonitor
        pm.write(message)
        pm.write(message)
        pm.write(message)
        pm.write(message)
        pm.done_writing() // closing stdin allows "cat" to terminate
      end
      h.long_test(2_000_000_000)
    else
      h.fail("Error running STDIN-WriteBuf test")
    end

  fun timed_out(h: TestHelper) =>
    h.log("_TestStdinWriteBuf.timed_out: ran for " +
     (Time.nanos() - _test_start).string() + " ns")
    try
      if _pm isnt None then // kill the child process and cleanup fd
        h.log("_TestStdinWriteBuf.timed_out: calling pm.dispose()")
        (_pm as ProcessMonitor).dispose()
      end
    else
      h.fail("Error disposing of forked process in STDIN-WriteBuf test")
    end
    h.complete(false)

    
class iso _TestStderr is UnitTest
  var _pm: (ProcessMonitor | None) = None
  
  fun name(): String =>
    "process/STDERR"

  fun ref apply(h: TestHelper) =>
    let notifier: ProcessNotify iso = _ProcessClient(0,
      "cat: file_does_not_exist: No such file or directory\n", 1, h)
    try
      let path = FilePath(h.env.root as AmbientAuth, "/bin/cat")
      let args: Array[String] iso = recover Array[String](2) end
      args.push("cat")
      args.push("file_does_not_exist")
   
      let vars: Array[String] iso = recover Array[String](2) end
      vars.push("HOME=/")
      vars.push("PATH=/bin")
    
      _pm = ProcessMonitor(consume notifier, path,
        consume args, consume vars)

      if _pm isnt None then // write to STDIN of the child process
        let pm = _pm as ProcessMonitor
        pm.done_writing() // closing stdin
      end
      h.long_test(5_000_000_000)
    else
      h.fail("Could not create FilePath!")
    end
    
  fun timed_out(h: TestHelper) =>
    try
      if _pm isnt None then // kill the child process and cleanup fd
        (_pm as ProcessMonitor).dispose()
      end
    else
      h.fail("Error disposing of forked process in STDIN-WriteBuf test")
    end
    h.complete(false)

class iso _TestFileExec is UnitTest
  """
  This test is expected to generate a CapError which when received
  by the notifier will terminate the test with success. 
  """
  fun name(): String =>
    "process/FileExec"

  fun apply(h: TestHelper) =>
    let notifier: ProcessNotify iso = _ProcessClient(0,
      "", 1, h)
    try
      let path = FilePath(h.env.root as AmbientAuth, "/bin/date",
        recover val FileCaps.all().unset(FileExec) end)
      let args: Array[String] iso = recover Array[String](1) end
      args.push("date")    
      let vars: Array[String] iso = recover Array[String](2) end
      vars.push("HOME=/")
      vars.push("PATH=/bin")
    
      let pm: ProcessMonitor = ProcessMonitor(consume notifier, path,
        consume args, consume vars)
      
      h.long_test(5_000_000_000)
    else
      h.fail("Could not create FilePath!")
    end

  fun timed_out(h: TestHelper) =>
    h.complete(false)


class _ProcessClient is ProcessNotify
  """
  Notifications for Process connections.
  """
  let _out: USize
  let _err: String
  let _exit_code: I32
  let _h: TestHelper
  var _d_stdout_chars: USize = 0
  let _d_stderr: String ref = String
  let _created: U64
  var _first_data: U64 = 0
  
  new iso create(out: USize, err: String, exit_code: I32,
    h: TestHelper) =>
    _out = out
    _err = err
    _exit_code = exit_code
    _h = h
    _created = Time.nanos()
    
  fun ref stdout(data: Array[U8] iso) => 
    """
    Called when new data is received on STDOUT of the forked process
    """
    _h.log("\tReceived from stdout: " + data.size().string() + " bytes")
    if (_first_data == 0) then
      _first_data = Time.nanos()
    end
    _d_stdout_chars = _d_stdout_chars + (consume data).size()
    _h.log("\tReceived so far: " +_d_stdout_chars.string() + " bytes")
    _h.log("\tExpecting: " +_out.string() + " bytes")


  fun ref stderr(data: Array[U8] iso) => _d_stderr.append(consume data)
    """
    Called when new data is received on STDERR of the forked process
    """
    
  fun ref failed(err: ProcessError) =>
    """
    ProcessMonitor calls this if we run into errors with the
    forked process.
    """
    match err
    | ExecveError   => _h.fail("ProcessError: ExecveError")
    | PipeError     => _h.fail("ProcessError: PipeError")
    | Dup2Error     => _h.fail("ProcessError: Dup2Error")
    | ForkError     => _h.fail("ProcessError: ForkError")
    | FcntlError    => _h.fail("ProcessError: FcntlError")
    | WaitpidError  => _h.fail("ProcessError: WaitpidError")
    | CloseError    => _h.fail("ProcessError: CloseError")
    | ReadError     => _h.fail("ProcessError: ReadError")
    | WriteError    => _h.fail("ProcessError: WriteError")
    | KillError     => _h.fail("ProcessError: KillError")
    | Unsupported   => _h.fail("ProcessError: Unsupported") 
    | CapError      => _h.complete(true) // used in _TestFileExec
    else
      _h.fail("Unknown ProcessError!")
    end
    
  fun ref dispose(child_exit_code: I32) =>
    """
    Called when ProcessMonitor terminates to cleanup ProcessNotify
    We receive the exit code of the child process from ProcessMonitor.
    """
    let last_data: U64 = Time.nanos()
    _h.log("dispose: child exit code: " + child_exit_code.string())
    _h.log("dispose: stdout: " + _d_stdout_chars.string() + " bytes")
    _h.log("dispose: stderr: " + _d_stderr)
    if (_first_data > 0) then
      _h.log("dispose: received first data after: \t" + (_first_data - _created).string()
        + " ns")
    end
    _h.log("dispose: total data process_time: \t" + (last_data - _first_data).string()
      + " ns")
    _h.log("dispose: ProcessNotify lifetime: \t" + (last_data - _created).string()
      + " ns")
    
    _h.assert_eq[USize](_out, _d_stdout_chars)
    _h.assert_eq[String box](_err, _d_stderr)
    _h.assert_eq[I32](_exit_code, child_exit_code)
    _h.complete(true)
