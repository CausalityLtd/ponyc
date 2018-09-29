use mut = "collections"

class val _MapEntry[K: Any #share, V: Any #share, H: mut.HashFunction[K] val]
  let key: K
  let value: V

  new val create(k: K, v: V) =>
    key = k
    value = v

  fun apply(k: K): (V | None) =>
    if H.eq(k, key) then value end

class val _MapCollisions[K: Any #share, V: Any #share, H: mut.HashFunction[K] val]
  let bins: Array[Array[_MapEntry[K, V, H]] trn] trn =
    [[]; []; []; []]

  fun val clone(): _MapCollisions[K, V, H] trn^ =>
    let cs = recover trn _MapCollisions[K, V, H] end
    try
      for i in bins.keys() do
        let x = recover bins(i)?.clone() end
        cs.bins.push(consume x)
      end
    end
    consume cs

  fun apply(hash: U32, k: K): (V | None) ? =>
    let idx = _Bits.mask32(hash, _Bits.collision_depth())
    let bin = bins(idx.usize_unsafe())?
    for node in bin.values() do
      if H.eq(k, node.key) then return node.value end
    end

  fun val remove(hash: U32, k: K): _MapCollisions[K, V, H] ? =>
    let idx = _Bits.mask32(hash, _Bits.collision_depth())
    let bin = bins(idx.usize_unsafe())?
    for (i, node) in bin.pairs() do
      if H.eq(k, node.key) then
        let bin' = recover bin.clone() end
        bin'.delete(i)?
        let n = clone()
        n.bins(idx.usize_unsafe())? = consume bin'
        return consume n
      end
    end
    error

  fun ref put_mut(hash: U32, entry: _MapEntry[K, V, H]): Bool ? =>
    let idx = _Bits.mask32(hash, _Bits.collision_depth())
    for i in mut.Range(0, bins(idx.usize_unsafe())?.size()) do
      let e = bins(idx.usize_unsafe())?(i)?
      if H.eq(entry.key, e.key) then
        bins(idx.usize_unsafe())?.push(entry)
        return false
      end
    end
    bins(idx.usize_unsafe())?.push(entry)
    true

type _MapNode[K: Any #share, V: Any #share, H: mut.HashFunction[K] val] is
  ( _MapEntry[K, V, H]
  | _MapCollisions[K, V, H]
  | _MapSubNodes[K, V, H]
  )

class val _MapSubNodes[K: Any #share, V: Any #share, H: mut.HashFunction[K] val]
  let nodes: Array[_MapNode[K, V, H]]
  var node_map: U32
  var data_map: U32

  new iso create(ns: Array[_MapNode[K, V, H]] iso = [], nm: U32 = 0, dm: U32 = 0) =>
    nodes = consume ns
    node_map = nm
    data_map = dm

  fun val clone(): _MapSubNodes[K, V, H] iso^ =>
    _MapSubNodes[K, V, H](recover nodes.clone() end, node_map, data_map)

  fun compressed_idx(idx: U32): U32 =>
    if not _Bits.check_bit(node_map or data_map, idx) then return -1 end
    let msk = not (U32(-1) << idx)
    if _Bits.check_bit(data_map, idx) then
      return (data_map and msk).popcount()
    end
    data_map.popcount() + (node_map and msk).popcount()

  fun apply(depth: U32, hash: U32, k: K): (V | None) ? =>
    let idx = _Bits.mask32(hash, depth)
    let c_idx = compressed_idx(idx)
    if c_idx == -1 then return None end
    match nodes(c_idx.usize_unsafe())?
    | let entry: _MapEntry[K, V, H] box => entry(k)
    | let sns: _MapSubNodes[K, V, H] box => sns(depth + 1, hash, k)?
    | let cs: _MapCollisions[K, V, H] box => cs(hash, k)?
    end

  // TODO: compact on remove
  fun val remove(depth: U32, hash: U32, k: K): _MapSubNodes[K, V, H] ? =>
    let idx = _Bits.mask32(hash, depth)
    let c_idx = compressed_idx(idx)

    if c_idx == -1 then error end

    let ns = recover nodes.clone() end
    var nm = node_map
    var dm = data_map

    if _Bits.check_bit(data_map, idx) then
      dm = _Bits.clear_bit(data_map, idx)
      ns.delete(c_idx.usize_unsafe())?
    else
      match nodes(c_idx.usize_unsafe())?
      | let entry: _MapEntry[K, V, H] val => error
      | let sns: _MapSubNodes[K, V, H] val =>
        ns(c_idx.usize_unsafe())? = sns.remove(depth + 1, hash, k)?
      | let cs: _MapCollisions[K, V, H] val =>
        ns(c_idx.usize_unsafe())? = cs.remove(hash, k)?
      end
    end

    _MapSubNodes[K, V, H](consume ns, nm, dm)

  fun ref put_mut(depth: U32, hash: U32, k: K, v: V): Bool ? =>
    let idx = _Bits.mask32(hash, depth)
    var c_idx = compressed_idx(idx)

    if c_idx == -1 then
      data_map = _Bits.set_bit(data_map, idx)
      c_idx = compressed_idx(idx)
      nodes.insert(c_idx.usize_unsafe(), _MapEntry[K, V, H](k, v))?
      return true
    end

    if _Bits.check_bit(node_map, idx) then
      var insert = false
      if depth < (_Bits.collision_depth() - 1) then
        let sn =
          (nodes(c_idx.usize_unsafe())? as _MapSubNodes[K, V, H]).clone()
        insert = sn.put_mut(depth + 1, hash, k, v)?
        nodes(c_idx.usize_unsafe())? = consume sn
      else
        let cs =
          (nodes(c_idx.usize_unsafe())? as _MapCollisions[K, V, H]).clone()
        insert = cs.put_mut(hash, _MapEntry[K, V, H](k, v))?
        nodes(c_idx.usize_unsafe())? = consume cs
      end
      return insert
    end

    // Debug([_Bits.check_bit(data_map, idx)])

    let entry0 = nodes(c_idx.usize_unsafe())? as _MapEntry[K, V, H]
    if H.eq(k, entry0.key) then
      nodes(c_idx.usize_unsafe())? = _MapEntry[K, V, H](k, v)
      return false
    end

    if depth < (_Bits.collision_depth() - 1) then
      let hash0 = H.hash(entry0.key).u32()
      let idx0 = _Bits.mask32(hash0, depth + 1)
      let sub_node = _MapSubNodes[K, V, H]([entry0], 0, _Bits.set_bit(0, idx0))
      sub_node.put_mut(depth + 1, hash, k, v)?

      nodes.delete(c_idx.usize_unsafe())?
      data_map = _Bits.clear_bit(data_map, idx)
      node_map = _Bits.set_bit(node_map, idx)
      c_idx = compressed_idx(idx)
      nodes.insert(c_idx.usize_unsafe(), consume sub_node)?
    else
      let sub_node = recover trn _MapCollisions[K, V, H] end
      let hash0 = H.hash(entry0.key).u32()
      let idx0 = _Bits.mask32(hash0, _Bits.collision_depth())
      sub_node.put_mut(hash0, entry0)?
      sub_node.bins(idx0.usize_unsafe())?.push(entry0)
      let idx1 = _Bits.mask32(hash, _Bits.collision_depth())
      sub_node.bins(idx1.usize_unsafe())?.push(_MapEntry[K, V, H](k, v))

      nodes.delete(c_idx.usize_unsafe())?
      data_map = _Bits.clear_bit(data_map, idx)
      node_map = _Bits.set_bit(node_map, idx)
      c_idx = compressed_idx(idx)
      nodes.insert(c_idx.usize_unsafe(), consume sub_node)?
    end
    true

  fun val put(depth: U32, hash: U32, k: K, v: V)
    : (_MapSubNodes[K, V, H], Bool) ?
  =>
    let node = clone()
    let r = node.put_mut(depth, hash, k, v)?
    (consume node, r)

  fun val iter(): _MapIter[K, V, H] =>
    object ref is _MapIter[K, V, H]
      let node: _MapSubNodes[K, V, H] = this
      var _idx: USize = 0

      fun ref has_next(): Bool =>
        _idx < node.nodes.size()

      fun ref next(): (_MapEntry[K, V, H] | _MapIter[K, V, H]) ? =>
        match node.nodes(_idx = _idx + 1)?
        | let e: _MapEntry[K, V, H] => e
        | let ns: _MapSubNodes[K, V, H] => ns.iter()
        else error // TODO
        end
    end
