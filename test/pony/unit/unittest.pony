use ponytest = "ponytest"
use base64 = "encode/base64"

actor Main
  new create(env: Env) =>
    ponytest.Main(env)
    base64.Main(env)
