library(methods)
require(stats)
source("helpers.R")
normalize <- function(
    x, scale = 2) {
    helper(x) / scale
}
apply_normalize = function(x) normalize(x)
ReadSet <- setClass("ReadSet", contains = "BaseSet")
Runner <- R6::R6Class("Runner", inherit = BaseRunner,
    public = list(run = function(x) normalize(x)))
# phantom <- function(x) helper(x)
text <- "phantom <- function(x) helper(x)"
lapply(1:3, FUN = function(x) x)
