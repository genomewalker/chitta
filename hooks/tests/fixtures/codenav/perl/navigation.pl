package BaseReads;
sub base_count { return 1; }
package Reads;
use strict;
use warnings;
use Exporter 'import';
use parent 'BaseReads';
require 'helpers.pl';
sub normalize {
    my ($value) = @_;
    return helper($value);
}
sub run { return normalize(1); }
# sub phantom { return 2; }
1;
