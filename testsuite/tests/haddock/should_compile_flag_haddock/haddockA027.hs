module ShouldCompile where

test :: [a] -- ^ doc1 
        -> forall b. (Ord b) => [b] {-^ doc2 -} 
        -> forall c. (Num c) => [c] -- ^ doc3
        -> [a]
test xs ys zs = xs
