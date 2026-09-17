open List
module Reads = struct
 type sample = { count : int }
 let normalize x = abs x
 let total xs = fold_left ( + ) 0 (map normalize xs)
end
class base = object end
class counts = object
 inherit base
 method describe xs = Reads.total xs
end
