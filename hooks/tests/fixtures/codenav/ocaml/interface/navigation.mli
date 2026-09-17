open List
module Reads : sig
 type sample = { count : int }
 val normalize : int -> int
 val total : int list -> int
end
class counts : object
 method describe : int list -> int
end
