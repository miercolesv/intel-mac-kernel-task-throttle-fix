#!/usr/bin/perl
# xcpm.pl - read and override XCPM thermal throttling through /dev/xcpm
#
# Usage (as root):
#   sudo perl xcpm.pl status        show thermal levels, CPU limits, forced idle
#   sudo perl xcpm.pl unthrottle    remove the 800 MHz cap and forced idle once
#   sudo perl xcpm.pl watch [secs]  re-check every N seconds (default 2), re-apply
#                                   whenever something throttles again; Ctrl+C stops
#   sudo perl xcpm.pl daemon [secs] same as watch (default 10s), but only logs when it
#                                   has to re-apply; meant for the launchd job
#
# ioctl numbers and struct layouts come from xcpm_cpu_control / bsd_xcpm_ioctl
# in the macOS 15.7.9 (24G830) kernel.

use strict;
use warnings;
use Fcntl;

$| = 1;  # print immediately

my %IOC = (
    get_levels      => 0xc0045815,  # 3 bytes: effective cpu/gpu/io thermal level
    get_levels_b    => 0xc0045819,  # 3 bytes: second level source
    get_hard_plimit => 0xc0085802,  # u32 max MHz, u32 min MHz
    set_hard_plimit => 0xc0085803,  # u32 max MHz, u32 min MHz (multiples of 100)
    get_forced_idle => 0xc008580a,  # u32 ratio (100 = off), u32 period us
    set_forced_idle => 0xc008580b,  # u32 ratio (<=100), u32 period us (<=1000000)
    get_soft_plimit => 0xc0085811,  # u32 max MHz, u32 min MHz
);

die "Run as root: sudo perl $0 @ARGV\n" if $> != 0;
sysopen(my $fh, "/dev/xcpm", O_RDWR) or die "Cannot open /dev/xcpm: $!\n";

sub xioctl {
    my ($name, $buf) = @_;
    ioctl($fh, $IOC{$name}, $buf) or die "ioctl $name failed: $!\n";
    return $buf;
}

sub get2 { my $b = xioctl($_[0], pack('LL', 0, 0)); return unpack('LL', $b); }
sub set2 { xioctl($_[0], pack('LL', $_[1], $_[2])); }
sub levels { my $b = xioctl($_[0], "\0" x 4); return unpack('CCC', $b); }

sub ts { my @t = localtime; sprintf("%02d:%02d:%02d", @t[2, 1, 0]); }

sub status {
    my ($c, $g, $i)    = levels('get_levels');
    my ($cb, $gb, $ib) = levels('get_levels_b');
    my ($hmax, $hmin)  = get2('get_hard_plimit');
    my ($smax, $smin)  = get2('get_soft_plimit');
    my ($fr, $fp)      = get2('get_forced_idle');
    print "[", ts(), "] thermal level cpu=$c gpu=$g io=$i (source B: $cb/$gb/$ib)\n";
    print "           hard limit ${hmin}-${hmax} MHz | soft limit ${smin}-${smax} MHz\n";
    print "           forced idle ratio $fr% over ${fp}us", ($fr == 100 ? " (off)" : " (ACTIVE)"), "\n";
    return { hmax => $hmax, hmin => $hmin, smax => $smax, smin => $smin, fr => $fr, fp => $fp };
}

sub unthrottle {
    my ($s, $quiet) = @_;
    my $did = 0;
    if ($s->{hmax} < $s->{smax}) {
        print "[", ts(), "] raising hard limit $s->{hmax} -> $s->{smax} MHz\n";
        set2('set_hard_plimit', $s->{smax}, $s->{smin});
        $did = 1;
    }
    if ($s->{fr} != 100) {
        my $period = $s->{fp} > 0 && $s->{fp} <= 1000000 ? $s->{fp} : 16666;
        print "[", ts(), "] turning forced idle off ($s->{fr}% -> 100%)\n";
        set2('set_forced_idle', 100, $period);
        $did = 1;
    }
    print "[", ts(), "] nothing to change\n" if !$did && !$quiet;
    return $did;
}

my $cmd = shift(@ARGV) // 'status';

if ($cmd eq 'status') {
    status();
} elsif ($cmd eq 'unthrottle') {
    my $s = status();
    unthrottle($s);
    print "--- after ---\n";
    status();
} elsif ($cmd eq 'watch') {
    my $every = shift(@ARGV) // 2;
    print "Watching every ${every}s, Ctrl+C to stop\n";
    my $n = 0;
    while (1) {
        my $s = status();
        my $did = unthrottle($s, 1);
        $n++ if $did;
        print "           re-applied $n time(s) so far\n" if $did;
        sleep $every;
    }
} elsif ($cmd eq 'daemon') {
    # For launchd: log the starting state, then only log when something re-throttles.
    my $every = shift(@ARGV) // 10;
    print "[", ts(), "] xcpm daemon started, checking every ${every}s\n";
    my $s = status();
    unthrottle($s, 1);
    my $n = 0;
    while (1) {
        sleep $every;
        my ($hmax, $hmin) = get2('get_hard_plimit');
        my ($smax, $smin) = get2('get_soft_plimit');
        my ($fr, $fp)     = get2('get_forced_idle');
        my $cur = { hmax => $hmax, hmin => $hmin, smax => $smax, smin => $smin, fr => $fr, fp => $fp };
        if (unthrottle($cur, 1)) {
            $n++;
            print "[", ts(), "] throttle came back and was removed ($n time(s) since start)\n";
        }
    }
} else {
    die "Unknown command '$cmd'. Use: status | unthrottle | watch [secs] | daemon [secs]\n";
}
