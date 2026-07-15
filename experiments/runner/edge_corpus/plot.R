#!/usr/bin/env Rscript
# Plot the edge-corpus staticmon-vs-monpoly benchmark: one small-multiple panel
# per formula, run time (s) vs trace size (events, log x), staticmon vs monpoly.
# Timed-out points are drawn as an open X at the 3s cap; a dashed line marks the
# cap. Base R only (no ggplot2/dplyr needed).
#
# Usage: Rscript plot.R [results.csv] [out.pdf] [timeout_s]

args <- commandArgs(trailingOnly = TRUE)
csv  <- if (length(args) >= 1) args[1] else "results.csv"
outf <- if (length(args) >= 2) args[2] else "edge_plots.pdf"
tout <- if (length(args) >= 3) as.numeric(args[3]) else 3

d <- read.csv(csv, stringsAsFactors = FALSE)
mm <- regmatches(d$benchmark, regexec("^(f[0-9]+)_.*__e([0-9]+)$", d$benchmark))
d$fid    <- vapply(mm, function(x) if (length(x) == 3) x[2] else NA_character_, "")
d$events <- as.numeric(vapply(mm, function(x) if (length(x) == 3) x[3] else NA_character_, ""))
d <- d[!is.na(d$fid), ]

# readable formula labels, in file order, mapped to f00, f01, ...
labs <- trimws(readLines(file.path(dirname(csv), "formulas")))
labs <- labs[labs != "" & !startsWith(labs, "#")]
lab_of <- function(fid) {
  i <- as.integer(sub("^f", "", fid)) + 1L
  if (i >= 1 && i <= length(labs)) labs[i] else fid
}

# median of the successful reps per (fid, events, monitor); a group with no ok
# rep is a timeout (plotted at the cap).
key <- paste(d$fid, d$events, d$monitor, sep = "|")
agg <- do.call(rbind, lapply(split(d, key), function(g) {
  ok <- g[g$status == "ok", ]
  data.frame(fid = g$fid[1], events = g$events[1], monitor = g$monitor[1],
             time = if (nrow(ok)) median(ok$time) else tout,
             ok = nrow(ok) > 0, stringsAsFactors = FALSE)
}))

cols  <- c(staticmon = "#1b6ca8", monpoly = "#d1495b")
fids  <- unique(agg$fid)
fids  <- fids[order(as.integer(sub("^f", "", fids)))]

if (grepl("\\.png$", outf, ignore.case = TRUE)) {
  png(outf, width = 1700, height = 1150, res = 110)
} else {
  pdf(outf, width = 16, height = 11)
}
op <- par(mfrow = c(5, 6), mar = c(3.2, 3.2, 2.6, 0.8), mgp = c(1.9, 0.6, 0),
          cex.main = 0.82, cex.axis = 0.75, cex.lab = 0.8)
for (fid in fids) {
  s <- agg[agg$fid == fid, ]
  ymax <- max(c(s$time, tout)) * 1.08
  plot(NA, xlim = range(s$events), ylim = c(0, ymax), log = "x",
       xlab = "events", ylab = "time (s)",
       main = paste(strwrap(lab_of(fid), width = 40), collapse = "\n"))
  abline(h = tout, lty = 3, col = "grey55")   # 3s timeout cap
  for (mon in names(cols)) {
    sm <- s[s$monitor == mon, ]
    sm <- sm[order(sm$events), ]
    if (!nrow(sm)) next
    lines(sm$events, sm$time, col = cols[mon], lwd = 1.6)
    points(sm$events, sm$time, col = cols[mon], lwd = 1.6,
           pch = ifelse(sm$ok, 19, 4), cex = ifelse(sm$ok, 1.0, 1.2))
  }
}
# legend in the spare panel(s)
plot.new()
legend("center", bty = "n", title = "monitor",
       legend = c("staticmon", "monpoly", "timed out (>cap)", "3s cap"),
       col = c(cols["staticmon"], cols["monpoly"], "grey30", "grey55"),
       pch = c(19, 19, 4, NA), lty = c(1, 1, NA, 3), lwd = 1.6, cex = 1.0)
par(op)
invisible(dev.off())
cat(sprintf("wrote %s (%d formulas)\n", outf, length(fids)))
