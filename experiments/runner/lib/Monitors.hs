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
import Flags (Flags (..))
import Process
import System.Clock (Clock (Monotonic), diffTimeSpec, getTime, toNanoSecs)
import System.FilePath ((</>))
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
monitors = []

monpoly = Monitor {..}
  where
    prepareMonitor _ _ = return (Right ())
    runBenchmark _ = benchmarkMonpoly False
    runMonitor _ = monitorMonpoly False
    monitorName = "monpoly"

verimon = Monitor {..}
  where
    prepareMonitor _ _ = return (Right ())
    runBenchmark _ = benchmarkMonpoly True
    runMonitor _ = monitorMonpoly True
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
    monitorName = "cppmon"

staticmon = Monitor {..}
  where
    prepareMonitor s f = do
      basedir <- RD.asks f_mon_path
      b <- RD.asks f_build_dir
      let header_dir = basedir </> "src" </> "staticmon" </> "input_formula"
          builddir = basedir </> T.unpack b
          staticmonCompile = builddir </> "bin" </> "staticmon_compile"
      -- native front-end: build it, generate the formula headers, then build
      -- the specialized monitor (no MonPoly dependency).
      runKeep "ninja" ["-C", builddir, "bin/staticmon_compile"] >>= \case
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
