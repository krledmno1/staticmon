# monitor oracle

The reference monitor is **VeriMon** = `monpoly -verified`. It is external — the
repo has no monpoly dependency:
- fixture method: invoked offline by `../../methods/fixture/regen-*.sh` to freeze
  `expected` verdicts into the case-sets.
- live method: a user-provided binary (`$STATICMON_VERIMON`) run live by
  `../../methods/live/run.py`.
