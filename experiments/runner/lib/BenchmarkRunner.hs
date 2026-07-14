module BenchmarkRunner (runBenchmarks) where

import Control.Applicative (liftA2)
import Control.Monad (foldM)
import Control.Monad.Reader qualified as RD
import Data.Aeson
  ( Options (constructorTagModifier, sumEncoding),
    SumEncoding (ObjectWithSingleField),
    defaultOptions,
    eitherDecodeFileStrict',
  )
import Data.Aeson.TH (deriveJSON)
import Data.Char (toLower)
import Data.Csv
  ( DefaultOrdered (..),
    ToNamedRecord (..),
    namedRecord,
    (.=),
  )
import Data.Csv.Incremental qualified as Csv
import Data.IORef (modifyIORef', newIORef, readIORef)
import Data.List (sortOn)
import Data.Set qualified as Set
import Data.Text qualified as T
import Data.Text.IO qualified as T
import Data.Text.Lazy.Encoding qualified as TL
import Data.Text.Lazy.IO qualified as TL
import Data.Vector qualified as V
import EventGenerators
  ( OperatorBenchmark,
    benchCost,
    generateLogForBenchmark,
    getBenchName,
  )
import Flags (Flags (..), NestedFlags (..))
import Monitors
  ( benchTimedOut,
    monitorName,
    monitors,
    prepareAndBenchmarkMonitor,
    supportsBenchmark,
  )
import Process (mkdir, rm_rf)
import System.FilePath ((</>))
import UnliftIO (MonadIO (liftIO), SomeException, try)
import UnliftIO.Resource (runResourceT)

newtype OperatorBenchmarks = OperatorBenchmarks [OperatorBenchmark] deriving (Show)

$( deriveJSON
     defaultOptions
       { constructorTagModifier = map toLower,
         sumEncoding = ObjectWithSingleField
       }
     ''OperatorBenchmarks
 )

data MeasurementRow = MeasurementRow
  { out_monitor :: T.Text,
    out_bench_name :: T.Text,
    out_rep :: Int,
    out_time :: Double,
    -- ok | timeout (killed at the timeout) | disqualified (timed out earlier on
    -- a smaller log of the same formula, so skipped) | error (crashed)
    out_status :: T.Text
  }

instance ToNamedRecord MeasurementRow where
  toNamedRecord MeasurementRow {..} =
    namedRecord
      [ "monitor" .= out_monitor,
        "benchmark" .= out_bench_name,
        "repetition" .= out_rep,
        "time" .= out_time,
        "status" .= out_status
      ]

instance DefaultOrdered MeasurementRow where
  headerOrder _ = V.fromList ["benchmark", "monitor", "repetition", "time", "status"]

-- | The disqualification set: (monitor, formula) pairs where the monitor has
-- already exceeded the timeout, so it is skipped on any larger log of that same
-- formula.
type Disq = Set.Set (T.Text, T.Text)

runMonitorBenchmark disqRef builder bench = do
  reps <- RD.asks (bf_reps . f_nes_flags)
  tout <- RD.asks (bf_timeout . f_nes_flags)
  outpath <- RD.asks (bf_out . f_nes_flags)
  let name = getBenchName bench
      benchpath = outpath </> T.unpack name
      log_f = benchpath </> "log"
      sig_f = benchpath </> "sig"
      fo_f = benchpath </> "fo"
  mkdir benchpath
  liftIO (generateLogForBenchmark log_f sig_f fo_f bench)
  -- The formula text is the disqualification key: benchmarks that differ only by
  -- log size share it, so a timeout on the small one skips the larger ones.
  fkey <- liftIO (T.strip <$> T.readFile fo_f)
  sel <- RD.asks (bf_monitors . f_nes_flags)
  let selected m = null sel || monitorName m `elem` sel
      mons = filter (\m -> selected m && supportsBenchmark m bench) monitors
      pairs = liftA2 (,) mons [0 .. reps - 1]
      runOne b (m, i) = do
        let mname = monitorName m
            key = (mname, fkey)
            row t st =
              b
                <> Csv.encodeNamedRecord
                  (MeasurementRow mname name i t st)
        disq <- liftIO (readIORef disqRef)
        if Set.member key disq
          then return (row (fromIntegral tout) "disqualified")
          else do
            res <- try (runResourceT $ prepareAndBenchmarkMonitor m (sig_f, fo_f, log_f))
            case res of
              Left (_ :: SomeException) -> do
                liftIO (modifyIORef' disqRef (Set.insert key))
                return (row (fromIntegral tout) "error")
              Right t
                | benchTimedOut t -> do
                    liftIO (modifyIORef' disqRef (Set.insert key))
                    return (row (fromIntegral tout) "timeout")
                | otherwise -> return (row t "ok")
  foldM runOne builder pairs

runBenchmarks' = do
  outpath <- RD.asks (bf_out . f_nes_flags)
  rm_rf outpath
  mkdir outpath
  confpath <- RD.asks (bf_config . f_nes_flags)
  econfs <- liftIO $ eitherDecodeFileStrict' confpath
  case econfs of
    Left err -> error err
    Right (OperatorBenchmarks confs) -> do
      disqRef <- liftIO (newIORef (Set.empty :: Disq))
      -- Smallest log first so a timeout disqualifies the monitor from the bigger
      -- logs of the same formula rather than the other way around.
      foldM (runMonitorBenchmark disqRef) mempty (sortOn benchCost confs)
        >>= (liftIO . TL.writeFile (outpath </> "out.csv"))
          . TL.decodeUtf8
          . Csv.encodeDefaultOrderedByName

runBenchmarks :: Flags -> IO ()
runBenchmarks =
  RD.runReaderT runBenchmarks'
