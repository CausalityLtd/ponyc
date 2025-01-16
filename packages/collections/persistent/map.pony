use mut = "collections"

type Map[K: (mut.Hashable val & Equatable[K] val), V: Any #share] is
  HashMap[K, V, mut.HashEq[K]]
  """
  A map that uses structural equality on the key.
  """

type MapIs[K: Any #share, V: Any #share] is HashMap[K, V, mut.HashIs[K]]
  """
  A map that uses identity comparison on the key.
  """

class val HashMap[K: Any #share, V: Any #share, H: mut.HashFunction[K] val]
  """
  A persistent map based on the Compressed Hash Array Mapped Prefix-tree from
  'Optimizing Hash-Array Mapped Tries for Fast and Lean Immutable JVM
  Collections' by Michael J. Steindorfer and Jurgen J. Vinju

  ## Usage
  ```pony
  use "collections/persistent"

  actor Main
    new create(env: Env) =>
      try
        let m1 = Map[String, U32] // {}
        // Update returns a new map with the provided key set
        // to the provided value. The old map is unchanged.
        let m2 = m1("a") = 5 // {a: 5}
        let m3 = m2("b") = 10 // {a: 5, b: 10}
        let m4 = m3.remove("a")? // {b: 10}
        // You can create a new map from key value pairs.
        let m5 = Map[String, U32].concat([("a", 2); ("b", 3)].values()) // {a: 2, b: 3}
      end
  ```
  """
  let _root: _MapSubNodes[K, V, H]
  let _size: USize

  new val create() =>
    _root = _MapSubNodes[K, V, H]
    _size = 0

  new val _create(r: _MapSubNodes[K, V, H], s: USize) =>
    _root = r
    _size = s

  fun val apply(k: K): val->V ? =>
    """
    Attempt to get the value corresponding to k.
    """
    _root(0, H.hash(k).u32(), k)? as V

  fun val size(): USize =>
    """
    Return the amount of key-value pairs in the Map.
    """
    _size

  fun val update(key: K, value: val->V): HashMap[K, V, H] =>
    """
    Update the value associated with the provided key.
    """
    (let r, let insertion) =
      try
        _root.update(0, H.hash(key).u32(), key, value)?
      else
        (_root, false) // should not occur
      end
    let s = if insertion then _size + 1 else _size end
    _create(r, s)

  fun val remove(k: K): HashMap[K, V, H] ? =>
    """
    Try to remove the provided key from the Map.
    """
    _create(_root.remove(0, H.hash(k).u32(), k)?, _size - 1)

  fun val get_or_else(k: K, alt: val->V): val->V =>
    """
    Get the value associated with provided key if present. Otherwise,
    return the provided alternate value.
    """
    try
      match _root(0, H.hash(k).u32(), k)?
      | let v: V => v
      else alt
      end
    else
      alt // should not occur
    end

  fun val contains(k: K): Bool =>
    """
    Check whether the node contains the provided key.
    """
    try _root(0, H.hash(k).u32(), k)? isnt None
    else false // should not occur
    end

  fun val concat(iter: Iterator[(val->K, val->V)]): HashMap[K, V, H] =>
    """
    Add the K, V pairs from the given iterator to the map.
    """
    var m = this
    for (k, v) in iter do
      m = m.update(k, v)
    end
    m

  fun val add(key: K, value: val->V): HashMap[K, V, H] =>
    """
    Return this Map with the given (key, value) mapping.
    """
    update(key, value)

  fun val sub(key: K): HashMap[K, V, H] =>
    """
    Return this Map without the given key.
    """
    try
      remove(key)?
    else
      this
    end

  fun val keys(): MapKeys[K, V, H] => MapKeys[K, V, H](this)

  fun val values(): MapValues[K, V, H] => MapValues[K, V, H](this)

  fun val pairs(): MapPairs[K, V, H] => MapPairs[K, V, H](this)

  fun _root_node(): _MapSubNodes[K, V, H] => _root

class MapKeys[K: Any #share, V: Any #share, H: mut.HashFunction[K] val]
  embed _pairs: MapPairs[K, V, H]

  new create(m: HashMap[K, V, H]) => _pairs = MapPairs[K, V, H](m)

  fun has_next(): Bool => _pairs.has_next()

  fun ref next(): K ? => _pairs.next()?._1

class MapValues[K: Any #share, V: Any #share, H: mut.HashFunction[K] val]
  embed _pairs: MapPairs[K, V, H]

  new create(m: HashMap[K, V, H]) => _pairs = MapPairs[K, V, H](m)

  fun has_next(): Bool => _pairs.has_next()

  fun ref next(): val->V ? => _pairs.next()?._2

interface _MapIter[K: Any #share, V: Any #share, H: mut.HashFunction[K] val] is
  Iterator[(_MapEntry[K, V, H] | _MapIter[K, V, H])]

class MapPairs[K: Any #share, V: Any #share, H: mut.HashFunction[K] val]
  embed _stack: Array[_MapIter[K, V, H]] = []

  new create(m: HashMap[K, V, H]) =>
    _stack.push(m._root_node().iter())

  fun has_next(): Bool =>
    _stack.size() > 0

  fun ref next(): (K, V) ? =>
    let iter = _stack(_stack.size() - 1)?
    let x = iter.next()?
    if not iter.has_next() then _stack.pop()? end
    match x
    | let e: _MapEntry[K, V, H] => (e.key, e.value)
    | let i: _MapIter[K, V, H] =>
      _stack.push(i)
      next()?
    end
