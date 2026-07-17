library(ggplot2)
library(readr)
library(dplyr)
library(xtable)
library(tidyr)

# Each section plots one prepared CSV (prepare_results.py) and is skipped when
# that CSV is absent, so the script works on partial results directories (e.g.
# an FRZ-only benchmark run).

make_grid_plot <- function(data, xvar, rvar, colvar) {
  dodge_text = position_dodge(width = 0.9)
  p <- ggplot(data, aes(xvar, time, fill=monitor)) +
    geom_col(width = 0.8, position = dodge_text) +
    facet_grid(formula(paste0(rvar, " ~ ", colvar))) +
    geom_text(
      aes(label = time),
      size = 2.5,
      vjust = -0.2,
      position = dodge_text,
    ) +
    ylab("Duration (s)") +
    xlab("Monitor")
  return(p)
}

make_wrap_plot <- function(data, xvar, wvar) {
  dodge_text = position_dodge(width = 0.9)
  p <- ggplot(data, aes(xvar, time, fill=monitor)) +
    geom_text(
      aes(label = time),
      size = 2.5,
      vjust = -0.2,
      position = dodge_text,
    ) +
    geom_col(width = 0.8, position = dodge_text) +
    facet_wrap(formula(paste0(". ~ ", wvar))) +
    ylab("Duration (s)") +
    xlab("Monitor")
}

if (file.exists("and_common_data.csv")) {
  and_common_data <- read_csv("and_common_data.csv")
  p1_data <- and_common_data %>% filter(lsize == 300, rsize == 300, n1 == 10, n2 == 10, monitor != "verimon")
  print(make_wrap_plot(p1_data, p1_data$monitor, "nc"))
  p2_data <- and_common_data %>% filter(lsize == 300, rsize == 300, nc == 1, monitor != "verimon", n1 != 3, n1 != 10)
  print(make_grid_plot(p2_data, p2_data$monitor, "n1", "n2"))
  p3_data <- and_common_data %>% filter(nc == 1, n1 == 3, n2 == 3, monitor != "verimon")
  print(make_grid_plot(p3_data, p3_data$monitor, "lsize", "rsize"))
}

if (file.exists("and_cartesian_data.csv")) {
  and_distinct_data <- read_csv("and_cartesian_data.csv")
  p4_data <- and_distinct_data %>% filter(lsize == 300, rsize == 300, monitor != "verimon", n1 != 3)
  print(make_grid_plot(p4_data, p4_data$monitor, "n1", "n2"))
  p5_data <- and_distinct_data %>% filter(n1 == 3, n2 == 3, monitor != "verimon")
  print(make_grid_plot(p5_data, p5_data$monitor, "lsize", "rsize"))
}

if (file.exists("or_data.csv")) {
  or_data <- read_csv("or_data.csv")
  p1_data <- or_data %>% filter(nvars == 10)
  print(make_grid_plot(p1_data, p1_data$order, "lsize", "rsize"))
  p2_data <- or_data %>% filter(lsize == 1000, rsize == 1000)
  print(make_wrap_plot(p2_data, p2_data$order, "nvars"))
}

if (file.exists("exists_data.csv")) {
  exists_data <- read_csv("exists_data.csv")
  print(make_wrap_plot(exists_data, exists_data$monitor, "size"))
}

if (file.exists("once_data.csv")) {
  once_data <- read_csv("once_data.csv")
  p1_data <- once_data %>% filter(evr == 100, numts == 200, lbound == 0, nvars == 1)
  print(make_wrap_plot(p1_data, p1_data$monitor, "ubound"))
  p2_data <- once_data %>% filter(lbound == 0, ubound == 10, nvars == 1)
  print(make_wrap_plot(p2_data, p2_data$monitor, "numts + evr"))
  p3_data <- once_data %>% filter(ubound == Inf)
  print(make_wrap_plot(p3_data, p3_data$monitor, "numts + evr"))
  p4_data <- once_data %>% filter(evr == 100, nvars == 1, lbound == ubound)
  print(make_wrap_plot(p4_data, p4_data$monitor, "lbound"))
  p5_data <- once_data %>% filter(evr == 100, numts == 200, lbound == 0, ubound == 10)
  print(make_wrap_plot(p5_data, p5_data$monitor, "nvars"))
}

if (file.exists("eventually_data.csv")) {
  eventually_data <- read_csv("eventually_data.csv")
  p1_data <- eventually_data %>% filter(evr == 100, numts == 200, lbound == 0, nvars == 1)
  print(make_wrap_plot(p1_data, p1_data$monitor, "ubound"))
  p2_data <- eventually_data %>% filter(lbound == 0, ubound == 10, nvars == 1)
  print(make_wrap_plot(p2_data, p2_data$monitor, "numts + evr"))
  p4_data <- eventually_data %>% filter(evr == 100, nvars == 1, lbound == ubound)
  print(make_wrap_plot(p4_data, p4_data$monitor, "lbound"))
  p5_data <- eventually_data %>% filter(evr == 100, numts == 200, lbound == 0, ubound == 10)
  print(make_wrap_plot(p5_data, p5_data$monitor, "nvars"))
}

if (file.exists("since_random_data.csv")) {
  since_rand_data <- read_csv("since_random_data.csv")
  p1_data <- since_rand_data %>% filter(evr == 100, numts == 200, n1 == 1, n2 == 3, prob == 0.001, lbound == 0)
  print(make_wrap_plot(p1_data, p1_data$monitor, "ubound"))
  p2_data <- since_rand_data %>% filter(evr == 100, numts == 200, n1 == 1, n2 == 3, prob == 0.999, lbound == 0)
  print(make_wrap_plot(p2_data, p2_data$monitor, "ubound"))
  p3_data <- since_rand_data %>% filter(n1 == 1, n2 == 3, prob == 0.001, lbound == 0, ubound == 10)
  print(make_wrap_plot(p3_data, p3_data$monitor, "evr + numts"))
  p4_data <- since_rand_data %>% filter(n1 == 1, n2 == 3, prob == 0.999, lbound == 0, ubound == 10)
  print(make_wrap_plot(p4_data, p4_data$monitor, "evr + numts"))

  p5_data <- since_rand_data %>% filter(evr == 100, numts == 200, n1 == 1, n2 == 3, lbound == ubound, prob == 0.001)
  print(make_wrap_plot(p5_data, p5_data$monitor, "lbound"))
  p6_data <- since_rand_data %>% filter(evr == 100, numts == 200, n1 == 1, n2 == 3, lbound == ubound, prob == 0.999)
  print(make_wrap_plot(p6_data, p6_data$monitor, "lbound"))
  p7_data <- since_rand_data %>% filter(evr == 100, numts == 200, n2 == 10, lbound == 0, ubound == 10, prob == 0.001)
  print(make_wrap_plot(p7_data, p7_data$monitor, "n1"))
  p8_data <- since_rand_data %>% filter(evr == 100, numts == 200, n2 == 10, lbound == 0, ubound == 10, prob == 0.999)
  print(make_wrap_plot(p8_data, p8_data$monitor, "n1"))
  p9_data <- since_rand_data %>% filter(evr == 100, numts == 200, n1 == 1, n2 == 3, lbound == 0, ubound == 10, neg == FALSE)
  print(make_wrap_plot(p9_data, p9_data$monitor, "prob"))
  # p10_data <- since_rand_data %>% filter(evr == 100, numts == 200, n1 == 1, n2 == 3, lbound == 0, ubound == 10, neg == TRUE)
  # print(make_wrap_plot(p10_data, p10_data$monitor, "prob"))
}

# --- FRZ (freeze operator): staticmon vs monpoly vs verimon -----------------
# Unlike the sections above, timed-out runs matter here (verimon and the nested
# shape hit the timeout): a timed-out/disqualified run is drawn as a bar at the
# recorded ceiling labelled "TO"/"dq" instead of being silently dropped.
if (file.exists("frz_data.csv")) {
  frz_data <- read_csv("frz_data.csv") %>%
    mutate(lab = case_when(
      status == "ok" ~ sprintf("%.2f", time),
      status == "timeout" ~ "TO",
      TRUE ~ "dq"
    ))

  make_frz_plot <- function(data, rvar, colvar) {
    dodge = position_dodge(width = 0.9)
    ggplot(data, aes(monitor, time, fill = monitor)) +
      geom_col(width = 0.8, position = dodge) +
      facet_grid(formula(paste0(rvar, " ~ ", colvar))) +
      geom_text(aes(label = lab), size = 2.5, vjust = -0.2, position = dodge) +
      ylab("Duration (s)") +
      xlab("Monitor") +
      theme(axis.text.x = element_text(angle = 45, hjust = 1))
  }

  # Overview: every body shape with the bounded [0,10] window, scaling in numts.
  # Body shapes map to staticmon's runtime modes: current = the LET-equivalent
  # path; once/since = per-instance replay with a bounded window; eventually =
  # persistent future instances; nested = a freeze inside each outer instance.
  p1_data <- frz_data %>% filter(ubound != Inf)
  print(make_frz_plot(p1_data, "body", "numts"))

  # Bounded window vs whole-prefix replay: ONCE[0,10] against ONCE[0,*).
  p2_data <- frz_data %>% filter(body == "once")
  print(make_frz_plot(p2_data, "ubound", "numts"))

  # Log-scale scaling curves: time vs. log size, one panel per body shape
  # (completed runs only; the bar plots above carry the TO/dq marks).
  p3_data <- frz_data %>%
    filter(status == "ok") %>%
    mutate(shape = ifelse(ubound == Inf, paste0(body, "[0,*)"), body))
  p3 <- ggplot(p3_data, aes(numts, time, color = monitor)) +
    geom_line() +
    geom_point(size = 1) +
    facet_wrap(. ~ shape) +
    scale_x_log10() +
    scale_y_log10() +
    ylab("Duration (s, log)") +
    xlab("Log size (time-points, log)")
  print(p3)
}
