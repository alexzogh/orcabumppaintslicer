# MCP geometry: shipping the plate to something that draws it

The rest of the MCP surface describes the project in words and numbers — object ids, sizes,
settings, slice reports. A client that wants to *show* the plate needs the triangles themselves,
and triangles are a different kind of payload: three orders of magnitude larger than anything else
a tool returns, mostly unchanged between calls, and useless without knowing exactly how the bytes
are laid out. Two tools carry them, split so that the large half is fetched only when it is
actually new.

## The split: `get_scene` and `get_mesh`

`get_scene` returns the structure of the plate and no geometry at all: the printable area, and one
node per volume per instance. Each node carries the transform that places it, the key of the mesh
it uses, and that mesh's triangle count. It is cheap no matter how heavy the models are, so a
client can call it on a timer to notice that something moved.

`get_mesh` returns one volume's triangles.

The alternative — one tool returning the whole plate, geometry included — is a worse fit for every
client that would use it. A client holding six objects and watching the user drag one of them
would re-download all six on every poll. Splitting lets it compare mesh keys against what it has
cached and fetch only what changed, which is the difference between a preview that tracks the
application and one that stutters.

## Mesh keys

A key is `<volume_id>-<16 hex digits>`, where the digits are an FNV-1a hash over the vertex data,
the index data, and the counts of both.

It is opaque: a client compares keys, it does not parse them.

The volume id alone would not do, because a volume keeps its id when its mesh is replaced — the
simplify gizmo, a cut, a reloaded source file — and a client caching on id alone would keep
drawing the old shape. The hash alone would not do either: two volumes that hold identical
geometry, which is what duplicating a part produces, would collide in a cache keyed by what was
asked for rather than by what came back.

The hash covers the index data as well as the vertices because an edit that re-winds or re-splits
faces without moving a point is a real change to what gets drawn.

It is not a cryptographic hash and does not try to be. A client that is lied to here shows a wrong
picture; nothing about slicing or output depends on it.

## Buffer layout

Each buffer in a `get_mesh` result states its own shape:

```json
{ "type": "float32", "components": 3, "count": 1536,
  "encoding": "base64", "byte_length": 18432, "data": "..." }
```

- `positions` — float32 triples, the mesh's own coordinates, millimetres.
- `indices` — int32 triples into the position buffer.

The bytes are raw and little-endian, which every platform this runs on is. They are base64 because
a tool result is JSON text and a few hundred thousand numbers written out in decimal is not a
payload anyone wants to parse.

**No normals are sent.** A renderer has to choose between flat and smooth shading, and the two want
different buffers — flat shading needs the mesh de-indexed. Making that choice on the server would
force it on every client and roughly double the bytes on the wire for the one that disagreed.
Computing normals from the triangles is a short loop at the drawing end.

## Transforms

A node's `transform` is 16 numbers in **column-major** order, mapping the mesh's own coordinates to
plate coordinates. It is the instance transform composed with the volume transform, so a client
sets one matrix per node and is done.

Column-major is what OpenGL, Metal and SceneKit take, and the reason the order is written down in
three places — here, the tool description, and the test — is that a transposed matrix renders
perfectly happily. Nothing errors. The scene is simply wrong, in a way that looks like a modelling
mistake rather than a protocol one.

Geometry is shared between instances deliberately: two instances of one object are two nodes
pointing at one mesh key, so the mesh travels once.

## Decimation

`get_mesh` takes `max_triangles`. Above the budget the mesh goes through quadric edge collapse and
the result reports `simplified: true` alongside `source_triangles`. The default is 0, meaning send
it whole — a tool that quietly approximated by default would be a poor answer for the clients that
want the real thing.

A decimated mesh is for looking at. It is not the geometry that will be sliced, and nothing should
measure it.

`mesh_key` identifies the **source** mesh, not the reduced one. A client that varies its budget has
to include the budget in its own cache key; the server does not know what budget a cached copy was
fetched with.

Asking for a mesh above two million triangles with no budget is refused, with an error that says to
pass one. That is not a resource limit so much as a signal: `get_scene` already said how many
triangles there were, so a request that large is a client that did not read the answer.

## Threading

Tool bodies reach the model through the main-thread dispatcher, and the dispatcher has a timeout
measured in seconds because a client blocked behind a modal dialog should find out rather than
wait. Hashing, decimating and base64-encoding a large mesh can take longer than that, and doing any
of it on the GUI thread would freeze the application for the length of the transfer.

So both tools hold the GUI thread only long enough to copy out ids, names and transforms and to
take a `shared_ptr` to each mesh — refcount bumps, microseconds. Everything expensive happens
afterwards, on the listener's thread.

Holding the `shared_ptr` is also what makes the result coherent. If the user deletes an object
while a mesh is being encoded, the mesh stays alive until the encoding finishes, and the client
receives the plate as it was when it asked rather than a half-built scene or a crash.

## What is not here

The payload carries shape and placement, not appearance: no per-facet paint state, no filament
colours, no support or seam annotations. A client renders plain geometry. The buffer list is a
JSON object rather than a positional array specifically so that a paint attribute can be added
later as another named buffer, which an existing client ignores without noticing.
