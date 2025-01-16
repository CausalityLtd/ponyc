use mut = "collections"

type Set[A: (mut.Hashable val & Equatable[A] val)] is HashSet[A, mut.HashEq[A]]

type SetIs[A: Any #share] is HashSet[A, mut.HashIs[A]]

class val HashSet[A: Any #share, H: mut.HashFunction[A] val]
  is Comparable[HashSet[A, H] box]
  """
  A set, built on top of persistent Map. This is implemented as map of an
  alias of a type to itself.
  """
  let _map: HashMap[A, A, H]

  new val create() =>
    _map = HashMap[A, A, H]

  new val _create(map': HashMap[A, A, H]) =>
    _map = map'

  fun size(): USize =>
    """
    Return the number of elements in the set.
    """
    _map.size()

  fun apply(value: val->A): val->A ? =>
    """
    Return the value if it is in the set, otherwise raise an error.
    """
    _map(value)?

  fun contains(value: val->A): Bool =>
    """
    Check whether the set contains the value.
    """
    _map.contains(value)

  fun val add(value: val->A): HashSet[A, H] =>
    """
    Return a set with the value added.
    """
    _create(_map(value) = value)

  fun val sub(value: val->A): HashSet[A, H] =>
    """
    Return a set with the value removed.
    """
    try _create(_map.remove(value)?) else this end

  fun val op_or(that: (HashSet[A, H] | Iterator[A])): HashSet[A, H] =>
    """
    Return a set with the elements of both this and that.
    """
    let iter =
      match that
      | let s: HashSet[A, H] => s.values()
      | let i: Iterator[A] => i
      end
    var s' = this
    for v in iter do
      s' = s' + v
    end
    s'

  fun val op_and(that: (HashSet[A, H] | Iterator[A])): HashSet[A, H] =>
    """
    Return a set with the elements that are in both this and that.
    """
    let iter =
      match that
      | let s: HashSet[A, H] => s.values()
      | let i: Iterator[A] => i
      end
    var s' = create()
    for v in iter do
      if contains(v) then
        s' = s' + v
      end
    end
    s'

  fun val op_xor(that: (HashSet[A, H] | Iterator[A])): HashSet[A, H] =>
    """
    Return a set with elements that are in either this or that, but not both.
    """
    let iter =
      match that
      | let s: HashSet[A, H] => s.values()
      | let i: Iterator[A] => i
      end
    var s' = this
    for v in iter do
      if contains(v) then
        s' = s' - v
      else
        s' = s' + v
      end
    end
    s'

  fun val without(that: (HashSet[A, H] | Iterator[A])): HashSet[A, H] =>
    """
    Return a set with the elements of this that are not in that.
    """
    let iter =
      match that
      | let s: HashSet[A, H] => s.values()
      | let i: Iterator[A] => i
      end
    var s' = this
    for v in iter do
      if contains(v) then
        s' = s' - v
      end
    end
    s'

  fun eq(that: HashSet[A, H] box): Bool =>
    """
    Return true if this and that contain the same elements.
    """
    (size() == that.size()) and (this <= that)

  fun lt(that: HashSet[A, H] box): Bool =>
    """
    Return true if every element in this is also in that, and this has fewer
    elements than that.
    """
    (size() < that.size()) and (this <= that)

  fun le(that: HashSet[A, H] box): Bool =>
    """
    Return true if every element in this is also in that.
    """
    for v in values() do
      if not that.contains(v) then return false end
    end
    true

  fun gt(that: HashSet[A, H] box): Bool =>
    """
    Return true if every element in that is also in this, and this has more
    elements than that.
    """
    (size() > that.size()) and (that <= this)

  fun ge(that: HashSet[A, H] box): Bool =>
    """
    Return true if every element in that is also in this.
    """
    that <= this

  fun values(): Iterator[A]^ =>
    """
    Return an iterator over the values in the set.
    """
    _map.values()
