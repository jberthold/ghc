{-# LANGUAGE GADTs #-}
module CmmSink (
     cmmSink
  ) where

import CodeGen.Platform (callerSaves)

import Cmm
import CmmOpt
import BlockId
import CmmLive
import CmmUtils
import Hoopl

import DynFlags
import UniqFM
import PprCmm ()

import Data.List (partition)
import qualified Data.Set as Set

-- -----------------------------------------------------------------------------
-- Sinking and inlining

-- This is an optimisation pass that
--  (a) moves assignments closer to their uses, to reduce register pressure
--  (b) pushes assignments into a single branch of a conditional if possible
--  (c) inlines assignments to registers that are mentioned only once
--  (d) discards dead assignments
--
-- This tightens up lots of register-heavy code.  It is particularly
-- helpful in the Cmm generated by the Stg->Cmm code generator, in
-- which every function starts with a copyIn sequence like:
--
--    x1 = R1
--    x2 = Sp[8]
--    x3 = Sp[16]
--    if (Sp - 32 < SpLim) then L1 else L2
--
-- we really want to push the x1..x3 assignments into the L2 branch.
--
-- Algorithm:
--
--  * Start by doing liveness analysis.
--
--  * Keep a list of assignments A; earlier ones may refer to later ones
--
--  * Walk forwards through the graph, look at each node N:
--    * If any assignments in A (1) occur only once in N, and (2) are
--      not live after N, inline the assignment and remove it
--      from A.
--    * If N is an assignment:
--      * If the register is not live after N, discard it
--      * otherwise pick up the assignment and add it to A
--    * If N is a non-assignment node:
--      * remove any assignments from A that conflict with N, and
--        place them before N in the current block.  (we call this
--        "dropping" the assignments).
--      * An assignment conflicts with N if it:
--        - assigns to a register mentioned in N
--        - mentions a register assigned by N
--        - reads from memory written by N
--      * do this recursively, dropping dependent assignments
--    * At a multi-way branch:
--      * drop any assignments that are live on more than one branch
--      * if any successor has more than one predecessor (a
--        join-point), drop everything live in that successor
-- 
-- As a side-effect we'll delete some dead assignments (transitively,
-- even).  This isn't as good as removeDeadAssignments, but it's much
-- cheaper.

-- If we do this *before* stack layout, we might be able to avoid
-- saving some things across calls/procpoints.
--
-- *but*, that will invalidate the liveness analysis, and we'll have
-- to re-do it.

-- -----------------------------------------------------------------------------
-- things that we aren't optimising very well yet.
--
-- -----------
-- (1) From GHC's FastString.hashStr:
--
--  s2ay:
--      if ((_s2an::I64 == _s2ao::I64) >= 1) goto c2gn; else goto c2gp;
--  c2gn:
--      R1 = _s2au::I64;
--      call (I64[Sp])(R1) args: 8, res: 0, upd: 8;
--  c2gp:
--      _s2cO::I64 = %MO_S_Rem_W64(%MO_UU_Conv_W8_W64(I8[_s2aq::I64 + (_s2an::I64 << 0)]) + _s2au::I64 * 128,
--                                 4091);
--      _s2an::I64 = _s2an::I64 + 1;
--      _s2au::I64 = _s2cO::I64;
--      goto s2ay;
--
-- a nice loop, but we didn't eliminate the silly assignment at the end.
-- See Note [dependent assignments], which would probably fix this.
--
-- -----------
-- (2) From stg_atomically_frame in PrimOps.cmm
--
-- We have a diamond control flow:
--
--     x = ...
--       |
--      / \
--     A   B
--      \ /
--       |
--    use of x
--
-- Now x won't be sunk down to its use, because we won't push it into
-- both branches of the conditional.  We certainly do have to check
-- that we can sink it past all the code in both A and B, but having
-- discovered that, we could sink it to its use.
--

-- -----------------------------------------------------------------------------

type Assignment = (LocalReg, CmmExpr, AbsMem)
  -- Assignment caches AbsMem, an abstraction of the memory read by
  -- the RHS of the assignment.

cmmSink :: DynFlags -> CmmGraph -> CmmGraph
cmmSink dflags graph = ofBlockList (g_entry graph) $ sink mapEmpty $ blocks
  where
  liveness = cmmLiveness graph
  getLive l = mapFindWithDefault Set.empty l liveness

  blocks = postorderDfs graph

  join_pts = findJoinPoints blocks

  sink :: BlockEnv [Assignment] -> [CmmBlock] -> [CmmBlock]
  sink _ [] = []
  sink sunk (b:bs) =
    -- pprTrace "sink" (ppr lbl) $
    blockJoin first final_middle final_last : sink sunk' bs
    where
      lbl = entryLabel b
      (first, middle, last) = blockSplit b

      succs = successors last

      -- Annotate the middle nodes with the registers live *after*
      -- the node.  This will help us decide whether we can inline
      -- an assignment in the current node or not.
      live = Set.unions (map getLive succs)
      live_middle = gen_kill last live
      ann_middles = annotate live_middle (blockToList middle)

      -- Now sink and inline in this block
      (middle', assigs) = walk dflags ann_middles (mapFindWithDefault [] lbl sunk)
      fold_last = constantFold dflags last
      (final_last, assigs') = tryToInline dflags live fold_last assigs

      -- We cannot sink into join points (successors with more than
      -- one predecessor), so identify the join points and the set
      -- of registers live in them.
      (joins, nonjoins) = partition (`mapMember` join_pts) succs
      live_in_joins = Set.unions (map getLive joins)

      -- We do not want to sink an assignment into multiple branches,
      -- so identify the set of registers live in multiple successors.
      -- This is made more complicated because when we sink an assignment
      -- into one branch, this might change the set of registers that are
      -- now live in multiple branches.
      init_live_sets = map getLive nonjoins
      live_in_multi live_sets r =
         case filter (Set.member r) live_sets of
           (_one:_two:_) -> True
           _ -> False

      -- Now, drop any assignments that we will not sink any further.
      (dropped_last, assigs'') = dropAssignments dflags drop_if init_live_sets assigs'

      drop_if a@(r,rhs,_) live_sets = (should_drop, live_sets')
          where
            should_drop =  conflicts dflags a final_last
                        || {- not (isSmall rhs) && -} live_in_multi live_sets r
                        || r `Set.member` live_in_joins

            live_sets' | should_drop = live_sets
                       | otherwise   = map upd live_sets

            upd set | r `Set.member` set = set `Set.union` live_rhs
                    | otherwise          = set

            live_rhs = foldRegsUsed extendRegSet emptyRegSet rhs

      final_middle = foldl blockSnoc middle' dropped_last

      sunk' = mapUnion sunk $
                 mapFromList [ (l, filterAssignments dflags (getLive l) assigs'')
                             | l <- succs ]

{- TODO: enable this later, when we have some good tests in place to
   measure the effect and tune it.

-- small: an expression we don't mind duplicating
isSmall :: CmmExpr -> Bool
isSmall (CmmReg (CmmLocal _)) = True  -- not globals, we want to coalesce them instead
isSmall (CmmLit _) = True
isSmall (CmmMachOp (MO_Add _) [x,y]) = isTrivial x && isTrivial y
isSmall (CmmRegOff (CmmLocal _) _) = True
isSmall _ = False

isTrivial :: CmmExpr -> Bool
isTrivial (CmmReg (CmmLocal _)) = True
isTrivial (CmmLit _) = True
isTrivial _ = False
-}

--
-- annotate each node with the set of registers live *after* the node
--
annotate :: RegSet -> [CmmNode O O] -> [(RegSet, CmmNode O O)]
annotate live nodes = snd $ foldr ann (live,[]) nodes
  where ann n (live,nodes) = (gen_kill n live, (live,n) : nodes)

--
-- Find the blocks that have multiple successors (join points)
--
findJoinPoints :: [CmmBlock] -> BlockEnv Int
findJoinPoints blocks = mapFilter (>1) succ_counts
 where
  all_succs = concatMap successors blocks

  succ_counts :: BlockEnv Int
  succ_counts = foldr (\l -> mapInsertWith (+) l 1) mapEmpty all_succs

--
-- filter the list of assignments to remove any assignments that
-- are not live in a continuation.
--
filterAssignments :: DynFlags -> RegSet -> [Assignment] -> [Assignment]
filterAssignments dflags live assigs = reverse (go assigs [])
  where go []             kept = kept
        go (a@(r,_,_):as) kept | needed    = go as (a:kept)
                               | otherwise = go as kept
           where
              needed = r `Set.member` live
                       || any (conflicts dflags a) (map toNode kept)
                       --  Note that we must keep assignments that are
                       -- referred to by other assignments we have
                       -- already kept.

-- -----------------------------------------------------------------------------
-- Walk through the nodes of a block, sinking and inlining assignments
-- as we go.

walk :: DynFlags
     -> [(RegSet, CmmNode O O)]         -- nodes of the block, annotated with
                                        -- the set of registers live *after*
                                        -- this node.

     -> [Assignment]                    -- The current list of
                                        -- assignments we are sinking.
                                        -- Later assignments may refer
                                        -- to earlier ones.

     -> ( Block CmmNode O O             -- The new block
        , [Assignment]                  -- Assignments to sink further
        )

walk dflags nodes assigs = go nodes emptyBlock assigs
 where
   go []               block as = (block, as)
   go ((live,node):ns) block as
    | shouldDiscard node live    = go ns block as
    | Just a <- shouldSink dflags node2 = go ns block (a : as1)
    | otherwise                         = go ns block' as'
    where
      node1 = constantFold dflags node

      (node2, as1) = tryToInline dflags live node1 as

      (dropped, as') = dropAssignmentsSimple dflags
                          (\a -> conflicts dflags a node2) as1

      block' = foldl blockSnoc block dropped `blockSnoc` node2


constantFold :: DynFlags -> CmmNode e x -> CmmNode e x
constantFold dflags node = mapExpDeep f node
  where f (CmmMachOp op args) = cmmMachOpFold dflags op args
        f (CmmRegOff r 0) = CmmReg r
        f e = e

--
-- Heuristic to decide whether to pick up and sink an assignment
-- Currently we pick up all assignments to local registers.  It might
-- be profitable to sink assignments to global regs too, but the
-- liveness analysis doesn't track those (yet) so we can't.
--
shouldSink :: DynFlags -> CmmNode e x -> Maybe Assignment
shouldSink dflags (CmmAssign (CmmLocal r) e) | no_local_regs = Just (r, e, exprMem dflags e)
  where no_local_regs = True -- foldRegsUsed (\_ _ -> False) True e
shouldSink _ _other = Nothing

--
-- discard dead assignments.  This doesn't do as good a job as
-- removeDeadAsssignments, because it would need multiple passes
-- to get all the dead code, but it catches the common case of
-- superfluous reloads from the stack that the stack allocator
-- leaves behind.
--
-- Also we catch "r = r" here.  You might think it would fall
-- out of inlining, but the inliner will see that r is live
-- after the instruction and choose not to inline r in the rhs.
--
shouldDiscard :: CmmNode e x -> RegSet -> Bool
shouldDiscard node live
   = case node of
       CmmAssign r (CmmReg r') | r == r' -> True
       CmmAssign (CmmLocal r) _ -> not (r `Set.member` live)
       _otherwise -> False
  

toNode :: Assignment -> CmmNode O O
toNode (r,rhs,_) = CmmAssign (CmmLocal r) rhs

dropAssignmentsSimple :: DynFlags -> (Assignment -> Bool) -> [Assignment]
                      -> ([CmmNode O O], [Assignment])
dropAssignmentsSimple dflags f = dropAssignments dflags (\a _ -> (f a, ())) ()

dropAssignments :: DynFlags -> (Assignment -> s -> (Bool, s)) -> s -> [Assignment]
                -> ([CmmNode O O], [Assignment])
dropAssignments dflags should_drop state assigs
 = (dropped, reverse kept)
 where
   (dropped,kept) = go state assigs [] []

   go _ []             dropped kept = (dropped, kept)
   go state (assig : rest) dropped kept
      | conflict  = go state' rest (toNode assig : dropped) kept
      | otherwise = go state' rest dropped (assig:kept)
      where
        (dropit, state') = should_drop assig state
        conflict = dropit || any (conflicts dflags assig) dropped


-- -----------------------------------------------------------------------------
-- Try to inline assignments into a node.

tryToInline
   :: DynFlags
   -> RegSet                    -- set of registers live after this
                                -- node.  We cannot inline anything
                                -- that is live after the node, unless
                                -- it is small enough to duplicate.
   -> CmmNode O x               -- The node to inline into
   -> [Assignment]              -- Assignments to inline
   -> (
        CmmNode O x             -- New node
      , [Assignment]            -- Remaining assignments
      )

tryToInline dflags live node assigs = go usages node [] assigs
 where
  usages :: UniqFM Int
  usages = foldRegsUsed addUsage emptyUFM node

  go _usages node _skipped [] = (node, [])

  go usages node skipped (a@(l,rhs,_) : rest)
   | can_inline              = inline_and_discard
   | False {- isTiny rhs -}  = inline_and_keep
     --  ^^ seems to make things slightly worse
   where
        inline_and_discard = go usages' node' skipped rest

        inline_and_keep = (node'', a : rest')
          where (node'',rest') = go usages' node' (l:skipped) rest

        can_inline =
            not (l `elemRegSet` live)
         && not (skipped `regsUsedIn` rhs)  -- Note [dependent assignments]
         && okToInline dflags rhs node
         && lookupUFM usages l == Just 1

        usages' = foldRegsUsed addUsage usages rhs

        node' = mapExpDeep inline node
           where inline (CmmReg    (CmmLocal l'))     | l == l' = rhs
                 inline (CmmRegOff (CmmLocal l') off) | l == l'
                    = cmmOffset dflags rhs off
                    -- re-constant fold after inlining
                 inline (CmmMachOp op args) = cmmMachOpFold dflags op args
                 inline other = other

  go usages node skipped (assig@(l,rhs,_) : rest)
    = (node', assig : rest')
    where (node', rest') = go usages' node (l:skipped) rest
          usages' = foldRegsUsed (\m r -> addToUFM m r 2) usages rhs
          -- we must not inline anything that is mentioned in the RHS
          -- of a binding that we have already skipped, so we set the
          -- usages of the regs on the RHS to 2.

-- Note [dependent assignments]
--
-- If our assignment list looks like
--
--    [ y = e,  x = ... y ... ]
--
-- We cannot inline x.  Remember this list is really in reverse order,
-- so it means  x = ... y ...; y = e
--
-- Hence if we inline x, the outer assignment to y will capture the
-- reference in x's right hand side.
--
-- In this case we should rename the y in x's right-hand side,
-- i.e. change the list to [ y = e, x = ... y1 ..., y1 = y ]
-- Now we can go ahead and inline x.
--
-- For now we do nothing, because this would require putting
-- everything inside UniqSM.

addUsage :: UniqFM Int -> LocalReg -> UniqFM Int
addUsage m r = addToUFM_C (+) m r 1

regsUsedIn :: [LocalReg] -> CmmExpr -> Bool
regsUsedIn [] _ = False
regsUsedIn ls e = wrapRecExpf f e False
  where f (CmmReg (CmmLocal l))      _ | l `elem` ls = True
        f (CmmRegOff (CmmLocal l) _) _ | l `elem` ls = True
        f _ z = z

-- we don't inline into CmmUnsafeForeignCall if the expression refers
-- to global registers.  This is a HACK to avoid global registers
-- clashing with C argument-passing registers, really the back-end
-- ought to be able to handle it properly, but currently neither PprC
-- nor the NCG can do it.  See Note [Register parameter passing]
-- See also StgCmmForeign:load_args_into_temps.
okToInline :: DynFlags -> CmmExpr -> CmmNode e x -> Bool
okToInline dflags expr CmmUnsafeForeignCall{} = not (anyCallerSavesRegs dflags expr)
okToInline _ _ _ = True

-- -----------------------------------------------------------------------------

-- | @conflicts (r,e) stmt@ is @False@ if and only if the assignment
-- @r = e@ can be safely commuted past @stmt@.
--
-- We only sink "r = G" assignments right now, so conflicts is very simple:
--
conflicts :: DynFlags -> Assignment -> CmmNode O x -> Bool
conflicts dflags (r, rhs, addr) node

  -- (1) an assignment to a register conflicts with a use of the register
  | CmmAssign reg  _ <- node, reg `regUsedIn` rhs                 = True
  | foldRegsUsed (\b r' -> r == r' || b) False node               = True

  -- (2) a store to an address conflicts with a read of the same memory
  | CmmStore addr' e <- node
  , memConflicts addr (loadAddr dflags addr' (cmmExprWidth dflags e)) = True

  -- (3) an assignment to Hp/Sp conflicts with a heap/stack read respectively
  | HeapMem    <- addr, CmmAssign (CmmGlobal Hp) _ <- node        = True
  | StackMem   <- addr, CmmAssign (CmmGlobal Sp) _ <- node        = True
  | SpMem{}    <- addr, CmmAssign (CmmGlobal Sp) _ <- node        = True

  -- (4) assignments that read caller-saves GlobalRegs conflict with a
  -- foreign call.  See Note [foreign calls clobber GlobalRegs].
  | CmmUnsafeForeignCall{} <- node, anyCallerSavesRegs dflags rhs = True

  -- (5) foreign calls clobber heap: see Note [foreign calls clobber heap]
  | CmmUnsafeForeignCall{} <- node, memConflicts addr AnyMem      = True

  -- (6) native calls clobber any memory
  | CmmCall{} <- node, memConflicts addr AnyMem                   = True

  -- (7) otherwise, no conflict
  | otherwise = False


anyCallerSavesRegs :: DynFlags -> CmmExpr -> Bool
anyCallerSavesRegs dflags e = wrapRecExpf f e False
  where f (CmmReg (CmmGlobal r)) _
         | callerSaves (targetPlatform dflags) r = True
        f _ z = z

-- An abstraction of memory read or written.
data AbsMem
  = NoMem            -- no memory accessed
  | AnyMem           -- arbitrary memory
  | HeapMem          -- definitely heap memory
  | StackMem         -- definitely stack memory
  | SpMem            -- <size>[Sp+n]
       {-# UNPACK #-} !Int
       {-# UNPACK #-} !Int

-- Having SpMem is important because it lets us float loads from Sp
-- past stores to Sp as long as they don't overlap, and this helps to
-- unravel some long sequences of
--    x1 = [Sp + 8]
--    x2 = [Sp + 16]
--    ...
--    [Sp + 8]  = xi
--    [Sp + 16] = xj
--
-- Note that SpMem is invalidated if Sp is changed, but the definition
-- of 'conflicts' above handles that.

-- ToDo: this won't currently fix the following commonly occurring code:
--    x1 = [R1 + 8]
--    x2 = [R1 + 16]
--    ..
--    [Hp - 8] = x1
--    [Hp - 16] = x2
--    ..

-- because [R1 + 8] and [Hp - 8] are both HeapMem.  We know that
-- assignments to [Hp + n] do not conflict with any other heap memory,
-- but this is tricky to nail down.  What if we had
--
--   x = Hp + n
--   [x] = ...
--
--  the store to [x] should be "new heap", not "old heap".
--  Furthermore, you could imagine that if we started inlining
--  functions in Cmm then there might well be reads of heap memory
--  that was written in the same basic block.  To take advantage of
--  non-aliasing of heap memory we will have to be more clever.

-- Note [foreign calls clobber]
--
-- It is tempting to say that foreign calls clobber only
-- non-heap/stack memory, but unfortunately we break this invariant in
-- the RTS.  For example, in stg_catch_retry_frame we call
-- stmCommitNestedTransaction() which modifies the contents of the
-- TRec it is passed (this actually caused incorrect code to be
-- generated).
--
-- Since the invariant is true for the majority of foreign calls,
-- perhaps we ought to have a special annotation for calls that can
-- modify heap/stack memory.  For now we just use the conservative
-- definition here.


bothMems :: AbsMem -> AbsMem -> AbsMem
bothMems NoMem    x         = x
bothMems x        NoMem     = x
bothMems HeapMem  HeapMem   = HeapMem
bothMems StackMem StackMem     = StackMem
bothMems (SpMem o1 w1) (SpMem o2 w2)
  | o1 == o2  = SpMem o1 (max w1 w2)
  | otherwise = StackMem
bothMems SpMem{}  StackMem  = StackMem
bothMems StackMem SpMem{}   = StackMem
bothMems _         _        = AnyMem

memConflicts :: AbsMem -> AbsMem -> Bool
memConflicts NoMem      _          = False
memConflicts _          NoMem      = False
memConflicts HeapMem    StackMem   = False
memConflicts StackMem   HeapMem    = False
memConflicts SpMem{}    HeapMem    = False
memConflicts HeapMem    SpMem{}    = False
memConflicts (SpMem o1 w1) (SpMem o2 w2)
  | o1 < o2   = o1 + w1 > o2
  | otherwise = o2 + w2 > o1
memConflicts _         _         = True

exprMem :: DynFlags -> CmmExpr -> AbsMem
exprMem dflags (CmmLoad addr w)  = bothMems (loadAddr dflags addr (typeWidth w)) (exprMem dflags addr)
exprMem dflags (CmmMachOp _ es)  = foldr bothMems NoMem (map (exprMem dflags) es)
exprMem _      _                 = NoMem

loadAddr :: DynFlags -> CmmExpr -> Width -> AbsMem
loadAddr dflags e w =
  case e of
   CmmReg r       -> regAddr dflags r 0 w
   CmmRegOff r i  -> regAddr dflags r i w
   _other | CmmGlobal Sp `regUsedIn` e -> StackMem
          | otherwise -> AnyMem

regAddr :: DynFlags -> CmmReg -> Int -> Width -> AbsMem
regAddr _      (CmmGlobal Sp) i w = SpMem i (widthInBytes w)
regAddr _      (CmmGlobal Hp) _ _ = HeapMem
regAddr _      (CmmGlobal CurrentTSO) _ _ = HeapMem -- important for PrimOps
regAddr dflags r _ _ | isGcPtrType (cmmRegType dflags r) = HeapMem -- yay! GCPtr pays for itself
regAddr _      _ _ _ = AnyMem
