# frontend oracle

The reference for typing (`-sigout`) and monitorability (`-check`) is **MonPoly**
itself (`monpoly -sigout` / `monpoly -check`). It is external — not vendored — and
is invoked only offline by `../../methods/fixture/regen.sh` to (re)generate the
frozen `expected.tsv`. Nothing here runs at test time.
