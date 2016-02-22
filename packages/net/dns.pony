primitive DNS
  """
  Helper functions for resolving DNS queries.
  """
  fun _ip4(host: String, service: String): Array[IPAddress] iso^ =>
    """
    Gets all IPv4 addresses for a host and service.
    """
    _resolve(1, host, service)

  fun _ip6(host: String, service: String): Array[IPAddress] iso^ =>
    """
    Gets all IPv6 addresses for a host and service.
    """
    _resolve(2, host, service)

  fun _broadcast_ip4(service: String): Array[IPAddress] iso^ =>
    """
    Link-local IP4 broadcast address.
    """
    _ip4("255.255.255.255", service)

  fun _broadcast_ip6(service: String): Array[IPAddress] iso^ =>
    """
    Link-local IP6 broadcast address.
    """
    _ip6("FF02::1", service)

  fun is_ip4(host: String): Bool =>
    """
    Returns true if the host is a literal IPv4 address.
    """
    @os_host_ip4[Bool](host.cstring())

  fun is_ip6(host: String): Bool =>
    """
    Returns true if the host is a literal IPv6 address.
    """
    @os_host_ip6[Bool](host.cstring())

  fun _resolve(family: U32, host: String, service: String):
    Array[IPAddress] iso^
  =>
    """
    Turns an addrinfo pointer into an array of addresses.
    """
    var list = recover Array[IPAddress] end
    var result = @os_addrinfo[Pointer[U8]](
      family, host.cstring(), service.cstring())

    if not result.is_null() then
      var addr = result

      while not addr.is_null() do
        let ip = recover IPAddress end
        @os_getaddr[None](addr, ip)
        list.push(consume ip)
        addr = @os_nextaddr[Pointer[U8]](addr)
      end

      @freeaddrinfo[None](result)
    end

    list
