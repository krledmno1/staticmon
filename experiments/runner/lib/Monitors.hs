module Monitors
  ( monitors,
    monpoly,
    verimon,
    staticmon,
    prepareAndRunMonitor,
    prepareAndBenchmarkMonitor,
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
import Flags (Flags (..))
import Process
import System.Clock (Clock (Monotonic), diffTimeSpec, getTime, toNanoSecs)
import System.Directory qualified as Dir
import System.FilePath (takeDirectory, (</>))
import UnliftIO (MonadIO (liftIO), throwIO)

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
monitors = [staticmon, monpoly, timelymon, whymon, dejavu]

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
    supportsBenchmark = const True -- MFOTL superset (no regex in the benchmarks)
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
    supportsBenchmark b = not (usesAggregation (benchFeatures b))
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
    -- past-only, one-sided metric, no future/aggregation
    supportsBenchmark b =
      let BenchFeatures {..} = benchFeatures b
       in not (usesFuture || usesTwoSidedInterval || usesMetricPrev || usesAggregation)
    monitorName = "dejavu"
    prepareMonitor _ f = do
      let work = takeDirectory f </> "dejavu-work"
      rm_rf work
      mkdir work
      jar <- siblingDir ("dejavu" </> "out" </> "artifacts" </> "dejavu_jar" </> "dejavu.jar")
      scala <- liftIO Dir.getHomeDirectory <&> (</> ".dejavu-scala")
      xlate "dejavu" "formula" f (work </> "prop.qtl") >>= \case
        Left e -> return (Left e)
        Right _ ->
          -- synthesize the monitor (java) then compile it (scalac 2.12), once per formula
          bash
            work
            ([] :: [T.Text])
            ( "java -cp '" ++ jar ++ "' dejavu.Verify prop.qtl && '"
                ++ (scala </> "scalac")
                ++ "' -cp '"
                ++ jar
                ++ "' TraceMonitor.scala"
            )
            >>= \case
              Left e -> return (Left e)
              Right _ -> return (Right (work, jar, scala))
    runBenchmark (work, jar, scala) _ _ l = do
      _ <- xlate "dejavu" "trace" l (work </> "trace.timed.csv")
      benchmark
        ( bash
            work
            ([] :: [T.Text])
            ("'" ++ (scala </> "scala") ++ "' -cp '.:" ++ jar ++ "' TraceMonitor trace.timed.csv 20")
        )
    runMonitor st s f l = runBenchmark st s f l >> return (Right "/dev/null")

staticmon = Monitor {..}
  where
    prepareMonitor s f = do
      basedir <- RD.asks f_mon_path
      b <- RD.asks f_build_dir
      let header_dir = basedir </> "src" </> "staticmon" </> "input_formula"
          builddir = basedir </> T.unpack b
          staticmonCompile = builddir </> "bin" </> "staticmon-headers"
      -- native front-end: build it, generate the formula headers, then build
      -- the specialized monitor (no MonPoly dependency).
      runKeep "ninja" ["-C", builddir, "bin/staticmon-headers"] >>= \case
        Left err -> return $ Left err
        Right _ ->
          runDiscard staticmonCompile
            ["-sig", s, "-formula", f, "-prefix", header_dir] >>= \case
            Left err -> return $ Left err
            Right _ ->
              runKeep "ninja" ["-C", builddir] >>= \case
                Left err -> return $ Left err
                Right _ -> return $ Right (builddir </> "bin" </> "staticmon")

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

benchmark a1 = do
  t0 <- liftIO $ getTime Monotonic
  a1 >>= \case
    Left _ -> error "benchmark failed"
    Right _ -> do
      liftIO (getTime Monotonic)
        <&> toSecsDouble . (`diffTimeSpec` t0)

benchmarkMonpoly v s f l = benchmark (runMonpoly opts s f)
  where
    opts = (["-verified" | v]) ++ ["-log", l]

runMonpoly opts s f =
  runDiscard "monpoly" (opts ++ monpolyBaseOpts s f)
