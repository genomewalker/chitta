module Reads
using Statistics
import LinearAlgebra: norm
include("helpers.jl")
abstract type AbstractRead end
struct Read <: AbstractRead
    sequence::String
end
function normalize(x::Vector{Float64})
    norm(x) + helper(x)
end
score(x) = normalize(x)
end
