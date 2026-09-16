const std = @import("std");
const Counts = struct {
 value: u32,
 pub fn total(self: Counts) u32 { return normalize(self.value); }
};
fn normalize(x: u32) u32 { return x + 1; }
pub fn main() void { const x = normalize(1); _ = x; }
