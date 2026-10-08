#!/usr/bin/env perl
# log_summary.pl - summarise bacnet_logger output (key=value event lines).
#   perl tools/log_summary.pl events.log      (or pipe: bacnet_logger | tee events.log)
use strict;
use warnings;

my (%count, %reason, %stat);
my $total = 0;

while (my $line = <>) {
    chomp $line;
    my %kv = $line =~ /(\w+)=("[^"]*"|\S+)/g;
    next unless defined $kv{evt};
    $total++;
    $count{ $kv{evt} }++;

    if ( $kv{evt} eq 'bad_packet' && defined $kv{reason} ) {
        ( my $r = $kv{reason} ) =~ s/^"|"$//g;
        $reason{$r}++;
    }
    if ( defined $kv{ai} && defined $kv{value} && $kv{evt} =~ /^(readprop|point_change)$/ ) {
        my $s = $stat{ $kv{ai} } //= { n => 0, sum => 0, min => $kv{value}, max => $kv{value} };
        $s->{n}++;
        $s->{sum} += $kv{value};
        $s->{min} = $kv{value} if $kv{value} < $s->{min};
        $s->{max} = $kv{value} if $kv{value} > $s->{max};
    }
}

print "total events: $total\n";
printf "  %-14s %d\n", $_, $count{$_} for sort keys %count;

if (%reason) {
    print "bad packets by reason:\n";
    printf "  %-40s %d\n", $_, $reason{$_} for sort keys %reason;
}
if (%stat) {
    print "analog-input values:\n";
    for my $ai ( sort { $a <=> $b } keys %stat ) {
        my $s = $stat{$ai};
        printf "  ai=%s n=%d min=%.2f max=%.2f mean=%.2f\n", $ai, $s->{n}, $s->{min}, $s->{max}, $s->{sum} / $s->{n};
    }
}
