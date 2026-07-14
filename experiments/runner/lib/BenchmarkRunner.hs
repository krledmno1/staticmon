module BenchmarkRunner (runBenchmarks) where

import Control.Applicative (liftA2)
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
import Data.Foldable (foldrM)
import Data.Text qualified as T
import Data.Text.Lazy.Encoding qualified as TL
import Data.Text.Lazy.IO qualified as TL
import Data.Vector qualified as V
import EventGenerators
  ( OperatorBenchmark,
    generateLogForBenchmark,
    getBenchName,
  )
import Flags (Flags (..), NestedFlags (..))
import Monitors
  ( monitorName,
    monitors,
    prepareAndBenchmarkMonitor,
    supportsBenchmark,
  )
import Process (mkdir, rm_rf)
import System.FilePath ((</>))
import UnliftIO (MonadIO (liftIO))
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
    out_time :: Double
  }

instance ToNamedRecord MeasurementRow where
  toNamedRecord MeasurementRow {..} =
    namedRecord
      [ "monitor" .= out_monitor,
        "benchmark" .= out_bench_name,
        "repetition" .= out_rep,
        "time" .= out_time
      ]

instance DefaultOrdered MeasurementRow where
  headerOrder _ = V.fromList ["benchmark", "monitor", "repetition", "time"]

runMonitorBenchmark bench builder =
  let name = getBenchName bench
      runSingleBenchmark s f log_f m i builder = do
        t <- runResourceT $ prepareAndBenchmarkMonitor m (s, f, log_f)
        return $
          builder
            <> Csv.encodeNamedRecord
              ( MeasurementRow
                  { out_monitor = monitorName m,
                    out_bench_name = name,
                    out_rep = i,
                    out_time = t
                  }
              )
   in do
        reps <- RD.asks (bf_reps . f_nes_flags)
        outpath <- RD.asks (bf_out . f_nes_flags)
        let benchpath = outpath </> T.unpack name
            log_f = benchpath </> "log"
            sig_f = benchpath </> "sig"
            fo_f = benchpath </> "fo"
         in do
              mkdir benchpath
              liftIO (generateLogForBenchmark log_f sig_f fo_f bench)
              foldrM
                ( \(m, i) builder ->
                    runSingleBenchmark sig_f fo_f log_f m i builder
                )
                builder
                (monitorItPairs reps)
  where
    -- Only benchmark the monitors whose fragment covers this formula; a formula
    -- outside the common fragment is thus compared on just the supporting subset.
    monitorItPairs reps =
      liftA2 (,) (filter (`supportsBenchmark` bench) monitors) [0 .. (reps - 1)]

runBenchmarks' = do
  outpath <- RD.asks (bf_out . f_nes_flags)
  rm_rf outpath
  mkdir outpath
  confpath <- RD.asks (bf_config . f_nes_flags)
  econfs <- liftIO $ eitherDecodeFileStrict' confpath
  case econfs of
    Left err -> error err
    Right (OperatorBenchmarks confs) ->
      foldrM runMonitorBenchmark mempty confs
        >>= (liftIO . TL.writeFile (outpath </> "out.csv"))
          . TL.decodeUtf8
          . Csv.encodeDefaultOrderedByName

runBenchmarks :: Flags -> IO ()
runBenchmarks =
  RD.runReaderT runBenchmarks'
