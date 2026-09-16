defmodule Reads do
 import Enum
 alias File, as: Input
 def normalize(x), do: abs(x)
 def total(xs), do: map(xs, &normalize/1)
end
