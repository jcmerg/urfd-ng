# Codec plugins for the tcd Docker image

`docker/Dockerfile.tcd` builds every `tcd/plugins/<name>/` that has a
`Makefile` with `make TCD=/build/tcd` and puts the shared objects into
`/usr/local/lib/tcd/`. Git ignores everything here but this README, so a
plugin from elsewhere can sit in its directory without ending up in the
repository. For example, after reading its patent notice:

```sh
git clone https://github.com/jcmerg/tcd-mbelib-neo-plugin.git tcd/plugins/mbelib-neo
```

See "Codec plugins" and "tcd in Docker" in [tcd/README.md](../README.md).
