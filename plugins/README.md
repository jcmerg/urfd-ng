# Plugins

Protocols that urfd loads from shared objects at start, see **Plugins** in the
[README](../README.md#plugins) and [reflector/Plugin.h](../reflector/Plugin.h).

A plugin's source goes into a directory of its own here, `plugins/<name>/`,
with a `Makefile` that builds the shared object against the urfd sources given
in `URFD=` (the path of `reflector/`, with its `urfd.mk`):

```sh
make -C plugins/<name> URFD=$PWD/reflector
```

The Docker image builds every `plugins/<name>/` that has a `Makefile` and puts
the shared objects into `/usr/local/lib/urfd/`. Git ignores everything here but
this README, so a plugin from elsewhere can sit in its directory without ending
up in the repository. `urfd.ini` loads a plugin with

```ini
[Plugins]
Load = /usr/local/lib/urfd/<name>.so
```
