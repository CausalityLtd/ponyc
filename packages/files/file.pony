primitive _FileHandle

class File
  """
  Operations on a file.
  """
  let path: String
  let writeable: Bool
  var _handle: Pointer[_FileHandle]
  var _last_line_length: U64 = 256

  new create(path': String) ? =>
    """
    Open for read/write, creating if it doesn't exist, truncating it if it
    does exist.
    """
    path = path'
    writeable = true
    _handle = @fopen[Pointer[_FileHandle]](path.cstring(), "w+b".cstring())

    if _handle.is_null() then
      error
    end

  new open(path': String) ? =>
    """
    Open for read only, failing if it doesn't exist.
    """
    path = path'
    writeable = false
    _handle = @fopen[Pointer[_FileHandle]](path.cstring(), "rb".cstring())

    if _handle.is_null() then
      error
    end

  new modify(path': String) ? =>
    """
    Open for read/write, creating if it doesn't exist, preserving the contents
    if it does exist.
    """
    path = path'
    writeable = true
    _handle = @fopen[Pointer[_FileHandle]](path.cstring(), "r+b".cstring())

    if _handle.is_null() then
      error
    end

  fun valid(): Bool =>
    """
    Returns true if the file is currently open.
    """
    not _handle.is_null()

  fun ref line(): String iso^ ? =>
    """
    Returns a line as a String. The newline is not included in the string. If
    there is no more data, this raises an error.
    """
    if _handle.is_null() then
      error
    end

    var offset: U64 = 0
    var len = _last_line_length
    var result = recover String end
    var done = false

    while not done do
      result.reserve(len)

      var r = if Platform.linux() then
        @fgets_unlocked[Pointer[U8]](
          result.cstring().u64() + offset, len - offset, _handle
          )
      else
        @fgets[Pointer[U8]](
          result.cstring().u64() + offset, len - offset, _handle
          )
      end

      result.recalc()

      done = try
        r.is_null() or (result.at_offset(-1) == '\n')
      else
        true
      end

      if not done then
        offset = result.size()
        len = len * 2
      end
    end

    if result.size() == 0 then
      error
    end

    try
      if result.at_offset(-1) == '\n' then
        result.truncate(result.size() - 1)

        if result.at_offset(-1) == '\r' then
          result.truncate(result.size() - 1)
        end
      end
    end

    _last_line_length = len
    result

  fun ref read(len: U64): Array[U8] iso^ =>
    """
    Returns up to len bytes.
    """
    if not _handle.is_null() then
      var result = recover Array[U8].undefined(len) end

      var r = if Platform.linux() then
        @fread_unlocked[U64](result.cstring(), U64(1), len, _handle)
      else
        @fread[U64](result.cstring(), U64(1), len, _handle)
      end

      result.truncate(r)
      result
    else
      recover Array[U8] end
    end

  fun ref read_string(len: U64): String iso^ =>
    """
    Returns up to len bytes. The resulting string may have internal null
    characters.
    """
    if not _handle.is_null() then
      var result = recover String(len) end

      var r = if Platform.linux() then
        @fread_unlocked[U64](result.cstring(), U64(1), len, _handle)
      else
        @fread[U64](result.cstring(), U64(1), len, _handle)
      end

      result.truncate(r)
      result
    else
      recover String end
    end

  fun ref print(data: Bytes box): Bool =>
    """
    Same as write, buts adds a newline.
    """
    write(data) and write("\n")

  fun ref printv(data: BytesList box): Bool =>
    """
    Print an array of Bytes.
    """
    var r = true

    for bytes in data.values() do
      r = r and write(bytes) and write("\n")
    end

    r

  fun ref write(data: Bytes box): Bool =>
    """
    Returns false if the file wasn't opened with write permission.
    Returns false and closes the file if not all the bytes were written.
    """
    if writeable and (not _handle.is_null()) then
      var len = if Platform.linux() then
        @fwrite_unlocked[U64](data.cstring(), U64(1), data.size(), _handle)
      else
        @fwrite[U64](data.cstring(), U64(1), data.size(), _handle)
      end

      if len == data.size() then
        return true
      end

      dispose()
    end
    false

  fun ref writev(data: BytesList box): Bool =>
    """
    Write an array of Bytes.
    """
    var r = true

    for bytes in data.values() do
      r = r and write(bytes)
    end

    r

  fun position(): U64 =>
    """
    Return the current cursor position in the file.
    """
    if not _handle.is_null() then
      if Platform.windows() then
        @_ftelli64[U64](_handle)
      else
        @ftell[U64](_handle)
      end
    else
      0
    end

  fun ref size(): U64 =>
    """
    Return the total length of the file.
    """
    var pos = position()
    seek_end(0)
    var len = position()
    seek_start(pos)
    len

  fun ref seek_start(offset: U64): File =>
    """
    Set the cursor position relative to the start of the file.
    """
    if not _handle.is_null() then
      if Platform.windows() then
        @_fseeki64[I32](_handle, offset, I32(0))
      else
        @fseek[I32](_handle, offset, I32(0))
      end
    end
    this

  fun ref seek_end(offset: U64): File =>
    """
    Set the cursor position relative to the end of the file.
    """
    if not _handle.is_null() then
      if Platform.windows() then
        @_fseeki64[I32](_handle, -offset, I32(2))
      else
        @fseek[I32](_handle, -offset, I32(2))
      end
    end
    this

  fun ref seek(offset: I64): File =>
    """
    Move the cursor position.
    """
    if not _handle.is_null() then
      if Platform.windows() then
        @_fseeki64[I32](_handle, offset, I32(1))
      else
        @fseek[I32](_handle, offset, I32(1))
      end
    end
    this

  fun ref flush(): File =>
    """
    Flush the file.
    """
    if not _handle.is_null() then
      if Platform.linux() then
        @fflush_unlocked[I32](_handle)
      else
        @fflush[I32](_handle)
      end
    end
    this

  fun ref sync(): File =>
    """
    Sync the file contents to physical storage.
    """
    if not _handle.is_null() then
      if Platform.windows() then
        var fd = @_fileno[I32](_handle)
        var h = @_get_osfhandle[U64](fd)
        @FlushFileBuffers[I32](h)
      else
        var fd = @fileno[I32](_handle)
        @fsync[I32](fd)
      end
    end
    this

  fun ref set_length(len: U64): Bool =>
    """
    Change the file size. If it is made larger, the new contents are undefined.
    """
    if writeable and (not _handle.is_null()) then
      flush()
      var pos = position()
      var success = if Platform.windows() then
        var fd = @_fileno[I32](_handle)
        @_chsize_s[I32](fd, len) == 0
      else
        var fd = @fileno[I32](_handle)
        @ftruncate[I32](fd, len) == 0
      end

      if pos >= len then
        seek_end(0)
      end
      success
    end
    false

  fun ref lines(): FileLines =>
    """
    Returns an iterator for reading lines from the file.
    """
    FileLines(this)

  fun ref dispose() =>
    """
    Close the file. Future operations will do nothing.
    """
    if not _handle.is_null() then
      @fclose[I32](_handle)
      _handle = Pointer[_FileHandle]
    end

  fun _final() =>
    """
    Close the file.
    """
    if not _handle.is_null() then
      @fclose[I32](_handle)
    end

class FileLines is Iterator[String]
  """
  Iterate over the lines in a file.
  """
  var _file: File
  var _line: String = ""
  var _next: Bool = false

  new create(file: File) =>
    _file = file

    try
      _line = file.line()
      _next = true
    end

  fun ref has_next(): Bool =>
    _next

  fun ref next(): String =>
    let r = _line

    try
      _line = _file.line()
    else
      _next = false
    end

    r
