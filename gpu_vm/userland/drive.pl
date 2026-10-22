#!/usr/bin/perl
# systest.sh's typist: runs the emulator (the arguments after the two files), prints its output (stdout and stderr)
# with each line's wall time since the start, writes the wall time of each shell prompt to PROMPTS, and types INPUT's
# lines into the emulator's stdin: the first line at the first prompt, the others at the second, then stdin's end.
#   perl drive.pl PROMPTS INPUT EMULATOR [ARGS...]      exit code: the emulator's
# (Nothing is typed before the shell reads it: what came while init started was echoed by the tty into the boot's
# output, and what the line editor had read past its line was lost once `stty -echo` gave the shell plain reads.)
use strict;
use warnings;
use IPC::Open3;
use Time::HiRes qw(time);

my ($prompts, $input, @cmd) = @ARGV;
open(my $pt, '>', $prompts) or die "drive.pl: $prompts: $!";
open(my $inf, '<', $input) or die "drive.pl: $input: $!";
my @lines = <$inf>;
close $inf;
$| = 1;
my $t0 = time;
my $pid = open3(my $to, my $from, undef, @cmd);
binmode $from;
binmode $to;
my ($buf, $tail, $bol, $seen) = ('', '', 1, 0);
sub type_lines { my $s = join('', @_); syswrite($to, $s) if length $s; }
while (sysread($from, $buf, 4096)) {
    my $t = time - $t0;
    $buf =~ s/\r//g;
    $tail .= $buf;
    while ($tail =~ s/^.*?\@gpuvm:[^\n]*?# //s) {
        printf $pt "%.3f\n", $t;
        $seen++;
        if ($seen == 1) { type_lines(shift @lines) if @lines; close $to unless @lines; }
        elsif ($seen == 2 && $to->opened) { type_lines(@lines); @lines = (); close $to; }
    }
    $tail = substr($tail, -64) if length($tail) > 64;
    for my $c (split /(?<=\n)/, $buf) { printf "%9.3f ", $t if $bol; print $c; $bol = ($c =~ /\n$/); }
}
waitpid($pid, 0);
exit($? >> 8);
