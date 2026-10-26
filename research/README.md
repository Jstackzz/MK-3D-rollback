# research: per-build reverse-engineering artifacts

One folder per XBE build, named by our short game id, keyed inside by the
XBE SHA-1 from `xbe_info.json`:

```
research/mkd/
  xbe_info.json          from tools/py/xbe_info.py (certificate, sections, XDK libraries)
  symbols.csv            XDK functions named by xbe_symbols / XbSymbolDatabase
  memory-map.md          what lives where during a fight, with evidence
  addresses.toml         every address we rely on, with the SHA-1 it applies to
  notes.md               running log of findings
```

Present: `mkda-rev1/`, `mkd/`, `mka/` with `xbe_info.json` each.
No extracted game files go here; those live in `orig/` (git-ignored).
