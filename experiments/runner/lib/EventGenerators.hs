{-# LANGUAGE ParallelListComp #-}
{-# OPTIONS_GHC -Wno-unrecognised-pragmas #-}

{-# HLINT ignore "Use second" #-}
{-# HLINT ignore "Use newtype instead of data" #-}

module EventGenerators
  ( generateRandomLog,
    getBenchName,
    OperatorConfig (..),
    OperatorBenchmark (..),
    BenchFeatures (..),
    benchFeatures,
    generateLogForBenchmark,
  )
where

import Control.Applicative (Applicative (liftA2))
import Control.Exception (assert)
import Control.Exception.Extra (assertIO)
import Control.Monad (forM_, replicateM, when)
import Control.Monad.Extra (replicateM_)
import Control.Monad.Reader (ReaderT)
import Control.Monad.Reader qualified as R
import Control.Monad.State
import Data.Aeson
  ( Options (constructorTagModifier, fieldLabelModifier, sumEncoding),
    SumEncoding (ObjectWithSingleField),
    defaultOptions,
  )
import Data.Aeson.TH (deriveJSON)
import Data.Char (toLower)
import Data.Int (Int64)
import Data.List (intersperse, sortBy)
import Data.Map qualified as M
import Data.Random (RVar, shuffle)
import Data.Random.Distribution.Bernoulli (bernoulli)
import Data.Random.List qualified as L
import Data.Random.RVar (runRVar)
import Data.Random.Vector qualified as V
import Data.Sequence (Seq ((:|>)), ViewL ((:<)))
import Data.Sequence qualified as S
import Data.Text qualified as T
import Data.Text.IO qualified as T
import Data.Vector ((!), (!?))
import Data.Vector qualified as V
import EventPrinting
import Flags (Flags (..), NestedFlags (..))
import Fmt (Buildable (build), Builder, fixedF, (+|), (|+))
import SignatureParser (SigType (..), Signature)
import System.IO (IOMode (WriteMode), withFile)
import System.Random.Stateful (globalStdGen, uniformRM)

maxEventIntValue :: Int64 = 100000

data AndOptions
  = Distinct -- No vars in common
  | Subset Bool -- Random subset of common vars, flag indicated if left is subset of right
  | FixedCommon Int -- n random vars in common
  deriving (Show)

$( deriveJSON
     defaultOptions
       { constructorTagModifier = map toLower,
         sumEncoding = ObjectWithSingleField
       }
     ''AndOptions
 )

data AndConfig = AndConfig
  { ac_lsize :: Int,
    ac_rsize :: Int,
    ac_n1 :: Int,
    ac_n2 :: Int,
    ac_opts :: AndOptions
  }
  deriving (Show)

$( deriveJSON
     defaultOptions
       { fieldLabelModifier = drop 3,
         constructorTagModifier = map toLower,
         sumEncoding = ObjectWithSingleField
       }
     ''AndConfig
 )

data OrOptions
  = SameLayout
  | Shuffled
  deriving (Show)

$( deriveJSON
     defaultOptions
       { constructorTagModifier = map toLower,
         sumEncoding = ObjectWithSingleField
       }
     ''OrOptions
 )

data OrConfig = OrConfig
  { or_lsize :: Int,
    or_rsize :: Int,
    or_nvars :: Int,
    or_opts :: OrOptions
  }
  deriving (Show)

$( deriveJSON
     defaultOptions
       { fieldLabelModifier = drop 3,
         constructorTagModifier = map toLower,
         sumEncoding = ObjectWithSingleField
       }
     ''OrConfig
 )

data VarSubsetOptions
  = SubsetSequence (Int, Int) -- Vars in left, right, subset in order
  | RandomSubset (Int, Int) -- Vars in left, right, random subset
  deriving (Show)

binPredicateSubsetVars nphi npsi left_subset =
  assert (if left_subset then nphi <= npsi else npsi <= nphi) $
    let l = [0 .. (varSize - 1)]
        r1 = shuffle l
        r2 = L.shuffleNofM subsetSize varSize l
     in do
          l1 <- runRVar r1 globalStdGen
          l2 <- runRVar r2 globalStdGen
          if left_subset
            then return (l2, l1)
            else return (l1, l2)
  where
    varSize = if left_subset then npsi else nphi
    subsetSize = if left_subset then nphi else npsi

commonVars vars1 vars2 =
  let m1 = M.fromList $ zip vars1 [0 :: Int ..]
      m2 = M.fromList $ zip vars2 [0 :: Int ..]
      mc = M.intersectionWith (,) m1 m2
      ml = map snd $ M.toList mc
      (cidx1, cidx2) = unzip $ sortBy (\k1 k2 -> compare (snd k1) (snd k2)) ml
   in (V.fromList cidx1, V.fromList cidx2)

genSubsetVars (SubsetSequence (n1, n2)) _ =
  return ([0 .. (n1 - 1)], [0 .. (n2 - 1)])
genSubsetVars (RandomSubset (n1, n2)) left_subset =
  binPredicateSubsetVars n1 n2 left_subset

$( deriveJSON
     defaultOptions
       { constructorTagModifier = map toLower,
         sumEncoding = ObjectWithSingleField
       }
     ''VarSubsetOptions
 )

data AntiJoinConfig = AntiJoinConfig
  { aj_lsize :: Int,
    aj_rsize :: Int,
    aj_matchprobability :: Double,
    aj_vars :: VarSubsetOptions
  }
  deriving (Show)

$( deriveJSON
     defaultOptions
       { fieldLabelModifier = drop 3,
         constructorTagModifier = map toLower,
         sumEncoding = ObjectWithSingleField
       }
     ''AntiJoinConfig
 )

data ExistsConfig = ExistsConfig
  { ex_n :: Int,
    ex_predn :: Int,
    ex_size :: Int
  }
  deriving (Show)

$( deriveJSON
     defaultOptions
       { fieldLabelModifier = drop 3,
         constructorTagModifier = map toLower,
         sumEncoding = ObjectWithSingleField
       }
     ''ExistsConfig
 )

data TemporalBound = InfBound | CstBound Int deriving (Show)

$( deriveJSON
     defaultOptions
       { fieldLabelModifier = drop 3,
         constructorTagModifier = map toLower,
         sumEncoding = ObjectWithSingleField
       }
     ''TemporalBound
 )

data PrevConfig = PrevConfig
  { pr_size :: Int
  }
  deriving (Show)

$( deriveJSON
     defaultOptions
       { fieldLabelModifier = drop 3,
         constructorTagModifier = map toLower,
         sumEncoding = ObjectWithSingleField
       }
     ''PrevConfig
 )

data NextConfig = NextConfig
  { nx_size :: Int
  }
  deriving (Show)

$( deriveJSON
     defaultOptions
       { fieldLabelModifier = drop 3,
         constructorTagModifier = map toLower,
         sumEncoding = ObjectWithSingleField
       }
     ''NextConfig
 )

data OnceConfig = OnceConfig
  { oc_eventrate :: Int,
    oc_nvars :: Int
  }
  deriving (Show)

$( deriveJSON
     defaultOptions
       { fieldLabelModifier = drop 3,
         constructorTagModifier = map toLower,
         sumEncoding = ObjectWithSingleField
       }
     ''OnceConfig
 )

data EventuallyConfig = EventuallyConfig
  { ev_eventrate :: Int,
    ev_nvars :: Int
  }
  deriving (Show)

$( deriveJSON
     defaultOptions
       { fieldLabelModifier = drop 3,
         constructorTagModifier = map toLower,
         sumEncoding = ObjectWithSingleField
       }
     ''EventuallyConfig
 )

data SinceConfig = SinceConfig
  { si_eventrate :: Int,
    si_vars :: VarSubsetOptions,
    si_negate :: Bool,
    si_removeprobability :: Double
  }
  deriving (Show)

$( deriveJSON
     defaultOptions
       { fieldLabelModifier = drop 3,
         constructorTagModifier = map toLower,
         sumEncoding = ObjectWithSingleField
       }
     ''SinceConfig
 )

data UntilConfig = UntilConfig
  { ut_eventrate :: Int,
    ut_vars :: VarSubsetOptions,
    ut_negate :: Bool,
    ut_removeprobability :: Double
  }
  deriving (Show)

$( deriveJSON
     defaultOptions
       { fieldLabelModifier = drop 3,
         constructorTagModifier = map toLower,
         sumEncoding = ObjectWithSingleField
       }
     ''UntilConfig
 )

data OnceAndEqConfig = OnceAndEqConfig
  { oa_eventrate :: Int
  }
  deriving (Show)

$( deriveJSON
     defaultOptions
       { fieldLabelModifier = drop 3,
         constructorTagModifier = map toLower,
         sumEncoding = ObjectWithSingleField
       }
     ''OnceAndEqConfig
 )

data SinceUntilConfig = SinceUntilConfig
  { siut_evr :: Int,
    siut_vars :: VarSubsetOptions,
    siut_neg :: Bool,
    siut_rmp :: Double,
    siut_isut :: Bool
  }

data TemporalSubOperator
  = UntilOperator UntilConfig
  | SinceOperator SinceConfig
  | OnceOperator OnceConfig
  | EventuallyOperator EventuallyConfig
  | PrevOperator PrevConfig
  | NextOperator NextConfig
  deriving (Show)

$( deriveJSON
     defaultOptions
       { fieldLabelModifier = drop 3,
         constructorTagModifier = map toLower,
         sumEncoding = ObjectWithSingleField
       }
     ''TemporalSubOperator
 )

data TemporalConfig = TemporalConfig
  { tc_lbound :: TemporalBound,
    tc_ubound :: TemporalBound,
    tc_suboperator :: TemporalSubOperator
  }
  deriving (Show)

$( deriveJSON
     defaultOptions
       { fieldLabelModifier = drop 3,
         constructorTagModifier = map toLower,
         sumEncoding = ObjectWithSingleField
       }
     ''TemporalConfig
 )

data OperatorConfig
  = AndOperator AndConfig
  | OrOperator OrConfig
  | AntiJoinOperator AntiJoinConfig
  | ExistsOperator ExistsConfig
  | TemporalOperator TemporalConfig
  | OnceAndEqOperator OnceAndEqConfig
  deriving (Show)

$( deriveJSON
     defaultOptions
       { fieldLabelModifier = drop 3,
         constructorTagModifier = map toLower,
         sumEncoding = ObjectWithSingleField
       }
     ''OperatorConfig
 )

data OperatorBenchmark = OperatorBenchmark
  { op_numtpperts :: Int,
    op_numts :: Int,
    op_config :: OperatorConfig
  }
  deriving (Show)

$( deriveJSON
     defaultOptions
       { fieldLabelModifier = drop 3,
         constructorTagModifier = map toLower,
         sumEncoding = ObjectWithSingleField
       }
     ''OperatorBenchmark
 )

instance Buildable TemporalBound where
  build InfBound = "inf"
  build (CstBound i) = build i

instance Buildable (TemporalBound, TemporalBound) where
  build (CstBound i1, CstBound i2) = "[" +| i1 |+ "," +| i2 |+ "]"
  build (CstBound i1, InfBound) = "[" +| i1 |+ ", *)"
  build _ = error "not a valid interval"

instance Buildable VarSubsetOptions where
  build (SubsetSequence (nc, n1)) = "seq_" +| nc |+ "_" +| n1 |+ ""
  build (RandomSubset (nc, n1)) = "random_" +| nc |+ "_" +| n1 |+ ""

intervalGeqLower (CstBound i, _) tsDiff = tsDiff >= i
intervalGeqLower (InfBound, _) _ = error "lower bound must be >= 0"

intervalGtUpper (_, CstBound i) tsDiff = tsDiff > i
intervalGtUpper (_, InfBound) _ = False

zeroInInterval (CstBound 0, _) = True
zeroInInterval _ = False

data SlidingWindow = SlidingWindow
  { sl_intv :: (TemporalBound, TemporalBound),
    sl_prewindow :: S.Seq (Int, V.Vector (V.Vector Int64)),
    sl_inwindow :: S.Seq (Int, V.Vector (V.Vector Int64)),
    sl_currts :: Int
  }

initSlidingWindow lbound ubound =
  SlidingWindow
    { sl_intv = (lbound, ubound),
      sl_prewindow = S.empty,
      sl_inwindow = S.empty,
      sl_currts = 0
    }

getInSlidingWindow SlidingWindow {..} = sl_inwindow

advanceSlidingWindow newTs SlidingWindow {..} =
  let prenoold = dropOld sl_prewindow
      innoold = dropOld sl_inwindow
      (premoved, inmoved) = movePreToIn prenoold innoold
   in SlidingWindow
        { sl_intv,
          sl_prewindow = premoved,
          sl_inwindow = inmoved,
          sl_currts = newTs
        }
  where
    dropOld s =
      case S.viewl s of
        (ts, _) :< sl ->
          if intervalGtUpper sl_intv (newTs - ts)
            then dropOld sl
            else s
        S.EmptyL -> s
    movePreToIn prewindow inwindow =
      case S.viewl prewindow of
        h@(ts, _) :< sl ->
          if intervalGeqLower sl_intv ts
            then movePreToIn sl (inwindow :|> h)
            else (prewindow, inwindow)
        S.EmptyL -> (prewindow, inwindow)

killRandomEventsSlidingWindow p SlidingWindow {..} =
  let x :: RVar Bool = bernoulli (1.0 - p)
   in do
        newInWindow <-
          mapM
            ( \(ts, evs) ->
                (ts,) <$> V.filterM (const $ runRVar x globalStdGen) evs
            )
            sl_inwindow
        return SlidingWindow {sl_inwindow = newInWindow, ..}

addEventsSlidingWindow evs SlidingWindow {..} =
  if zeroInInterval sl_intv
    then SlidingWindow {sl_inwindow = sl_inwindow :|> (sl_currts, evs), ..}
    else SlidingWindow {sl_prewindow = sl_prewindow :|> (sl_currts, evs), ..}

-- | Logical features a benchmark's formula uses, so a monitor can decide
-- whether the formula is in its fragment. (Which monitor supports which feature
-- is decided in Monitors.hs.) The current operator microbenchmarks are all
-- first-order past/future MFOTL with no aggregations, so only the temporal
-- shapes vary; DejaVu is the only monitor these features constrain.
data BenchFeatures = BenchFeatures
  { usesFuture :: Bool, -- next / eventually / until
    usesTwoSidedInterval :: Bool, -- [l,u] with l>0 and u finite
    usesMetricPrev :: Bool, -- previous with a non-trivial interval
    usesAggregation :: Bool -- SUM/CNT/... (none in the microbenchmarks)
  }

benchFeatures :: OperatorBenchmark -> BenchFeatures
benchFeatures OperatorBenchmark {..} =
  case op_config of
    TemporalOperator TemporalConfig {..} ->
      case tc_suboperator of
        OnceOperator _ -> base {usesTwoSidedInterval = twoSided tc_lbound tc_ubound}
        SinceOperator _ -> base {usesTwoSidedInterval = twoSided tc_lbound tc_ubound}
        PrevOperator _ -> base {usesMetricPrev = not (untimed tc_lbound tc_ubound)}
        EventuallyOperator _ -> base {usesFuture = True}
        NextOperator _ -> base {usesFuture = True}
        UntilOperator _ -> base {usesFuture = True}
    _ -> base -- And / Or / AntiJoin / Exists / OnceAndEq: pure past FO
  where
    base = BenchFeatures False False False False
    twoSided (CstBound l) (CstBound _) = l > 0
    twoSided _ _ = False
    untimed (CstBound 0) InfBound = True
    untimed _ _ = False

getBenchName :: OperatorBenchmark -> T.Text
getBenchName OperatorBenchmark {..} =
  let opparam :: Builder = "" +| op_numts |+ "_" +| op_numtpperts |+ ""
      opdesc :: Builder =
        case op_config of
          AndOperator AndConfig {..} ->
            let prefix :: Builder = "and_" +| ac_lsize |+ "_" +| ac_rsize |+ ""
                suffix :: Builder = case ac_opts of
                  Distinct ->
                    "cartesian_"
                      +| ac_n1
                      |+ "_"
                      +| ac_n2
                      |+ ""
                  Subset left_subset ->
                    "subset_"
                      +| left_subset
                      |+ "_"
                      +| ac_n1
                      |+ "_"
                      +| ac_n2
                      |+ ""
                  FixedCommon nc ->
                    "common_"
                      +| nc
                      |+ "_"
                      +| ac_n1
                      |+ "_"
                      +| ac_n2
                      |+ ""
             in "" +| prefix |+ "_" +| suffix |+ ""
          OrOperator OrConfig {..} ->
            let prefix :: Builder =
                  "or_"
                    +| or_lsize
                    |+ "_"
                    +| or_rsize
                    |+ ""
                suffix :: Builder = case or_opts of
                  SameLayout -> "same_" +| or_nvars |+ ""
                  Shuffled -> "shuffled_" +| or_nvars |+ ""
             in "" +| prefix |+ "_" +| suffix |+ ""
          AntiJoinOperator AntiJoinConfig {..} ->
            let prefix :: Builder =
                  "and_not_"
                    +| aj_lsize
                    |+ "_"
                    +| aj_rsize
                    |+ ""
             in ""
                  +| prefix
                  |+ "_"
                  +| aj_matchprobability
                  |+ "_"
                  +| aj_vars
                  |+ ""
          ExistsOperator ExistsConfig {..} ->
            "exists_" +| ex_n |+ "_" +| ex_predn |+ "_" +| ex_size |+ ""
          OnceAndEqOperator OnceAndEqConfig {..} ->
            "once_and_not_"
              +| oa_eventrate
              |+ ""
          TemporalOperator TemporalConfig {..} ->
            case tc_suboperator of
              PrevOperator PrevConfig {..} ->
                "prev_"
                  +| tc_lbound
                  |+ "_"
                  +| tc_ubound
                  |+ "_"
                  +| pr_size
                  |+ ""
              NextOperator NextConfig {..} ->
                "next_"
                  +| tc_lbound
                  |+ "_"
                  +| tc_ubound
                  |+ "_"
                  +| nx_size
                  |+ ""
              UntilOperator UntilConfig {..} ->
                "until_"
                  +| tc_lbound
                  |+ "_"
                  +| tc_ubound
                  |+ "_"
                  +| ut_eventrate
                  |+ "_"
                  +| ut_negate
                  |+ "_"
                  +| ut_vars
                  |+ ""
              SinceOperator SinceConfig {..} ->
                "since_"
                  +| tc_lbound
                  |+ "_"
                  +| tc_ubound
                  |+ "_"
                  +| si_eventrate
                  |+ "_"
                  +| si_negate
                  |+ "_"
                  +| fixedF 3 si_removeprobability
                  |+ "_"
                  +| si_vars
                  |+ ""
              OnceOperator OnceConfig {..} ->
                "once_"
                  +| tc_lbound
                  |+ "_"
                  +| tc_ubound
                  |+ "_"
                  +| oc_eventrate
                  |+ "_"
                  +| oc_nvars
                  |+ ""
              EventuallyOperator EventuallyConfig {..} ->
                "eventually_"
                  +| tc_lbound
                  |+ "_"
                  +| tc_ubound
                  |+ "_"
                  +| ev_eventrate
                  |+ "_"
                  +| ev_nvars
                  |+ ""
   in "" +| opparam |+ "_" +| opdesc |+ ""

intArgs = map Intgr

addPredToSig name arity sig_h =
  T.hPutStr sig_h psig
  where
    psig = "" +| name |+ "(" +| intlist |+ ")"
    intlist = (mconcat . intersperse ",") (replicate arity (build "int"))

argsF vars = mconcat . intersperse "," $ map (\v -> "x" +| v |+ "") vars :: Builder

varsToPred name vars =
  " " +| name |+ "(" +| argsF vars |+ ")" :: Builder

randomEvent arity ub =
  intArgs <$> replicateM arity (uniformRM (0, ub) globalStdGen)

randomInt64Vec arity =
  V.replicateM arity (uniformRM (0, maxEventIntValue) globalStdGen)

outputRandomEvents name num arity =
  forM_ [0 .. (num - 1)] $
    const $
      randomInt64Vec arity >>= outputNewEvent name

binPredicateCommonVars :: Int -> Int -> Int -> IO ([Int], [Int])
binPredicateCommonVars nphi npsi nc = do
  assertIO (nc >= npsi && nc >= nphi)
  let l1 = [0 .. (nphi - nc - 1)]
      l2 = [(nphi - nc) .. (nphi - nc + npsi - nc - 1)]
      prefix = [(nphi - nc + npsi - nc) .. (nphi - nc + npsi - 1)]
      r1 = shuffle (prefix ++ l1)
      r2 = shuffle (prefix ++ l2)
  res1 <- runRVar r1 globalStdGen
  res2 <- runRVar r2 globalStdGen
  return (res1, res2)

binPredicateDistinctVars nphi npsi =
  let r1 = shuffle [0 .. (nphi - 1)]
      r2 = shuffle [nphi .. (nphi + npsi - 1)]
   in do
        l1 <- runRVar r1 globalStdGen
        l2 <- runRVar r2 globalStdGen
        return (l1, l2)

projectVec v idxs =
  V.unfoldr
    ( \i -> case idxs !? i of
        Just idx -> Just (v ! idx, i + 1)
        Nothing -> Nothing
    )
    0

genAndCommonTables lsize rsize n1 n2 cidx1 cidx2 = do
  levs <- V.replicateM lsize (randomInt64Vec n1)
  if rsize == 0
    then return (levs, V.empty)
    else do
      revs <- V.replicateM rsize (randomInt64Vec n2)
      let revsproj = V.map (`projectVec` cidx2) revs
          rrvar = V.randomElement revsproj
      levrep <-
        V.mapM
          ( \v -> do
              vproj <- runRVar rrvar globalStdGen
              return $ V.update_ v cidx1 vproj
          )
          levs
      return (levrep, revs)

andBenchTpGen AndConfig {..} cidx1 cidx2 =
  case ac_opts of
    Distinct -> do
      outputRandomEvents "P" ac_lsize ac_n1
      outputRandomEvents "Q" ac_rsize ac_n2
    Subset _ -> commonCase ac_n1 ac_n2
    FixedCommon _ -> commonCase ac_n1 ac_n2
  where
    commonCase n1 n2 = do
      (l, r) <- genAndCommonTables ac_lsize ac_rsize n1 n2 cidx1 cidx2
      V.forM_ l (outputNewEvent "P")
      V.forM_ r (outputNewEvent "Q")

andNotBenchGen log_f sig_f fo_f nts ntp AntiJoinConfig {..} = do
  (vars1, vars2) <- genSubsetVars aj_vars False
  let pred1 = varsToPred "P" vars1
      pred2 = varsToPred "Q" vars2
      n1 = length vars1
      n2 = length vars2
  T.writeFile fo_f ("" +| pred1 |+ " AND (NOT " +| pred2 |+ ")")
  withFile
    sig_f
    WriteMode
    ( \sig_h -> do
        addPredToSig "P" n1 sig_h
        addPredToSig "Q" n2 sig_h
    )
  let numLeftMatching :: Int = round (aj_matchprobability * fromIntegral aj_lsize)
      (cidx1, cidx2) = commonVars vars1 vars2
  withPrintState log_f $ do
    forM_ [0 .. nts] $ \_ ->
      forM_ [0 .. (ntp - 1)] $ \_ -> do
        (lmatchevs, revs) <- genAndCommonTables numLeftMatching aj_rsize n1 n2 cidx1 cidx2
        V.forM_ lmatchevs (outputNewEvent "P")
        V.forM_ revs (outputNewEvent "Q")
    endOutput

andBenchGen log_f sig_f fo_f nts ntp conf@AndConfig {..} =
  do
    withFile
      sig_f
      WriteMode
      ( \sig_h -> do
          addPredToSig "P" ac_n1 sig_h
          addPredToSig "Q" ac_n2 sig_h
      )
    (phivars, psivars) <- case ac_opts of
      Distinct -> liftIO $ binPredicateDistinctVars ac_n1 ac_n2
      Subset left_subset -> liftIO $ binPredicateSubsetVars ac_n1 ac_n2 left_subset
      FixedCommon nc -> liftIO $ binPredicateCommonVars ac_n1 ac_n2 nc
    let ppred :: Builder = varsToPred "P" phivars
        qpred :: Builder = varsToPred "Q" psivars
    T.writeFile fo_f ("" +| ppred |+ " AND " +| qpred |+ "\n")
    let (cidx1, cidx2) = commonVars phivars psivars
    withPrintState log_f $ do
      forM_ [0 .. nts] $ \i ->
        forM_ [0 .. (ntp - 1)] $ \_ -> do
          newDb i
          andBenchTpGen conf cidx1 cidx2
      endOutput

orBenchGen log_f sig_f fo_f nts ntp OrConfig {..} =
  let n = or_nvars
   in do
        withFile
          sig_f
          WriteMode
          ( \sig_h -> do
              addPredToSig "P" n sig_h
              addPredToSig "Q" n sig_h
          )
        (phivars, psivars) <- case or_opts of
          SameLayout -> return ([0 :: Int .. (n - 1)], [0 .. (n - 1)])
          Shuffled ->
            let rvar = shuffle [0 :: Int .. (n - 1)]
             in liftA2 (,) (runRVar rvar globalStdGen) (runRVar rvar globalStdGen)
        let ppred :: Builder = varsToPred "P" phivars
            qpred :: Builder = varsToPred "Q" psivars
        T.writeFile fo_f ("" +| ppred |+ " OR " +| qpred |+ "\n")
        withPrintState log_f $ do
          forM_ [0 .. nts] $ \i ->
            forM_ [0 .. (ntp - 1)] $ \_ -> do
              newDb i
              outputRandomEvents "P" or_lsize n
              outputRandomEvents "Q" or_rsize n
          endOutput

existsBenchGen log_f sig_f fo_f nts ntp ExistsConfig {..} =
  let vars = [0 .. (ex_predn - 1)]
      ppred = varsToPred "P" vars
      samplervar = L.shuffleNofM ex_n ex_predn vars
   in do
        projvars <- runRVar samplervar globalStdGen
        let exvars = argsF projvars
        T.writeFile fo_f ("EXISTS " +| exvars |+ ". " +| ppred |+ "\n")
        withFile sig_f WriteMode (addPredToSig "P" ex_predn)
        withPrintState log_f $ do
          forM_ [0 .. nts] $ \i ->
            forM_ [0 .. (ntp - 1)] $ \_ -> do
              newDb i
              outputRandomEvents "P" ex_size ex_predn
          endOutput

eventuallyOnceBenchGen log_f sig_f fo_f nts ntp lbound ubound eventrate nvars isOnce =
  let vars = [0 .. (nvars - 1)]
      ppred = varsToPred "P" vars
      newEvPerTp = eventrate `div` ntp
   in do
        T.writeFile
          fo_f
          ( (if isOnce then "ONCE" else "EVENTUALLY")
              +| (lbound, ubound)
              |+ " "
              +| ppred
              |+ "\n"
          )
        withFile sig_f WriteMode (addPredToSig "P" nvars)
        withPrintState log_f $ do
          forM_ [0 .. nts] $ \i ->
            forM_ [0 .. (ntp - 1)] $ \_ -> do
              newDb i
              outputRandomEvents "P" newEvPerTp nvars
          endOutput

sinceGenHelper lbound ubound n2 cidx2 evr rmp nts ntp =
  let newEvPerTp = evr `div` ntp
      go i j sl
        | i == nts = return ()
        | otherwise =
            let advsl = advanceSlidingWindow i sl
             in do
                  newDb (fromIntegral i)
                  revs <- V.replicateM newEvPerTp (randomInt64Vec n2)
                  V.forM_ revs (outputNewEvent "Q")
                  let revsproj = V.map (`projectVec` cidx2) revs
                      newsl = addEventsSlidingWindow revsproj advsl
                  killsl <- killRandomEventsSlidingWindow rmp newsl
                  forM_
                    (getInSlidingWindow killsl)
                    ( \(_, evs) ->
                        V.forM_ evs (outputNewEvent "P")
                    )
                  if (j + 1) == ntp
                    then go (i + 1) 0 killsl
                    else go i (j + 1) killsl
   in go 0 0 (initSlidingWindow lbound ubound)

untilGenHelper _ _ _ _ _ _ _ _ =
  undefined

sinceUntilBenchGen log_f sig_f fo_f nts ntp lbound ubound SinceUntilConfig {..} = do
  (vars1, vars2) <- genSubsetVars siut_vars True
  let pred1 = varsToPred "P" vars1
      pred2 = varsToPred "Q" vars2
      n1 = length vars1
      n2 = length vars2
      (_, cidx2) = commonVars vars1 vars2
  let pred1MaybeNeg :: Builder =
        if siut_neg
          then "(NOT " +| pred1 |+ ")"
          else "" +| pred1 |+ ""
  let sinceUntilStr :: Builder =
        if siut_isut
          then "UNTIL"
          else "SINCE"
  T.writeFile
    fo_f
    ( ""
        +| pred1MaybeNeg
        |+ " "
        +| sinceUntilStr
        |+ " "
        +| (lbound, ubound)
        |+ " "
        +| pred2
        |+ "\n"
    )
  withFile
    sig_f
    WriteMode
    ( \sig_h -> do
        addPredToSig "P" n1 sig_h
        addPredToSig "Q" n2 sig_h
    )
  if siut_isut
    then untilGenHelper lbound ubound n2 cidx2 siut_evr siut_rmp nts ntp
    else withPrintState log_f $
      do
        sinceGenHelper lbound ubound n2 cidx2 siut_evr siut_rmp nts ntp
        endOutput

onceAndNotBenchGen log_f sig_f fo_f nts ntp evr =
  let newEvPerTp = evr `div` ntp
   in do
        T.writeFile fo_f "(ONCE A(x)) AND (x = 5)"
        withFile sig_f WriteMode (addPredToSig "A" 1)
        withPrintState log_f $ do
          forM_ [0 .. nts] $ \i ->
            forM_ [0 .. (ntp - 1)] $ \_ -> do
              newDb i
              outputRandomEvents "A" newEvPerTp 1
          endOutput

temporalBenchGen log_f sig_f fo_f nts ntp TemporalConfig {..} =
  case tc_suboperator of
    PrevOperator PrevConfig {..} -> do
      T.writeFile fo_f ("PREV" +| (tc_lbound, tc_ubound) |+ " " +| varsToPred "P" [0, 1] |+ "\n")
      withFile sig_f WriteMode (addPredToSig "P" 2)
      withPrintState log_f $ do
        forM_ [0 .. nts] $ \i ->
          forM_ [0 .. (ntp - 1)] $ \_ -> do
            newDb i
            outputRandomEvents "P" pr_size 2
        endOutput
    NextOperator NextConfig {..} -> do
      T.writeFile fo_f ("NEXT" +| (tc_lbound, tc_ubound) |+ " " +| varsToPred "P" [0, 1] |+ "\n")
      withFile sig_f WriteMode (addPredToSig "P" 2)
      withPrintState log_f $ do
        forM_ [0 .. nts] $ \i ->
          forM_ [0 .. (ntp - 1)] $ \_ -> do
            newDb i
            outputRandomEvents "P" nx_size 2
        endOutput
    OnceOperator OnceConfig {..} ->
      eventuallyOnceBenchGen log_f sig_f fo_f nts ntp tc_lbound tc_ubound oc_eventrate oc_nvars True
    EventuallyOperator EventuallyConfig {..} ->
      eventuallyOnceBenchGen log_f sig_f fo_f nts ntp tc_lbound tc_ubound ev_eventrate ev_nvars False
    SinceOperator SinceConfig {..} ->
      sinceUntilBenchGen
        log_f
        sig_f
        fo_f
        nts
        ntp
        tc_lbound
        tc_ubound
        SinceUntilConfig
          { siut_evr = si_eventrate,
            siut_neg = si_negate,
            siut_vars = si_vars,
            siut_isut = False,
            siut_rmp = si_removeprobability
          }
    UntilOperator UntilConfig {..} ->
      sinceUntilBenchGen
        log_f
        sig_f
        fo_f
        nts
        ntp
        tc_lbound
        tc_ubound
        SinceUntilConfig
          { siut_evr = ut_eventrate,
            siut_neg = ut_negate,
            siut_vars = ut_vars,
            siut_isut = True,
            siut_rmp = ut_removeprobability
          }

generateLogForBenchmark :: FilePath -> FilePath -> FilePath -> OperatorBenchmark -> IO ()
generateLogForBenchmark log_f sig_f fo_f OperatorBenchmark {..} =
  case op_config of
    AndOperator andconf -> andBenchGen log_f sig_f fo_f op_numts op_numtpperts andconf
    AntiJoinOperator ajconf -> andNotBenchGen log_f sig_f fo_f op_numts op_numtpperts ajconf
    OrOperator orconf -> orBenchGen log_f sig_f fo_f op_numts op_numtpperts orconf
    ExistsOperator exconf -> existsBenchGen log_f sig_f fo_f op_numts op_numtpperts exconf
    TemporalOperator tempconf -> temporalBenchGen log_f sig_f fo_f op_numts op_numtpperts tempconf
    OnceAndEqOperator OnceAndEqConfig {..} ->
      onceAndNotBenchGen log_f sig_f fo_f op_numts op_numtpperts oa_eventrate

generateRandomLog :: FilePath -> Signature -> ReaderT Flags IO ()
generateRandomLog fp sig =
  let arrsig = map (\(a, b) -> (a, length b)) sig
   in do
        when (any (any (/= IntTy) . snd) sig) $
          error "only integer signatures supported"
        Flags {f_nes_flags = RandomTestFlags {..}, ..} <- R.ask
        R.liftIO . withPrintState fp $ do
          forM_ [0 .. rt_num_ts] $ \ts ->
            replicateM_ rt_db_per_ts $ do
              newDb (fromIntegral ts)
              forM_ arrsig $ \(name, arity) ->
                replicateM_ rt_events_per_db $
                  randomEvent arity rt_ub
                    >>= outputNewEvent name
          endOutput
