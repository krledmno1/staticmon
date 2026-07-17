module Monitors
  ( monitors,
    monpoly,
    verimon,
    staticmon,
    prepareAndRunMonitor,
    prepareAndBenchmarkMonitor,
    benchTimedOut,
    verifyMonitor,
    VerificationFailure (..),
    Monitor (..),
  )
where

import Control.Exception (Exception)
import Control.Monad.Reader (ReaderT)
import Control.Monad.Reader qualified as RD
import Control.Monad.Trans.Resource (ResourceT)
import Data.Data (Typeable)
import Data.Functor ((<&>))
import Data.Text qualified as T
import EventGenerators (BenchFeatures (..), OperatorBenchmark, benchFeatures)
import Flags (Flags (..), NestedFlags (..))
import Process
import System.Clock (Clock (Monotonic), diffTimeSpec, getTime, toNanoSecs)
import System.Directory qualified as Dir
import System.FilePath (takeDirectory, (</>))
import UnliftIO (MonadIO (liftIO), throwIO)
import UnliftIO.Timeout (timeout)

default (T.Text)

data VerificationFailure
  = VerificationFailed FilePath
  | VerificationCrash FilePath

data PrepareMonitorFailed = PrepareMonitorFailed deriving (Show, Typeable)

data RunMonitorFailed = RunMonitorFailed deriving (Show, Typeable)

instance Exception PrepareMonitorFailed

instance Exception RunMonitorFailed

type FlagsReader = ReaderT Flags IO

type FlagsResource = ResourceT FlagsReader

data Monitor = forall s.
  Monitor
  { prepareMonitor ::
      FilePath -> -- Sig file
      FilePath -> -- Formula file
      FlagsResource (Either FilePath s), -- stderr or state
    runBenchmark ::
      s -> -- Some state
      FilePath -> -- Sig file
      FilePath -> -- Formula file
      FilePath -> -- Log file
      FlagsResource Double,
    runMonitor ::
      s -> -- Some state
      FilePath -> -- Sig file
      FilePath -> -- Formula file
      FilePath -> -- Log file
      FlagsResource (Either FilePath FilePath), -- stderr or stdout
    -- | Whether this monitor's fragment covers the benchmark's formula. The
    -- benchmark runner only runs a monitor on benchmarks it supports, so a
    -- formula outside the common fragment is compared on just the supporting
    -- subset of monitors.
    supportsBenchmark :: OperatorBenchmark -> Bool,
    monitorName :: T.Text
  }

prepareAndRunMonitor ::
  Monitor ->
  (FilePath, FilePath, FilePath) ->
  FlagsResource (Either FilePath FilePath)
prepareAndRunMonitor Monitor {..} (s, f, l) =
  prepareMonitor s f >>= \case
    Left outerr -> return (Left outerr)
    Right state ->
      runMonitor state s f l >>= \case
        Left outerr -> return (Left outerr)
        Right outf -> return (Right outf)

prepareAndBenchmarkMonitor :: Monitor -> (FilePath, FilePath, FilePath) -> FlagsResource Double
prepareAndBenchmarkMonitor Monitor {..} (s, f, l) = do
  prepareMonitor s f >>= \case
    Left _ -> throwIO PrepareMonitorFailed
    Right state -> runBenchmark state s f l

prepareForVerification Monitor {..} (s, f, l) afail a =
  prepareMonitor s f
    >>= either
      (afail . VerificationCrash)
      ( \state ->
          runMonitor state s f l
            >>= either (afail . VerificationCrash) a
      )

removeMaxTSLine out afail a =
  runDiscard "sed" ["--in-place", "-e", "$ {/MaxTS/ d}", out]
    >>= either (afail . VerificationCrash) a

verifyMonitor ::
  (FilePath, FilePath, FilePath) ->
  FlagsResource (Either VerificationFailure ())
verifyMonitor args =
  prepareForVerification verimon args aErr $ \vmon_out ->
    removeMaxTSLine vmon_out aErr $ \_ ->
      prepareForVerification staticmon args aErr $ \smon_out ->
        runKeep "diff" [vmon_out, smon_out] >>= \case
          Left err -> aErr (VerificationFailed err)
          Right _ -> return $ Right ()
  where
    aErr = return . Left

monitors :: [Monitor]
monitors = [staticmon, monpoly, verimon, timelymon, whymon, dejavu]

monpoly = Monitor {..}
  where
    prepareMonitor _ _ = return (Right ())
    runBenchmark _ = benchmarkMonpoly False
    runMonitor _ = monitorMonpoly False
    supportsBenchmark = const True
    monitorName = "monpoly"

verimon = Monitor {..}
  where
    prepareMonitor _ _ = return (Right ())
    runBenchmark _ = benchmarkMonpoly True
    runMonitor _ = monitorMonpoly True
    supportsBenchmark = const True
    monitorName = "verimon"

cppmon = Monitor {..}
  where
    opts s f l =
      ["--formula", f, "--sig", s, "--log", l]
    prepareMonitor s f =
      runKeep "monpoly" ("-cppmon" : monpolyBaseOpts s f)
    runBenchmark f s _ l =
      benchmark (runDiscard "cppmon" (opts s f l))
    runMonitor f s _ l =
      runKeep "cppmon" (opts s f l)
    supportsBenchmark = const True
    monitorName = "cppmon"

-- --------------------------------------------------------------------------
-- External monitors (dejavu, timelymon, whymon). Their sig/formula/trace are
-- translated from the harness's MonPoly format by experiments/runner/translate.py
-- (never inside a timed region). Tool binaries are the sibling repos of the
-- staticmon prefix; Scala 2.12 (for dejavu) lives in ~/.dejavu-scala.
-- --------------------------------------------------------------------------

runnerScript = RD.asks ((</> "experiments" </> "runner" </> "translate.py") . f_mon_path)

siblingDir sub = RD.asks ((\p -> takeDirectory p </> sub) . f_mon_path)

-- Translate a MonPoly sig/formula/trace file into a tool's format.
xlate tool kind inp outp = do
  sc <- runnerScript
  runDiscard "python3" [sc, "--to", tool, "--kind", kind, inp, outp]

-- Chain two translations (sig then formula), returning the output paths.
prepareTranslate tool s f =
  let s' = s ++ "." ++ tool
      f' = f ++ "." ++ tool
   in xlate tool "sig" s s' >>= \case
        Left e -> return (Left e)
        Right _ ->
          xlate tool "formula" f f' >>= \case
            Left e -> return (Left e)
            Right _ -> return (Right (s', f'))

timelymon = Monitor {..}
  where
    -- MFOTL superset (no regex in the benchmarks), but no freeze operator
    supportsBenchmark b = not (usesFrz (benchFeatures b))
    monitorName = "timelymon"
    prepareMonitor = prepareTranslate "timelymon"
    runBenchmark (s', f') _ _ l = do
      let l' = l ++ ".timelymon"
      _ <- xlate "timelymon" "trace" l l'
      bin <- siblingDir ("timelymon" </> "target" </> "release" </> "timelymon")
      benchmark (runDiscard bin [f', l', "--sig-file", s', "-w", "1", "-m", "3"])
    runMonitor (s', f') _ _ l = do
      let l' = l ++ ".timelymon"
      _ <- xlate "timelymon" "trace" l l'
      bin <- siblingDir ("timelymon" </> "target" </> "release" </> "timelymon")
      runKeep bin [f', l', "--sig-file", s', "-w", "1", "-m", "1"]

whymon = Monitor {..}
  where
    supportsBenchmark b =
      let BenchFeatures {..} = benchFeatures b
       in not (usesAggregation || usesFrz)
    monitorName = "whymon"
    prepareMonitor = prepareTranslate "whymon"
    runBenchmark (s', f') _ _ l = do
      let l' = l ++ ".whymon"
      _ <- xlate "whymon" "trace" l l'
      bin <- siblingDir ("whymon" </> "bin" </> "whymon.exe")
      benchmark
        ( runDiscard
            bin
            ["-sig", s', "-formula", f', "-log", l', "-mode", "unverified", "-measure", "size", "-out", "/dev/null"]
        )
    runMonitor (s', f') _ _ l = do
      let l' = l ++ ".whymon"
      _ <- xlate "whymon" "trace" l l'
      bin <- siblingDir ("whymon" </> "bin" </> "whymon.exe")
      runKeep bin ["-sig", s', "-formula", f', "-log", l', "-mode", "light"]

dejavu = Monitor {..}
  where
    -- past-only, one-sided metric, no future/aggregation/freeze
    supportsBenchmark b =
      let BenchFeatures {..} = benchFeatures b
       in not (usesFuture || usesTwoSidedInterval || usesMetricPrev || usesAggregation || usesFrz)
    monitorName = "dejavu"
    prepareMonitor _ f = do
      let work = takeDirectory f </> "dejavu-work"
      rm_rf work
      mkdir work
      repo <- siblingDir "dejavu"
      scala <- liftIO Dir.getHomeDirectory <&> (</> ".dejavu-scala")
      let jar = repo </> "out" </> "artifacts" </> "dejavu_jar" </> "dejavu.jar"
          compile = repo </> "dejavu-compile"
      xlate "dejavu" "formula" f (work </> "prop.qtl") >>= \case
        Left e -> return (Left e)
        Right _ ->
          -- cache-backed synth (java) + scalac, keyed by spec hash (dejavu-compile
          -- caches the compiled classes under ~/.cache/dejavu), once per formula
          bash
            work
            ([] :: [T.Text])
            ("DEJAVU_SCALA='" ++ scala ++ "' '" ++ compile ++ "' prop.qtl .")
            >>= \case
              Left e -> return (Left e)
              Right _ -> return (Right (work, jar, scala))
    runBenchmark (work, jar, scala) _ _ l = do
      _ <- xlate "dejavu" "trace" l (work </> "trace.timed.csv")
      benchmark
        -- `exec` so a timeout terminates the JVM directly (bash is replaced by it)
        ( bash
            work
            ([] :: [T.Text])
            ("exec '" ++ (scala </> "scala") ++ "' -cp '.:" ++ jar ++ "' TraceMonitor trace.timed.csv 20")
        )
    runMonitor st s f l = runBenchmark st s f l >> return (Right "/dev/null")

staticmon = Monitor {..}
  where
    prepareMonitor s f = do
      basedir <- RD.asks f_mon_path
      b <- RD.asks f_build_dir
      let builddir = basedir </> T.unpack b
          impl = basedir </> "scripts" </> "staticmon-impl"
          headersBin = builddir </> "bin" </> "staticmon-headers"
          outbin = takeDirectory f </> "staticmon.bin"
      -- Compile the per-formula monitor through scripts/staticmon-impl, which
      -- caches the built binary by (build_id, header-hash) under ~/.cache/staticmon
      -- -- so re-running the same formulas is a copy, not a recompile. Same
      -- builddir as before, so the measured binary is unchanged. Build the
      -- front-end first (staticmon-impl requires it).
      runKeep "ninja" ["-C", builddir, "bin/staticmon-headers"] >>= \case
        Left err -> return (Left err)
        Right _ ->
          bash
            basedir
            ([] :: [T.Text])
            ( "STATICMON_BUILDDIR='" ++ builddir ++ "' STATICMON_HEADERS='" ++ headersBin
                ++ "' '" ++ impl ++ "' compile -sig '" ++ s ++ "' -formula '" ++ f
                ++ "' -keep '" ++ outbin ++ "' -quiet"
            )
            >>= \case
              Left err -> return (Left err)
              Right _ ->
                -- Untimed warm-up on the freshly copied binary: macOS validates
                -- a new executable inode's code signature on its FIRST exec
                -- (~0.2s, cached per inode afterwards), which would otherwise
                -- be billed to the first timed run. An empty trace exercises
                -- exec + dyld + the validation without doing monitoring work.
                runDiscard outbin ["--log", "/dev/null"] >>= \case
                  Left err -> return (Left err)
                  Right _ -> return (Right outbin)

    runBenchmark exe _ _ l =
      benchmark (runDiscard exe ["--log", l])
    runMonitor exe _ _ l =
      runKeep exe ["--log", l]

    supportsBenchmark = const True
    monitorName = "staticmon"

monpolyBaseOpts s f = ["-formula", f, "-sig", s, "-no_rw", "-nofilteremptytp"]

monitorMonpoly v s f l =
  runKeep "monpoly" (opts ++ monpolyBaseOpts s f)
  where
    opts = (["-verified" | v]) ++ ["-log", l]

toSecsDouble = (/ 1e9) . fromInteger . toNanoSecs

-- | A negative measured time signals the run was killed by the timeout.
benchTimedOut :: Double -> Bool
benchTimedOut = (< 0)

benchmark a1 = do
  secs <- RD.asks (bf_timeout . f_nes_flags)
  t0 <- liftIO $ getTime Monotonic
  (if secs > 0 then timeout (secs * 1000000) a1 else Just <$> a1) >>= \case
    Nothing -> return (-1) -- timed out; the process was terminated
    Just (Left _) -> error "benchmark failed"
    Just (Right _) ->
      liftIO (getTime Monotonic)
        <&> toSecsDouble . (`diffTimeSpec` t0)

benchmarkMonpoly v s f l = benchmark (runMonpoly opts s f)
  where
    opts = (["-verified" | v]) ++ ["-log", l]

runMonpoly opts s f =
  runDiscard "monpoly" (opts ++ monpolyBaseOpts s f)
