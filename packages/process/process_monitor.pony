"""
# Process package

The Process package provides support for handling Unix style processes.
For each external process that you want to handle, you need to create a
`ProcessMonitor` and a corresponding `ProcessNotify` object. Each
ProcessMonitor runs as it own actor and upon receiving data will call its
corresponding `ProcessNotify`'s method.

## Example program

The following program will spawn an external program and write to it's
STDIN. Output received on STDOUT of the child process is forwarded to the
ProcessNotify client and printed.

```pony
use "process"
use "files"

actor Main
  new create(env: Env) =>
    // create a notifier
    let client = ProcessClient(env)
    let notifier: ProcessNotify iso = consume client
    // define the binary to run
    try
      let path = FilePath(env.root as AmbientAuth, "/bin/cat")?
      // define the arguments; first arg is always the binary name
      let args: Array[String] val = ["cat"]
      // define the environment variable for the execution
      let vars: Array[String] val = ["HOME=/"; "PATH=/bin"]
      // create a ProcessMonitor and spawn the child process
      let auth = env.root as AmbientAuth
      let pm: ProcessMonitor = ProcessMonitor(auth, auth, consume notifier,
      path, args, vars)
      // write to STDIN of the child process
      pm.write("one, two, three")
      pm.done_writing() // closing stdin allows cat to terminate
    else
      env.out.print("Could not create FilePath!")
    end

// define a client that implements the ProcessNotify interface
class ProcessClient is ProcessNotify
  let _env: Env

  new iso create(env: Env) =>
    _env = env

  fun ref stdout(process: ProcessMonitor ref, data: Array[U8] iso) =>
    let out = String.from_array(consume data)
    _env.out.print("STDOUT: " + out)

  fun ref stderr(process: ProcessMonitor ref, data: Array[U8] iso) =>
    let err = String.from_array(consume data)
    _env.out.print("STDERR: " + err)

  fun ref failed(process: ProcessMonitor ref, err: ProcessError) =>
    match err
    | ExecveError => _env.out.print("ProcessError: ExecveError")
    | PipeError => _env.out.print("ProcessError: PipeError")
    | ForkError => _env.out.print("ProcessError: ForkError")
    | WaitpidError => _env.out.print("ProcessError: WaitpidError")
    | WriteError => _env.out.print("ProcessError: WriteError")
    | KillError => _env.out.print("ProcessError: KillError")
    | CapError => _env.out.print("ProcessError: CapError")
    else _env.out.print("Unknown ProcessError!")
    end

  fun ref dispose(process: ProcessMonitor ref, child_exit_code: I32) =>
    let code: I32 = consume child_exit_code
    _env.out.print("Child exit code: " + code.string())
```

## Process portability

The ProcessMonitor supports spawning processes on Linux, FreeBSD and OSX.
Processes are not supported on Windows and attempting to use them will cause
a runtime error.

## Shutting down ProcessMonitor and external process

Document waitpid behaviour (stops world)

"""
use "backpressure"
use "collections"
use "files"
use "time"

primitive ExecveError
primitive PipeError
primitive ForkError
primitive WaitpidError
primitive WriteError
primitive KillError   // Not thrown at this time
primitive CapError
primitive ChdirError
primitive UnknownError

type ProcessError is
  ( ExecveError
  | ForkError
  | KillError
  | PipeError
  | WaitpidError
  | WriteError
  | CapError
  | ChdirError
  | UnknownError
  )

type ProcessMonitorAuth is (AmbientAuth | StartProcessAuth)

actor ProcessMonitor
  """
  Fork+execs / creates a child process and monitors it. Notifies a client about
  STDOUT / STDERR events.
  """
  let _notifier: ProcessNotify
  let _backpressure_auth: BackpressureAuth

  var _stdin: _Pipe = _Pipe.none()
  var _stdout: _Pipe = _Pipe.none()
  var _stderr: _Pipe = _Pipe.none()
  var _err: _Pipe = _Pipe.none()
  var _child: _Process = _ProcessNone

  let _max_size: USize = 4096
  var _read_buf: Array[U8] iso = recover Array[U8] .> undefined(_max_size) end
  var _read_len: USize = 0
  var _expect: USize = 0

  embed _pending: List[(ByteSeq, USize)] = _pending.create()
  var _done_writing: Bool = false

  var _closed: Bool = false
  var _timers: (Timers tag | None) = None  // For windows only

  new create(
    auth: ProcessMonitorAuth,
    backpressure_auth: BackpressureAuth,
    notifier: ProcessNotify iso,
    filepath: FilePath,
    args: Array[String] val,
    vars: Array[String] val,
    wdir: (FilePath | None) = None)
  =>
    """
    Create infrastructure to communicate with a forked child process and
    register the asio events. Fork child process and notify our user about
    incoming data via the notifier.
    """
    _backpressure_auth = backpressure_auth
    _notifier = consume notifier

    // We need permission to execute and the
    // file itself needs to be an executable
    if not filepath.caps(FileExec) then
      _notifier.failed(this, CapError)
      return
    end

    let ok = try
      FileInfo(filepath)?.file
    else
      false
    end
    if not ok then
      // unable to stat the file path given so it may not exist
      // or may be a directory.
      _notifier.failed(this, ExecveError)
      return
    end

    try
      _stdin = _Pipe.outgoing()?
      _stdout = _Pipe.incoming()?
      _stderr = _Pipe.incoming()?
      _err = _Pipe.incoming()?
    else
      _stdin.close()
      _stdout.close()
      _stderr.close()
      _err.close()
      _notifier.failed(this, PipeError)
      return
    end

    try
      ifdef posix then
        _child = _ProcessPosix.create(
          filepath.path, args, vars, wdir, _err, _stdin, _stdout, _stderr)?
      elseif windows then
        let windows_child = _ProcessWindows.create(
          filepath.path, args, vars, wdir, _stdin, _stdout, _stderr)
        _child = windows_child
        // notify about errors
        match windows_child.processError
        | let pe: ProcessError => 
          _notifier.failed(this, pe)
          return
        end
      else
        compile_error "unsupported platform"
      end
      _err.begin(this)
      _stdin.begin(this)
      _stdout.begin(this)
      _stderr.begin(this)

      // Asio is not wired up for Windows, so use a timer for now.
      ifdef windows then
        let timers = Timers
        let pm: ProcessMonitor tag = this
        let tn =
          object iso is TimerNotify
            fun ref apply(timer: Timer, count: U64): Bool =>
              pm.timer_notify()
              true
          end
        let timer = Timer(consume tn, 50_000_000, 10_000_000)
        timers(consume timer)
        _timers = timers
      end
    else
      _notifier.failed(this, ForkError)
      return
    end
    _notifier.created(this)

  be print(data: ByteSeq) =>
    """
    Print some bytes and append a newline.
    """
    if not _done_writing then
      _write_final(data)
      _write_final("\n")
    end

  be write(data: ByteSeq) =>
    """
    Write to STDIN of the child process.
    """
    if not _done_writing then
      _write_final(data)
    end

  be printv(data: ByteSeqIter) =>
    """
    Print an iterable collection of ByteSeqs.
    """
    for bytes in data.values() do
      _write_final(bytes)
      _write_final("\n")
    end

  be writev(data: ByteSeqIter) =>
    """
    Write an iterable collection of ByteSeqs.
    """
    for bytes in data.values() do
      _write_final(bytes)
    end

  be done_writing() =>
    """
    Set the _done_writing flag to true. If _pending is empty we can close the
    _stdin pipe.
    """
    _done_writing = true
    Backpressure.release(_backpressure_auth)
    if _pending.size() == 0 then
      _stdin.close_near()
    end

  be dispose() =>
    """
    Terminate child and close down everything.
    """
    Backpressure.release(_backpressure_auth)
    _child.kill()
    _close()

  fun ref expect(qty: USize = 0) =>
    """
    A `stdout` call on the notifier must contain exactly `qty` bytes. If
    `qty` is zero, the call can contain any amount of data.
    """
    _expect = _notifier.expect(this, qty)
    _read_buf_size()

  be _event_notify(event: AsioEventID, flags: U32, arg: U32) =>
    """
    Handle the incoming Asio event from one of the pipes.
    """
    match event
    | _stdin.event =>
      if AsioEvent.writeable(flags) then
        _pending_writes()
      elseif AsioEvent.disposable(flags) then
        _stdin.dispose()
      end
    | _stdout.event =>
      if AsioEvent.readable(flags) then
        _pending_reads(_stdout)
      elseif AsioEvent.disposable(flags) then
        _stdout.dispose()
      end
    | _stderr.event =>
      if AsioEvent.readable(flags) then
        _pending_reads(_stderr)
      elseif AsioEvent.disposable(flags) then
        _stderr.dispose()
      end
    | _err.event =>
      if AsioEvent.readable(flags) then
        _pending_reads(_err)
      elseif AsioEvent.disposable(flags) then
        _err.dispose()
      end
    end
    _try_shutdown()

  be timer_notify() =>
    """
    Windows IO polling timer has fired
    """
    _pending_writes() // try writes
    _pending_reads(_stdout)
    _pending_reads(_stderr)
    _try_shutdown()

  fun ref _close() =>
    """
    Close all pipes and wait for the child process to exit.
    """
    if not _closed then
      _closed = true
      _stdin.close()
      _stdout.close()
      _stderr.close()
      let exit_code = _child.wait()
      if exit_code < 0 then
        // An error waiting for pid
        _notifier.failed(this, WaitpidError)
      else
        // process child exit code
        _notifier.dispose(this, exit_code)
      end
      match _timers
      | let t: Timers => t.dispose()
      end
    end

  fun ref _try_shutdown() =>
    """
    If neither stdout nor stderr are open we close down and exit.
    """
    if _stdin.is_closed() and
      _stdout.is_closed() and
      _stderr.is_closed()
    then
       _close()
    end

  fun ref _pending_reads(pipe: _Pipe) =>
    """
    Read from stdout or stderr while data is available. If we read 4 kb of
    data, send ourself a resume message and stop reading, to avoid starving
    other actors.
    It's safe to use the same buffer for stdout and stderr because of
    causal messaging. Events get processed one _after_ another.
    """
    if pipe.is_closed() then return end
    var sum: USize = 0
    while true do
      (_read_buf, let len, let errno) =
        pipe.read(_read_buf = recover Array[U8] end, _read_len)

      let next = _read_buf.space()
      match len
      | -1 =>
        if (errno == _EAGAIN()) then
          return // nothing to read right now, try again later
        end
        pipe.close()
        return
      | 0  =>
        pipe.close()
        return
      end

      _read_len = _read_len + len.usize()

      let data = _read_buf = recover Array[U8] .> undefined(next) end
      data.truncate(_read_len)

      match pipe.near_fd
      | _stdout.near_fd =>
        if _read_len >= _expect then
          _notifier.stdout(this, consume data)
        end
      | _stderr.near_fd =>
        _notifier.stderr(this, consume data)
      | _err.near_fd =>
        let child_errno: I32 = try data.read_u32(0)?.i32() else -1 end
        let step: U8 = try data.read_u8(4)? else -1 end
        match step
        | _StepChdir() =>
          _notifier.failed(this, ChdirError)
        | _StepExecve() =>
          _notifier.failed(this, ExecveError)
        else
          _notifier.failed(this, UnknownError)
        end
      end

      _read_len = 0
      _read_buf_size()

      sum = sum + len.usize()
      if sum > (1 << 12) then
        // If we've read 4 kb, yield and read again later.
        _read_again(pipe.near_fd)
        return
      end
    end

  fun ref _read_buf_size() =>
    if _expect > 0 then
      _read_buf.undefined(_expect)
    else
      _read_buf.undefined(_max_size)
    end

  be _read_again(near_fd: U32) =>
    """
    Resume reading on file descriptor.
    """
    match near_fd
    | _stdout.near_fd => _pending_reads(_stdout)
    | _stderr.near_fd => _pending_reads(_stderr)
    end

  fun ref _write_final(data: ByteSeq) =>
    """
    Write as much as possible to the pipe if it is open and there are no
    pending writes. Save everything unwritten into _pending and apply
    backpressure.
    """
    if (not _closed) and not _stdin.is_closed() and (_pending.size() == 0) then
      // Send as much data as possible.
      (let len, let errno) = _stdin.write(data, 0)

      if len == -1 then // write error
        if errno == _EAGAIN() then
          // Resource temporarily unavailable, send data later.
          _pending.push((data, 0))
          Backpressure.apply(_backpressure_auth)
        else
          // Notify caller of error, close fd and done.
          _notifier.failed(this, WriteError)
          _stdin.close_near()
        end
      elseif len.usize() < data.size() then
        // Send any remaining data later.
        _pending.push((data, len.usize()))
        Backpressure.apply(_backpressure_auth)
      end
    else
      // Send later, when the pipe is available for writing.
      _pending.push((data, 0))
      Backpressure.apply(_backpressure_auth)
    end

  fun ref _pending_writes() =>
    """
    Send any pending data. If any data can't be sent, keep it in _pending.
    Once _pending is non-empty, direct writes will get queued there,
    and they can only be written here. If the _done_writing flag is set, close
    the pipe once we've processed pending writes.
    """
    while (not _closed) and not _stdin.is_closed() and (_pending.size() > 0) do
      try
        let node = _pending.head()?
        (let data, let offset) = node()?

        // Write as much data as possible.
        (let len, let errno) = _stdin.write(data, offset)

        if len == -1 then // OS signals write error
          if errno == _EAGAIN() then
            // Resource temporarily unavailable, send data later.
            return
          else
            // Close pipe and bail out.
            _notifier.failed(this, WriteError)
            _stdin.close_near()
            return
          end
        elseif (len.usize() + offset) < data.size() then
          // Send remaining data later.
          node()? = (data, offset + len.usize())
          return
        else
          // This pending chunk has been fully sent.
          _pending.shift()?
          if (_pending.size() == 0) then
            Backpressure.release(_backpressure_auth)
            // check if the client has signaled it is done
            if _done_writing then
              _stdin.close_near()
            end
          end
        end
      else
        // handle error
        _notifier.failed(this, WriteError)
        return
      end
    end
