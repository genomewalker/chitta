module Reads where
import Data.List (sort)
data Sample = Sample Int
normalize :: Int -> Int
normalize x = abs x
total :: [Int] -> Int
total xs = sum (map normalize (sort xs))
class Countable a where
 count :: a -> Int
instance Countable Sample where
 count (Sample n) = n
