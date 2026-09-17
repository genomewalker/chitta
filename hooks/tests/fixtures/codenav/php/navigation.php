<?php
namespace Reads;
require 'config.php';
class BaseReads {}
class Counts extends BaseReads {
 public function total($xs) { return normalize($xs); }
}
function normalize($xs) { return count($xs); }
