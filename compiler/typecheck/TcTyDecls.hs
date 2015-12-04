{-
(c) The University of Glasgow 2006
(c) The GRASP/AQUA Project, Glasgow University, 1992-1999


Analysis functions over data types.  Specficially, detecting recursive types.

This stuff is only used for source-code decls; it's recorded in interface
files for imported data types.
-}

{-# LANGUAGE CPP #-}

module TcTyDecls(
        calcRecFlags, RecTyInfo(..),
        calcSynCycles, calcClassCycles,

        -- * Roles
        RoleAnnots, extractRoleAnnots, emptyRoleAnnots, lookupRoleAnnots,

        -- * Implicits
        tcAddImplicits,

        -- * Record selectors
        mkRecSelBinds, mkOneRecordSelector
    ) where

#include "HsVersions.h"

import TcRnMonad
import TcEnv
import TcTypeable( mkTypeableBinds )
import TcBinds( tcRecSelBinds )
import TypeRep( Type(..) )
import TcType
import TysWiredIn( unitTy )
import MkCore( rEC_SEL_ERROR_ID )
import HsSyn
import Class
import Type
import HscTypes
import TyCon
import ConLike
import DataCon
import Name
import NameEnv
import RdrName ( mkVarUnqual )
import Id
import IdInfo
import VarEnv
import VarSet
import NameSet
import Coercion ( ltRole )
import Digraph
import BasicTypes
import SrcLoc
import Unique ( mkBuiltinUnique )
import Outputable
import UniqSet
import Util
import Maybes
import Data.List
import Bag
import FastString ( fastStringToByteString )

import Control.Monad

{-
************************************************************************
*                                                                      *
        Cycles in class and type synonym declarations
*                                                                      *
************************************************************************

Checking for class-decl loops is easy, because we don't allow class decls
in interface files.

We allow type synonyms in hi-boot files, but we *trust* hi-boot files,
so we don't check for loops that involve them.  So we only look for synonym
loops in the module being compiled.

We check for type synonym and class cycles on the *source* code.
Main reasons:

  a) Otherwise we'd need a special function to extract type-synonym tycons
     from a type, whereas we already have the free vars pinned on the decl

  b) If we checked for type synonym loops after building the TyCon, we
        can't do a hoistForAllTys on the type synonym rhs, (else we fall into
        a black hole) which seems unclean.  Apart from anything else, it'd mean
        that a type-synonym rhs could have for-alls to the right of an arrow,
        which means adding new cases to the validity checker

        Indeed, in general, checking for cycles beforehand means we need to
        be less careful about black holes through synonym cycles.

The main disadvantage is that a cycle that goes via a type synonym in an
.hi-boot file can lead the compiler into a loop, because it assumes that cycles
only occur entirely within the source code of the module being compiled.
But hi-boot files are trusted anyway, so this isn't much worse than (say)
a kind error.

[  NOTE ----------------------------------------------
If we reverse this decision, this comment came from tcTyDecl1, and should
 go back there
        -- dsHsType, not tcHsKindedType, to avoid a loop.  tcHsKindedType does hoisting,
        -- which requires looking through synonyms... and therefore goes into a loop
        -- on (erroneously) recursive synonyms.
        -- Solution: do not hoist synonyms, because they'll be hoisted soon enough
        --           when they are substituted

We'd also need to add back in this definition

synonymTyConsOfType :: Type -> [TyCon]
-- Does not look through type synonyms at all
-- Return a list of synonym tycons
synonymTyConsOfType ty
  = nameEnvElts (go ty)
  where
     go :: Type -> NameEnv TyCon  -- The NameEnv does duplicate elim
     go (TyVarTy v)               = emptyNameEnv
     go (TyConApp tc tys)         = go_tc tc tys
     go (AppTy a b)               = go a `plusNameEnv` go b
     go (FunTy a b)               = go a `plusNameEnv` go b
     go (ForAllTy _ ty)           = go ty

     go_tc tc tys | isTypeSynonymTyCon tc = extendNameEnv (go_s tys)
                                                          (tyConName tc) tc
                  | otherwise             = go_s tys
     go_s tys = foldr (plusNameEnv . go) emptyNameEnv tys
---------------------------------------- END NOTE ]
-}

mkSynEdges :: [LTyClDecl Name] -> [(LTyClDecl Name, Name, [Name])]
mkSynEdges syn_decls = [ (ldecl, name, nameSetElems fvs)
                       | ldecl@(L _ (SynDecl { tcdLName = L _ name
                                             , tcdFVs = fvs })) <- syn_decls ]

calcSynCycles :: [LTyClDecl Name] -> [SCC (LTyClDecl Name)]
calcSynCycles = stronglyConnCompFromEdgedVertices . mkSynEdges

{-
Note [Superclass cycle check]
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
We can't allow cycles via superclasses because it would result in the
type checker looping when it canonicalises a class constraint (superclasses
are added during canonicalisation).  More precisely, given a constraint
    C ty1 .. tyn
we want to instantiate all of C's superclasses, transitively, and
that set must be finite.  So if
     class (D b, E b a) => C a b
then when we encounter the constraint
     C ty1 ty2
we'll instantiate the superclasses
     (D ty2, E ty2 ty1)
and then *their* superclasses, and so on.  This set must be finite!

It is OK for superclasses to be type synonyms for other classes, so
must "look through" type synonyms. Eg
     type X a = C [a]
     class X a => C a   -- No!  Recursive superclass!

We want definitions such as:

  class C cls a where cls a => a -> a
  class C D a => D a where

to be accepted, even though a naive acyclicity check would reject the
program as having a cycle between D and its superclass.  Why? Because
when we instantiate
     D ty1
we get the superclas
     C D ty1
and C has no superclasses, so we have terminated with a finite set.

More precisely, the rule is this: the superclasses sup_C of a class C
are rejected iff:

  C \elem expand(sup_C)

Where expand is defined as follows:

(1)  expand(a ty1 ... tyN) = expand(ty1) \union ... \union expand(tyN)

(2)  expand(D ty1 ... tyN) = {D}
                             \union sup_D[ty1/x1, ..., tyP/xP]
                             \union expand(ty(P+1)) ... \union expand(tyN)
           where (D x1 ... xM) is a class, P = min(M,N)

(3)  expand(T ty1 ... tyN) = expand(ty1) \union ... \union expand(tyN)
        where T is not a class

Eqn (1) is conservative; when there's a type variable at the head,
look in all the argument types.  Eqn (2) expands superclasses; the
third component of the union is like Eqn (1).  Eqn (3) happens mainly
when the context is a (constraint) tuple, such as (Eq a, Show a).

Furthermore, expand always looks through type synonyms.
-}

calcClassCycles :: Class -> [[TyCon]]
calcClassCycles cls
  = nubBy eqAsCycle $
    expandTheta (unitUniqSet cls) [classTyCon cls] (classSCTheta cls) []
  where
    -- The last TyCon in the cycle is always the same as the first
    eqAsCycle xs ys = any (xs ==) (cycles (tail ys))
    cycles xs = take n . map (take n) . tails . cycle $ xs
      where n = length xs

    -- No more superclasses to expand ==> no problems with cycles
    -- See Note [Superclass cycle check]
    expandTheta :: UniqSet Class -- Path of Classes to here in set form
                -> [TyCon]       -- Path to here
                -> ThetaType     -- Superclass work list
                -> [[TyCon]]     -- Input error paths
                -> [[TyCon]]     -- Final error paths
    expandTheta _    _    []           = id
    expandTheta seen path (pred:theta) = expandType seen path pred . expandTheta seen path theta

    expandType seen path (TyConApp tc tys)
      -- Expand unsaturated classes to their superclass theta if they are yet unseen.
      -- If they have already been seen then we have detected an error!
      | Just cls <- tyConClass_maybe tc
      , let (env, remainder) = papp (classTyVars cls) tys
            rest_tys = either (const []) id remainder
      = if cls `elementOfUniqSet` seen
         then (reverse (classTyCon cls:path):)
              . flip (foldr (expandType seen path)) tys
         else expandTheta (addOneToUniqSet seen cls) (tc:path)
                          (substTys (mkTopTvSubst env) (classSCTheta cls))
              . flip (foldr (expandType seen path)) rest_tys

      -- For synonyms, try to expand them: some arguments might be
      -- phantoms, after all. We can expand with impunity because at
      -- this point the type synonym cycle check has already happened.
      | Just (tvs, rhs) <- synTyConDefn_maybe tc
      , let (env, remainder) = papp tvs tys
            rest_tys = either (const []) id remainder
      = expandType seen (tc:path) (substTy (mkTopTvSubst env) rhs)
        . flip (foldr (expandType seen path)) rest_tys

      -- For non-class, non-synonyms, just check the arguments
      | otherwise
      = flip (foldr (expandType seen path)) tys

    expandType _    _    (TyVarTy {})     = id
    expandType _    _    (LitTy {})       = id
    expandType seen path (AppTy t1 t2)    = expandType seen path t1 . expandType seen path t2
    expandType seen path (FunTy t1 t2)    = expandType seen path t1 . expandType seen path t2
    expandType seen path (ForAllTy _tv t) = expandType seen path t

    papp :: [TyVar] -> [Type] -> ([(TyVar, Type)], Either [TyVar] [Type])
    papp []       tys      = ([], Right tys)
    papp tvs      []       = ([], Left tvs)
    papp (tv:tvs) (ty:tys) = ((tv, ty):env, remainder)
      where (env, remainder) = papp tvs tys

{-
************************************************************************
*                                                                      *
        Deciding which type constructors are recursive
*                                                                      *
************************************************************************

Identification of recursive TyCons
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
The knot-tying parameters: @rec_details_list@ is an alist mapping @Name@s to
@TyThing@s.

Identifying a TyCon as recursive serves two purposes

1.  Avoid infinite types.  Non-recursive newtypes are treated as
"transparent", like type synonyms, after the type checker.  If we did
this for all newtypes, we'd get infinite types.  So we figure out for
each newtype whether it is "recursive", and add a coercion if so.  In
effect, we are trying to "cut the loops" by identifying a loop-breaker.

2.  Avoid infinite unboxing.  This has nothing to do with newtypes.
Suppose we have
        data T = MkT Int T
        f (MkT x t) = f t
Well, this function diverges, but we don't want the strictness analyser
to diverge.  But the strictness analyser will diverge because it looks
deeper and deeper into the structure of T.   (I believe there are
examples where the function does something sane, and the strictness
analyser still diverges, but I can't see one now.)

Now, concerning (1), the FC2 branch currently adds a coercion for ALL
newtypes.  I did this as an experiment, to try to expose cases in which
the coercions got in the way of optimisations.  If it turns out that we
can indeed always use a coercion, then we don't risk recursive types,
and don't need to figure out what the loop breakers are.

For newtype *families* though, we will always have a coercion, so they
are always loop breakers!  So you can easily adjust the current
algorithm by simply treating all newtype families as loop breakers (and
indeed type families).  I think.



For newtypes, we label some as "recursive" such that

    INVARIANT: there is no cycle of non-recursive newtypes

In any loop, only one newtype need be marked as recursive; it is
a "loop breaker".  Labelling more than necessary as recursive is OK,
provided the invariant is maintained.

A newtype M.T is defined to be "recursive" iff
        (a) it is declared in an hi-boot file (see RdrHsSyn.hsIfaceDecl)
        (b) it is declared in a source file, but that source file has a
            companion hi-boot file which declares the type
   or   (c) one can get from T's rhs to T via type
            synonyms, or non-recursive newtypes *in M*
             e.g.  newtype T = MkT (T -> Int)

(a) is conservative; declarations in hi-boot files are always
        made loop breakers. That's why in (b) we can restrict attention
        to tycons in M, because any loops through newtypes outside M
        will be broken by those newtypes
(b) ensures that a newtype is not treated as a loop breaker in one place
and later as a non-loop-breaker.  This matters in GHCi particularly, when
a newtype T might be embedded in many types in the environment, and then
T's source module is compiled.  We don't want T's recursiveness to change.

The "recursive" flag for algebraic data types is irrelevant (never consulted)
for types with more than one constructor.


An algebraic data type M.T is "recursive" iff
        it has just one constructor, and
        (a) it is declared in an hi-boot file (see RdrHsSyn.hsIfaceDecl)
        (b) it is declared in a source file, but that source file has a
            companion hi-boot file which declares the type
 or     (c) one can get from its arg types to T via type synonyms,
            or by non-recursive newtypes or non-recursive product types in M
             e.g.  data T = MkT (T -> Int) Bool
Just like newtype in fact

A type synonym is recursive if one can get from its
right hand side back to it via type synonyms.  (This is
reported as an error.)

A class is recursive if one can get from its superclasses
back to it.  (This is an error too.)

Hi-boot types
~~~~~~~~~~~~~
A data type read from an hi-boot file will have an AbstractTyCon as its AlgTyConRhs
and will respond True to isAbstractTyCon. The idea is that we treat these as if one
could get from these types to anywhere.  So when we see

        module Baz where
        import {-# SOURCE #-} Foo( T )
        newtype S = MkS T

then we mark S as recursive, just in case. What that means is that if we see

        import Baz( S )
        newtype R = MkR S

then we don't need to look inside S to compute R's recursiveness.  Since S is imported
(not from an hi-boot file), one cannot get from R back to S except via an hi-boot file,
and that means that some data type will be marked recursive along the way.  So R is
unconditionly non-recursive (i.e. there'll be a loop breaker elsewhere if necessary)

This in turn means that we grovel through fewer interface files when computing
recursiveness, because we need only look at the type decls in the module being
compiled, plus the outer structure of directly-mentioned types.
-}

data RecTyInfo = RTI { rti_promotable :: Bool
                     , rti_roles      :: Name -> [Role]
                     , rti_is_rec     :: Name -> RecFlag }

calcRecFlags :: SelfBootInfo -> Bool  -- hs-boot file?
             -> RoleAnnots -> [TyCon] -> RecTyInfo
-- The 'boot_names' are the things declared in M.hi-boot, if M is the current module.
-- Any type constructors in boot_names are automatically considered loop breakers
-- Recursion of newtypes/data types can happen via
-- the class TyCon, so all_tycons includes the class tycons
calcRecFlags boot_details is_boot mrole_env all_tycons
  = RTI { rti_promotable = is_promotable
        , rti_roles      = roles
        , rti_is_rec     = is_rec }
  where
    rec_tycon_names = mkNameSet (map tyConName all_tycons)

    is_promotable = all (computeTyConPromotability rec_tycon_names) all_tycons

    roles = inferRoles is_boot mrole_env all_tycons

    ----------------- Recursion calculation ----------------
    is_rec n | n `elemNameSet` rec_names = Recursive
             | otherwise                 = NonRecursive

    boot_name_set = case boot_details of
                      NoSelfBoot                -> emptyNameSet
                      SelfBoot { sb_tcs = tcs } -> tcs
    rec_names = boot_name_set     `unionNameSet`
                nt_loop_breakers  `unionNameSet`
                prod_loop_breakers


        -------------------------------------------------
        --                      NOTE
        -- These edge-construction loops rely on
        -- every loop going via tyclss, the types and classes
        -- in the module being compiled.  Stuff in interface
        -- files should be correctly marked.  If not (e.g. a
        -- type synonym in a hi-boot file) we can get an infinite
        -- loop.  We could program round this, but it'd make the code
        -- rather less nice, so I'm not going to do that yet.

    single_con_tycons = [ tc | tc <- all_tycons
                             , not (tyConName tc `elemNameSet` boot_name_set)
                                 -- Remove the boot_name_set because they are
                                 -- going to be loop breakers regardless.
                             , isSingleton (tyConDataCons tc) ]
        -- Both newtypes and data types, with exactly one data constructor

    (new_tycons, prod_tycons) = partition isNewTyCon single_con_tycons
        -- NB: we do *not* call isProductTyCon because that checks
        --     for vanilla-ness of data constructors; and that depends
        --     on empty existential type variables; and that is figured
        --     out by tcResultType; which uses tcMatchTy; which uses
        --     coreView; which calls expandSynTyCon_maybe; which uses
        --     the recursiveness of the TyCon.  Result... a black hole.
        -- YUK YUK YUK

        --------------- Newtypes ----------------------
    nt_loop_breakers = mkNameSet (findLoopBreakers nt_edges)
    is_rec_nt tc = tyConName tc  `elemNameSet` nt_loop_breakers
        -- is_rec_nt is a locally-used helper function

    nt_edges = [(t, mk_nt_edges t) | t <- new_tycons]

    mk_nt_edges nt      -- Invariant: nt is a newtype
        = [ tc | tc <- nameEnvElts (tyConsOfType (new_tc_rhs nt))
                        -- tyConsOfType looks through synonyms
               , tc `elem` new_tycons ]
           -- If not (tc `elem` new_tycons) we know that either it's a local *data* type,
           -- or it's imported.  Either way, it can't form part of a newtype cycle

        --------------- Product types ----------------------
    prod_loop_breakers = mkNameSet (findLoopBreakers prod_edges)

    prod_edges = [(tc, mk_prod_edges tc) | tc <- prod_tycons]

    mk_prod_edges tc    -- Invariant: tc is a product tycon
        = concatMap (mk_prod_edges1 tc) (dataConOrigArgTys (head (tyConDataCons tc)))

    mk_prod_edges1 ptc ty = concatMap (mk_prod_edges2 ptc) (nameEnvElts (tyConsOfType ty))

    mk_prod_edges2 ptc tc
        | tc `elem` prod_tycons   = [tc]                -- Local product
        | tc `elem` new_tycons    = if is_rec_nt tc     -- Local newtype
                                    then []
                                    else mk_prod_edges1 ptc (new_tc_rhs tc)
                -- At this point we know that either it's a local non-product data type,
                -- or it's imported.  Either way, it can't form part of a cycle
        | otherwise = []

new_tc_rhs :: TyCon -> Type
new_tc_rhs tc = snd (newTyConRhs tc)    -- Ignore the type variables

findLoopBreakers :: [(TyCon, [TyCon])] -> [Name]
-- Finds a set of tycons that cut all loops
findLoopBreakers deps
  = go [(tc,tc,ds) | (tc,ds) <- deps]
  where
    go edges = [ name
               | CyclicSCC ((tc,_,_) : edges') <- stronglyConnCompFromEdgedVerticesR edges,
                 name <- tyConName tc : go edges']

{-
************************************************************************
*                                                                      *
        Role annotations
*                                                                      *
************************************************************************
-}

type RoleAnnots = NameEnv (LRoleAnnotDecl Name)

extractRoleAnnots :: TyClGroup Name -> RoleAnnots
extractRoleAnnots (TyClGroup { group_roles = roles })
  = mkNameEnv [ (tycon, role_annot)
              | role_annot@(L _ (RoleAnnotDecl (L _ tycon) _)) <- roles ]

emptyRoleAnnots :: RoleAnnots
emptyRoleAnnots = emptyNameEnv

lookupRoleAnnots :: RoleAnnots -> Name -> Maybe (LRoleAnnotDecl Name)
lookupRoleAnnots = lookupNameEnv

{-
************************************************************************
*                                                                      *
        Role inference
*                                                                      *
************************************************************************

Note [Role inference]
~~~~~~~~~~~~~~~~~~~~~
The role inference algorithm datatype definitions to infer the roles on the
parameters. Although these roles are stored in the tycons, we can perform this
algorithm on the built tycons, as long as we don't peek at an as-yet-unknown
roles field! Ah, the magic of laziness.

First, we choose appropriate initial roles. For families and classes, roles
(including initial roles) are N. For datatypes, we start with the role in the
role annotation (if any), or otherwise use Phantom. This is done in
initialRoleEnv1.

The function irGroup then propagates role information until it reaches a
fixpoint, preferring N over (R or P) and R over P. To aid in this, we have a
monad RoleM, which is a combination reader and state monad. In its state are
the current RoleEnv, which gets updated by role propagation, and an update
bit, which we use to know whether or not we've reached the fixpoint. The
environment of RoleM contains the tycon whose parameters we are inferring, and
a VarEnv from parameters to their positions, so we can update the RoleEnv.
Between tycons, this reader information is missing; it is added by
addRoleInferenceInfo.

There are two kinds of tycons to consider: algebraic ones (excluding classes)
and type synonyms. (Remember, families don't participate -- all their parameters
are N.) An algebraic tycon processes each of its datacons, in turn. Note that
a datacon's universally quantified parameters might be different from the parent
tycon's parameters, so we use the datacon's univ parameters in the mapping from
vars to positions. Note also that we don't want to infer roles for existentials
(they're all at N, too), so we put them in the set of local variables. As an
optimisation, we skip any tycons whose roles are already all Nominal, as there
nowhere else for them to go. For synonyms, we just analyse their right-hand sides.

irType walks through a type, looking for uses of a variable of interest and
propagating role information. Because anything used under a phantom position
is at phantom and anything used under a nominal position is at nominal, the
irType function can assume that anything it sees is at representational. (The
other possibilities are pruned when they're encountered.)

The rest of the code is just plumbing.

How do we know that this algorithm is correct? It should meet the following
specification:

Let Z be a role context -- a mapping from variables to roles. The following
rules define the property (Z |- t : r), where t is a type and r is a role:

Z(a) = r'        r' <= r
------------------------- RCVar
Z |- a : r

---------- RCConst
Z |- T : r               -- T is a type constructor

Z |- t1 : r
Z |- t2 : N
-------------- RCApp
Z |- t1 t2 : r

forall i<=n. (r_i is R or N) implies Z |- t_i : r_i
roles(T) = r_1 .. r_n
---------------------------------------------------- RCDApp
Z |- T t_1 .. t_n : R

Z, a:N |- t : r
---------------------- RCAll
Z |- forall a:k.t : r


We also have the following rules:

For all datacon_i in type T, where a_1 .. a_n are universally quantified
and b_1 .. b_m are existentially quantified, and the arguments are t_1 .. t_p,
then if forall j<=p, a_1 : r_1 .. a_n : r_n, b_1 : N .. b_m : N |- t_j : R,
then roles(T) = r_1 .. r_n

roles(->) = R, R
roles(~#) = N, N

With -dcore-lint on, the output of this algorithm is checked in checkValidRoles,
called from checkValidTycon.

Note [Role-checking data constructor arguments]
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Consider
  data T a where
    MkT :: Eq b => F a -> (a->a) -> T (G a)

Then we want to check the roles at which 'a' is used
in MkT's type.  We want to work on the user-written type,
so we need to take into account
  * the arguments:   (F a) and (a->a)
  * the context:     C a b
  * the result type: (G a)   -- this is in the eq_spec
-}

type RoleEnv    = NameEnv [Role]        -- from tycon names to roles

-- This, and any of the functions it calls, must *not* look at the roles
-- field of a tycon we are inferring roles about!
-- See Note [Role inference]
inferRoles :: Bool -> RoleAnnots -> [TyCon] -> Name -> [Role]
inferRoles is_boot annots tycons
  = let role_env  = initialRoleEnv is_boot annots tycons
        role_env' = irGroup role_env tycons in
    \name -> case lookupNameEnv role_env' name of
      Just roles -> roles
      Nothing    -> pprPanic "inferRoles" (ppr name)

initialRoleEnv :: Bool -> RoleAnnots -> [TyCon] -> RoleEnv
initialRoleEnv is_boot annots = extendNameEnvList emptyNameEnv .
                                map (initialRoleEnv1 is_boot annots)

initialRoleEnv1 :: Bool -> RoleAnnots -> TyCon -> (Name, [Role])
initialRoleEnv1 is_boot annots_env tc
  | isFamilyTyCon tc      = (name, map (const Nominal) tyvars)
  | isAlgTyCon tc         = (name, default_roles)
  | isTypeSynonymTyCon tc = (name, default_roles)
  | otherwise             = pprPanic "initialRoleEnv1" (ppr tc)
  where name         = tyConName tc
        tyvars       = tyConTyVars tc
        (kvs, tvs)   = span isKindVar tyvars

          -- if the number of annotations in the role annotation decl
          -- is wrong, just ignore it. We check this in the validity check.
        role_annots
          = case lookupNameEnv annots_env name of
              Just (L _ (RoleAnnotDecl _ annots))
                | annots `equalLength` tvs -> map unLoc annots
              _                            -> map (const Nothing) tvs
        default_roles = map (const Nominal) kvs ++
                        zipWith orElse role_annots (repeat default_role)

        default_role
          | isClassTyCon tc               = Nominal
          | is_boot && isAbstractTyCon tc = Representational
          | otherwise                     = Phantom

irGroup :: RoleEnv -> [TyCon] -> RoleEnv
irGroup env tcs
  = let (env', update) = runRoleM env $ mapM_ irTyCon tcs in
    if update
    then irGroup env' tcs
    else env'

irTyCon :: TyCon -> RoleM ()
irTyCon tc
  | isAlgTyCon tc
  = do { old_roles <- lookupRoles tc
       ; unless (all (== Nominal) old_roles) $  -- also catches data families,
                                                -- which don't want or need role inference
    do { whenIsJust (tyConClass_maybe tc) (irClass tc_name)
       ; addRoleInferenceInfo tc_name (tyConTyVars tc) $
         mapM_ (irType emptyVarSet) (tyConStupidTheta tc)  -- See #8958
       ; mapM_ (irDataCon tc_name) (visibleDataCons $ algTyConRhs tc) }}

  | Just ty <- synTyConRhs_maybe tc
  = addRoleInferenceInfo tc_name (tyConTyVars tc) $
    irType emptyVarSet ty

  | otherwise
  = return ()

  where
    tc_name = tyConName tc

-- any type variable used in an associated type must be Nominal
irClass :: Name -> Class -> RoleM ()
irClass tc_name cls
  = addRoleInferenceInfo tc_name cls_tvs $
    mapM_ ir_at (classATs cls)
  where
    cls_tvs    = classTyVars cls
    cls_tv_set = mkVarSet cls_tvs

    ir_at at_tc
      = mapM_ (updateRole Nominal) (varSetElems nvars)
      where nvars = (mkVarSet $ tyConTyVars at_tc) `intersectVarSet` cls_tv_set

-- See Note [Role inference]
irDataCon :: Name -> DataCon -> RoleM ()
irDataCon tc_name datacon
  = addRoleInferenceInfo tc_name univ_tvs $
    mapM_ (irType ex_var_set) (eqSpecPreds eq_spec ++ theta ++ arg_tys)
      -- See Note [Role-checking data constructor arguments]
  where
    (univ_tvs, ex_tvs, eq_spec, theta, arg_tys, _res_ty) = dataConFullSig datacon
    ex_var_set = mkVarSet ex_tvs

irType :: VarSet -> Type -> RoleM ()
irType = go
  where
    go lcls (TyVarTy tv) = unless (tv `elemVarSet` lcls) $
                           updateRole Representational tv
    go lcls (AppTy t1 t2) = go lcls t1 >> mark_nominal lcls t2
    go lcls (TyConApp tc tys)
      = do { roles <- lookupRolesX tc
           ; zipWithM_ (go_app lcls) roles tys }
    go lcls (FunTy t1 t2) = go lcls t1 >> go lcls t2
    go lcls (ForAllTy tv ty) = go (extendVarSet lcls tv) ty
    go _    (LitTy {}) = return ()

    go_app _ Phantom _ = return ()                 -- nothing to do here
    go_app lcls Nominal ty = mark_nominal lcls ty  -- all vars below here are N
    go_app lcls Representational ty = go lcls ty

    mark_nominal lcls ty = let nvars = tyVarsOfType ty `minusVarSet` lcls in
                           mapM_ (updateRole Nominal) (varSetElems nvars)

-- like lookupRoles, but with Nominal tags at the end for oversaturated TyConApps
lookupRolesX :: TyCon -> RoleM [Role]
lookupRolesX tc
  = do { roles <- lookupRoles tc
       ; return $ roles ++ repeat Nominal }

-- gets the roles either from the environment or the tycon
lookupRoles :: TyCon -> RoleM [Role]
lookupRoles tc
  = do { env <- getRoleEnv
       ; case lookupNameEnv env (tyConName tc) of
           Just roles -> return roles
           Nothing    -> return $ tyConRoles tc }

-- tries to update a role; won't ever update a role "downwards"
updateRole :: Role -> TyVar -> RoleM ()
updateRole role tv
  = do { var_ns <- getVarNs
       ; case lookupVarEnv var_ns tv of
       { Nothing -> pprPanic "updateRole" (ppr tv)
       ; Just n  -> do
       { name <- getTyConName
       ; updateRoleEnv name n role }}}

-- the state in the RoleM monad
data RoleInferenceState = RIS { role_env  :: RoleEnv
                              , update    :: Bool }

-- the environment in the RoleM monad
type VarPositions = VarEnv Int
data RoleInferenceInfo = RII { var_ns :: VarPositions
                             , name   :: Name }

-- See [Role inference]
newtype RoleM a = RM { unRM :: Maybe RoleInferenceInfo
                            -> RoleInferenceState
                            -> (a, RoleInferenceState) }

instance Functor RoleM where
    fmap = liftM

instance Applicative RoleM where
    pure x = RM $ \_ state -> (x, state)
    (<*>) = ap

instance Monad RoleM where
  return = pure
  a >>= f  = RM $ \m_info state -> let (a', state') = unRM a m_info state in
                                   unRM (f a') m_info state'

runRoleM :: RoleEnv -> RoleM () -> (RoleEnv, Bool)
runRoleM env thing = (env', update)
  where RIS { role_env = env', update = update } = snd $ unRM thing Nothing state
        state = RIS { role_env  = env, update    = False }

addRoleInferenceInfo :: Name -> [TyVar] -> RoleM a -> RoleM a
addRoleInferenceInfo name tvs thing
  = RM $ \_nothing state -> ASSERT( isNothing _nothing )
                            unRM thing (Just info) state
  where info = RII { var_ns = mkVarEnv (zip tvs [0..]), name = name }

getRoleEnv :: RoleM RoleEnv
getRoleEnv = RM $ \_ state@(RIS { role_env = env }) -> (env, state)

getVarNs :: RoleM VarPositions
getVarNs = RM $ \m_info state ->
                case m_info of
                  Nothing -> panic "getVarNs"
                  Just (RII { var_ns = var_ns }) -> (var_ns, state)

getTyConName :: RoleM Name
getTyConName = RM $ \m_info state ->
                    case m_info of
                      Nothing -> panic "getTyConName"
                      Just (RII { name = name }) -> (name, state)


updateRoleEnv :: Name -> Int -> Role -> RoleM ()
updateRoleEnv name n role
  = RM $ \_ state@(RIS { role_env = role_env }) -> ((),
         case lookupNameEnv role_env name of
           Nothing -> pprPanic "updateRoleEnv" (ppr name)
           Just roles -> let (before, old_role : after) = splitAt n roles in
                         if role `ltRole` old_role
                         then let roles' = before ++ role : after
                                  role_env' = extendNameEnv role_env name roles' in
                              RIS { role_env = role_env', update = True }
                         else state )


{- *********************************************************************
*                                                                      *
                Building implicits
*                                                                      *
********************************************************************* -}

tcAddImplicits :: [TyCon] -> TcM TcGblEnv
-- Given a [TyCon], add to the TcGblEnv
--   * extend the TypeEnv with their implicitTyThings
--   * extend the TypeEnv with any default method Ids
--   * add bindings for record selectors
--   * add bindings for type representations for the TyThings
tcAddImplicits tycons
  = discardWarnings $
    tcExtendGlobalEnvImplicit implicit_things  $
    tcExtendGlobalValEnv def_meth_ids          $
    do { traceTc "tcAddImplicits" $ vcat
            [ text "tycons" <+> ppr tycons
            , text "implicits" <+> ppr implicit_things ]
       ; gbl_env <- mkTypeableBinds tycons
       ; gbl_env <- setGblEnv gbl_env $
                    tcRecSelBinds (mkRecSelBinds tycons)
       ; return gbl_env }
 where
   implicit_things = concatMap implicitTyConThings tycons
   def_meth_ids    = mkDefaultMethodIds tycons

mkDefaultMethodIds :: [TyCon] -> [Id]
-- We want to put the default-method Ids (both vanilla and generic)
-- into the type environment so that they are found when we typecheck
-- the filled-in default methods of each instance declaration
-- See Note [Default method Ids and Template Haskell]
mkDefaultMethodIds tycons
  = [ mkExportedLocalId VanillaId dm_name (mk_dm_ty cls sel_id dm_spec)
    | tc <- tycons
    , Just cls <- [tyConClass_maybe tc]
    , (sel_id, Just (dm_name, dm_spec)) <- classOpItems cls ]
  where
    mk_dm_ty :: Class -> Id -> DefMethSpec Type -> Type
    mk_dm_ty _ sel_id VanillaDM        = idType sel_id
    mk_dm_ty cls _   (GenericDM dm_ty) = mkSigmaTy cls_tvs [pred] dm_ty
       where
         cls_tvs = classTyVars cls
         pred    = mkClassPred cls (mkTyVarTys cls_tvs)

{-
************************************************************************
*                                                                      *
                Building record selectors
*                                                                      *
************************************************************************
-}

{-
Note [Default method Ids and Template Haskell]
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Consider this (Trac #4169):
   class Numeric a where
     fromIntegerNum :: a
     fromIntegerNum = ...

   ast :: Q [Dec]
   ast = [d| instance Numeric Int |]

When we typecheck 'ast' we have done the first pass over the class decl
(in tcTyClDecls), but we have not yet typechecked the default-method
declarations (because they can mention value declarations).  So we
must bring the default method Ids into scope first (so they can be seen
when typechecking the [d| .. |] quote, and typecheck them later.
-}

{-
************************************************************************
*                                                                      *
                Building record selectors
*                                                                      *
************************************************************************
-}

mkRecSelBinds :: [TyCon] -> HsValBinds Name
-- NB We produce *un-typechecked* bindings, rather like 'deriving'
--    This makes life easier, because the later type checking will add
--    all necessary type abstractions and applications
mkRecSelBinds tycons
  = ValBindsOut binds sigs
  where
    (sigs, binds) = unzip rec_sels
    rec_sels = map mkRecSelBind [ (tc,fld)
                                | tc <- tycons
                                , fld <- tyConFieldLabels tc ]

mkRecSelBind :: (TyCon, FieldLabel) -> (LSig Name, (RecFlag, LHsBinds Name))
mkRecSelBind (tycon, fl)
  = mkOneRecordSelector all_cons (RecSelData tycon) fl
  where
    all_cons     = map RealDataCon (tyConDataCons tycon)

mkOneRecordSelector :: [ConLike] -> RecSelParent -> FieldLabel
                    -> (LSig Name, (RecFlag, LHsBinds Name))
mkOneRecordSelector all_cons idDetails fl
  = (L loc (IdSig sel_id), (NonRecursive, unitBag (L loc sel_bind)))
  where
    loc    = getSrcSpan sel_name
    lbl      = flLabel fl
    sel_name = flSelector fl

    sel_id = mkExportedLocalId rec_details sel_name sel_ty
    rec_details = RecSelId { sel_tycon = idDetails, sel_naughty = is_naughty }

    -- Find a representative constructor, con1
    cons_w_field = conLikesWithFields all_cons [lbl]
    con1 = ASSERT( not (null cons_w_field) ) head cons_w_field

    -- Selector type; Note [Polymorphic selectors]
    field_ty   = conLikeFieldType con1 lbl
    data_tvs   = tyVarsOfType data_ty
    is_naughty = not (tyVarsOfType field_ty `subVarSet` data_tvs)
    (field_tvs, field_theta, field_tau) = tcSplitSigmaTy field_ty
    sel_ty | is_naughty = unitTy  -- See Note [Naughty record selectors]
           | otherwise  = mkForAllTys (varSetElemsKvsFirst $
                                       data_tvs `extendVarSetList` field_tvs) $
                          mkPhiTy (conLikeStupidTheta con1) $   -- Urgh!
                          mkPhiTy field_theta               $   -- Urgh!
                          -- req_theta is empty for normal DataCon
                          mkPhiTy req_theta                 $
                          mkFunTy data_ty field_tau

    -- Make the binding: sel (C2 { fld = x }) = x
    --                   sel (C7 { fld = x }) = x
    --    where cons_w_field = [C2,C7]
    sel_bind = mkTopFunBind Generated sel_lname alts
      where
        alts | is_naughty = [mkSimpleMatch [] unit_rhs]
             | otherwise =  map mk_match cons_w_field ++ deflt
    mk_match con = mkSimpleMatch [L loc (mk_sel_pat con)]
                                 (L loc (HsVar (L loc field_var)))
    mk_sel_pat con = ConPatIn (L loc (getName con)) (RecCon rec_fields)
    rec_fields = HsRecFields { rec_flds = [rec_field], rec_dotdot = Nothing }
    rec_field  = noLoc (HsRecField
                        { hsRecFieldLbl = L loc (FieldOcc (mkVarUnqual lbl)
                                                 sel_name)
                        , hsRecFieldArg = L loc (VarPat (L loc field_var))
                        , hsRecPun = False })
    sel_lname = L loc sel_name
    field_var = mkInternalName (mkBuiltinUnique 1) (getOccName sel_name) loc

    -- Add catch-all default case unless the case is exhaustive
    -- We do this explicitly so that we get a nice error message that
    -- mentions this particular record selector
    deflt | all dealt_with all_cons = []
          | otherwise = [mkSimpleMatch [L loc (WildPat placeHolderType)]
                            (mkHsApp (L loc (HsVar
                                            (L loc (getName rEC_SEL_ERROR_ID))))
                                     (L loc (HsLit msg_lit)))]

        -- Do not add a default case unless there are unmatched
        -- constructors.  We must take account of GADTs, else we
        -- get overlap warning messages from the pattern-match checker
        -- NB: we need to pass type args for the *representation* TyCon
        --     to dataConCannotMatch, hence the calculation of inst_tys
        --     This matters in data families
        --              data instance T Int a where
        --                 A :: { fld :: Int } -> T Int Bool
        --                 B :: { fld :: Int } -> T Int Char
    dealt_with :: ConLike -> Bool
    dealt_with (PatSynCon _) = False -- We can't predict overlap
    dealt_with con@(RealDataCon dc) =
      con `elem` cons_w_field || dataConCannotMatch inst_tys dc

    (univ_tvs, _, eq_spec, _, req_theta, _, data_ty) = conLikeFullSig con1

    inst_tys = substTyVars (mkTopTvSubst eq_spec) univ_tvs

    unit_rhs = mkLHsTupleExpr []
    msg_lit = HsStringPrim "" (fastStringToByteString lbl)

{-
Note [Polymorphic selectors]
~~~~~~~~~~~~~~~~~~~~~~~~~~~~
When a record has a polymorphic field, we pull the foralls out to the front.
   data T = MkT { f :: forall a. [a] -> a }
Then f :: forall a. T -> [a] -> a
NOT  f :: T -> forall a. [a] -> a

This is horrid.  It's only needed in deeply obscure cases, which I hate.
The only case I know is test tc163, which is worth looking at.  It's far
from clear that this test should succeed at all!

Note [Naughty record selectors]
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
A "naughty" field is one for which we can't define a record
selector, because an existential type variable would escape.  For example:
        data T = forall a. MkT { x,y::a }
We obviously can't define
        x (MkT v _) = v
Nevertheless we *do* put a RecSelId into the type environment
so that if the user tries to use 'x' as a selector we can bleat
helpfully, rather than saying unhelpfully that 'x' is not in scope.
Hence the sel_naughty flag, to identify record selectors that don't really exist.

In general, a field is "naughty" if its type mentions a type variable that
isn't in the result type of the constructor.  Note that this *allows*
GADT record selectors (Note [GADT record selectors]) whose types may look
like     sel :: T [a] -> a

For naughty selectors we make a dummy binding
   sel = ()
so that the later type-check will add them to the environment, and they'll be
exported.  The function is never called, because the typechecker spots the
sel_naughty field.

Note [GADT record selectors]
~~~~~~~~~~~~~~~~~~~~~~~~~~~~
For GADTs, we require that all constructors with a common field 'f' have the same
result type (modulo alpha conversion).  [Checked in TcTyClsDecls.checkValidTyCon]
E.g.
        data T where
          T1 { f :: Maybe a } :: T [a]
          T2 { f :: Maybe a, y :: b  } :: T [a]
          T3 :: T Int

and now the selector takes that result type as its argument:
   f :: forall a. T [a] -> Maybe a

Details: the "real" types of T1,T2 are:
   T1 :: forall r a.   (r~[a]) => a -> T r
   T2 :: forall r a b. (r~[a]) => a -> b -> T r

So the selector loooks like this:
   f :: forall a. T [a] -> Maybe a
   f (a:*) (t:T [a])
     = case t of
         T1 c   (g:[a]~[c]) (v:Maybe c)       -> v `cast` Maybe (right (sym g))
         T2 c d (g:[a]~[c]) (v:Maybe c) (w:d) -> v `cast` Maybe (right (sym g))
         T3 -> error "T3 does not have field f"

Note the forall'd tyvars of the selector are just the free tyvars
of the result type; there may be other tyvars in the constructor's
type (e.g. 'b' in T2).

Note the need for casts in the result!

Note [Selector running example]
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
It's OK to combine GADTs and type families.  Here's a running example:

        data instance T [a] where
          T1 { fld :: b } :: T [Maybe b]

The representation type looks like this
        data :R7T a where
          T1 { fld :: b } :: :R7T (Maybe b)

and there's coercion from the family type to the representation type
        :CoR7T a :: T [a] ~ :R7T a

The selector we want for fld looks like this:

        fld :: forall b. T [Maybe b] -> b
        fld = /\b. \(d::T [Maybe b]).
              case d `cast` :CoR7T (Maybe b) of
                T1 (x::b) -> x

The scrutinee of the case has type :R7T (Maybe b), which can be
gotten by appying the eq_spec to the univ_tvs of the data con.

-}
