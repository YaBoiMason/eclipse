#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License, Version 1.0 only
# (the "License").  You may not use this file except in compliance
# with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#
use 5.7.2;
use strict;
use ExtUtils::MakeMaker;

my $name = 'Symbol';
my %tables = (
	      symbol_t => [qw(
			      symbol.ucm
			      dingbats.ucm
			      adobeSymbol.ucm
			      adobeZdingbat.ucm
			      macSymbol.ucm
			      macDingbats.ucm
			      )
			      ],
	      );

WriteMakefile(
              INC		=> "-I../Encode",
	      NAME		=> 'Encode::'.$name,
	      VERSION_FROM	=> "$name.pm",
	      OBJECT		=> '$(O_FILES)',
	      'dist'		=> {
		  COMPRESS	=> 'gzip -9f',
		  SUFFIX	=> 'gz',
		  DIST_DEFAULT => 'all tardist',
	      },
	      MAN3PODS	=> {},
	      # OS 390 winges about line numbers > 64K ???
	      XSOPT => '-nolinenumbers',
	      );

package MY;

sub post_initialize
{
    my ($self) = @_;
    my %o;
    my $x = $self->{'OBJ_EXT'};
    # Add the table O_FILES
    foreach my $e (keys %tables)
    {
	$o{$e.$x} = 1;
    }
    $o{"$name$x"} = 1;
    $self->{'O_FILES'} = [sort keys %o];
    my @files = ("$name.xs");
    $self->{'C'} = ["$name.c"];
    $self->{SOURCE} .= " $name.c"
        if $^O eq 'MacOS' && $self->{SOURCE} !~ /\b$name\.c\b/;
    $self->{'H'} = [$self->catfile($self->updir,'Encode', 'encode.h')];
    my %xs;
    foreach my $table (keys %tables) {
	push (@{$self->{'C'}},"$table.c");
	# Do NOT add $table.h etc. to H_FILES unless we own up as to how they
	# get built.
	foreach my $ext (qw($(OBJ_EXT) .c .h .exh .fnm)) {
	    push (@files,$table.$ext);
	}
	$self->{SOURCE} .= " $table.c"
	    if $^O eq 'MacOS' && $self->{SOURCE} !~ /\b$table\.c\b/;
    }
    $self->{'XS'} = { "$name.xs" => "$name.c" };
    $self->{'clean'}{'FILES'} .= join(' ',@files);
    open(XS,">$name.xs") || die "Cannot open $name.xs:$!";
    print XS <<'END';
#include <EXTERN.h>
#include <perl.h>
#include <XSUB.h>
#define U8 U8
#include "encode.h"
END
    foreach my $table (keys %tables) {
	print XS qq[#include "${table}.h"\n];
    }
    print XS <<"END";

static void
Encode_XSEncoding(pTHX_ encode_t *enc)
{
 dSP;
 HV *stash = gv_stashpv("Encode::XS", TRUE);
 SV *sv    = sv_bless(newRV_noinc(newSViv(PTR2IV(enc))),stash);
 int i = 0;
 PUSHMARK(sp);
 XPUSHs(sv);
 while (enc->name[i])
  {
   const char *name = enc->name[i++];
   XPUSHs(sv_2mortal(newSVpvn(name,strlen(name))));
  }
 PUTBACK;
 call_pv("Encode::define_encoding",G_DISCARD);
 SvREFCNT_dec(sv);
}

MODULE = Encode::$name	PACKAGE = Encode::$name
PROTOTYPES: DISABLE
BOOT:
{
END
    foreach my $table (keys %tables) {
	print XS qq[#include "${table}.exh"\n];
    }
    print XS "}\n";
    close(XS);
    return "# Built $name.xs\n\n";
}

sub postamble
{
    my $self = shift;
    my $dir  = $self->catdir($self->updir,'ucm');
    my $str  = "# $name\$(OBJ_EXT) depends on .h and .exh files not .c files - but all written by enc2xs\n";
    $str    .= "$name.c : $name.xs ";
    foreach my $table (keys %tables)
    {
	$str .= " $table.c";
    }
    $str .= "\n\n";
    $str .= "$name\$(OBJ_EXT) : $name.c\n\n";

    my $enc2xs = $self->catfile($self->updir,'bin', 'enc2xs');
    foreach my $table (keys %tables)
    {
	my $numlines = 1;
	my $lengthsofar = length($str);
	my $continuator = '';
	$str .= "$table.c : $enc2xs Makefile.PL";
	foreach my $file (@{$tables{$table}})
	{
	    $str .= $continuator.' '.$self->catfile($dir,$file);
	    if ( length($str)-$lengthsofar > 128*$numlines )
	    {
		$continuator .= " \\\n\t";
		$numlines++;
	    } else {
		$continuator = '';
	    }
	}
	my $plib   = $self->{PERL_CORE} ? '"-I$(PERL_LIB)"' : '';
	$plib .= " -MCross=$::Cross::platform" if defined $::Cross::platform;
	my $ucopts = '-"Q" -"O"';
	$str .=  
	    qq{\n\t\$(PERL) $plib $enc2xs $ucopts -o \$\@ -f $table.fnm\n\n};
	open (FILELIST, ">$table.fnm")
	    || die "Could not open $table.fnm: $!";
	foreach my $file (@{$tables{$table}})
	{
	    print FILELIST $self->catfile($dir,$file) . "\n";
	}
	close(FILELIST);
    }
    return $str;
}

