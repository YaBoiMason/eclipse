/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 *	Copyright (c) 1988 AT&T
 *	  All Rights Reserved
 *
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */
#pragma ident	"%Z%%M%	%I%	%E% SMI"

/*
 * SPARC machine dependent and ELF file class dependent functions.
 * Contains routines for performing function binding and symbol relocations.
 */
#include	"_synonyms.h"

#include	<stdio.h>
#include	<sys/elf.h>
#include	<sys/elf_SPARC.h>
#include	<sys/mman.h>
#include	<dlfcn.h>
#include	<synch.h>
#include	<string.h>
#include	<debug.h>
#include	<reloc.h>
#include	<conv.h>
#include	"_rtld.h"
#include	"_audit.h"
#include	"_elf.h"
#include	"msg.h"


extern void	iflush_range(caddr_t, size_t);
extern void	plt_full_range(uintptr_t, uintptr_t);


int
elf_mach_flags_check(Rej_desc *rej, Ehdr *ehdr)
{
	/*
	 * Check machine type and flags.
	 */
	if (ehdr->e_machine != EM_SPARC) {
		if (ehdr->e_machine != EM_SPARC32PLUS) {
			rej->rej_type = SGS_REJ_MACH;
			rej->rej_info = (uint_t)ehdr->e_machine;
			return (0);
		}
		if ((ehdr->e_flags & EF_SPARC_32PLUS) == 0) {
			rej->rej_type = SGS_REJ_MISFLAG;
			rej->rej_info = (uint_t)ehdr->e_flags;
			return (0);
		}
		if ((ehdr->e_flags & ~at_flags) & EF_SPARC_32PLUS_MASK) {
			rej->rej_type = SGS_REJ_BADFLAG;
			rej->rej_info = (uint_t)ehdr->e_flags;
			return (0);
		}
	} else if ((ehdr->e_flags & ~EF_SPARCV9_MM) != 0) {
		rej->rej_type = SGS_REJ_BADFLAG;
		rej->rej_info = (uint_t)ehdr->e_flags;
		return (0);
	}
	return (1);
}

void
ldso_plt_init(Rt_map * lmp)
{
	/*
	 * There is no need to analyze ld.so because we don't map in any of
	 * its dependencies.  However we may map these dependencies in later
	 * (as if ld.so had dlopened them), so initialize the plt and the
	 * permission information.
	 */
	if (PLTGOT(lmp))
		elf_plt_init((PLTGOT(lmp)), (caddr_t)lmp);
}

/*
 * elf_plt_write() will test to see how far away our destination
 *	address lies.  If it is close enough that a branch can
 *	be used instead of a jmpl - we will fill the plt in with
 * 	single branch.  The branches are much quicker then
 *	a jmpl instruction - see bug#4356879 for further
 *	details.
 *
 *	NOTE: we pass in both a 'pltaddr' and a 'vpltaddr' since
 *		librtld/dldump update PLT's who's physical
 *		address is not the same as the 'virtual' runtime
 *		address.
 */
Pltbindtype
/* ARGSUSED4 */
elf_plt_write(uintptr_t addr, uintptr_t vaddr, void *rptr, uintptr_t symval,
	Xword pltndx)
{
	Rela		*rel = (Rela *)rptr;
	uintptr_t	vpltaddr, pltaddr;
	long		disp;


	pltaddr = addr + rel->r_offset;
	vpltaddr = vaddr + rel->r_offset;
	disp = symval - vpltaddr - 4;

	/*
	 * Test if the destination address is close enough to use
	 * a ba,a... instruction to reach it.
	 */
	if (S_INRANGE(disp, 23) && !(rtld_flags & RT_FL_NOBAPLT)) {
		uint_t		*pltent, bainstr;
		Pltbindtype	rc;

		pltent = (uint_t *)pltaddr;
		/*
		 * The
		 *
		 *	ba,a,pt %icc, <dest>
		 *
		 * is the most efficient of the PLT's.  If we
		 * are within +-20 bits *and* running on a
		 * v8plus architecture - use that branch.
		 */
		if ((at_flags & EF_SPARC_32PLUS) &&
		    S_INRANGE(disp, 20)) {
			bainstr = M_BA_A_PT;	/* ba,a,pt %icc,<dest> */
			bainstr |= (S_MASK(19) & (disp >> 2));
			rc = PLT_T_21D;
			DBG_CALL(pltcnt21d++);
		} else {
			/*
			 * Otherwise - we fall back to the good old
			 *
			 *	ba,a	<dest>
			 *
			 * Which still beats a jmpl instruction.
			 */
			bainstr = M_BA_A;		/* ba,a <dest> */
			bainstr |= (S_MASK(22) & (disp >> 2));
			rc = PLT_T_24D;
			DBG_CALL(pltcnt24d++);
		}

		pltent[2] = M_NOP;		/* nop instr */
		pltent[1] = bainstr;

		iflush_range((char *)(&pltent[1]), 4);
		pltent[0] = M_NOP;		/* nop instr */
		iflush_range((char *)(&pltent[0]), 4);
		return (rc);
	}

	/*
	 * The PLT destination is not in reach of
	 * a branch instruction - so we fall back
	 * to a 'jmpl' sequence.
	 */
	plt_full_range(pltaddr, symval);
	DBG_CALL(pltcntfull++);
	return (PLT_T_FULL);
}


/*
 * Local storage space created on the stack created for this glue
 * code includes space for:
 *		0x4	pointer to dyn_data
 *		0x4	size prev stack frame
 */
static const uchar_t dyn_plt_template[] = {
/* 0x00 */	0x80, 0x90, 0x00, 0x1e,	/* tst   %fp */
/* 0x04 */	0x02, 0x80, 0x00, 0x04, /* be    0x14 */
/* 0x08 */	0x82, 0x27, 0x80, 0x0e,	/* sub   %sp, %fp, %g1 */
/* 0x0c */	0x10, 0x80, 0x00, 0x03, /* ba	 0x20 */
/* 0x10 */	0x01, 0x00, 0x00, 0x00, /* nop */
/* 0x14 */	0x82, 0x10, 0x20, 0x60, /* mov	0x60, %g1 */
/* 0x18 */	0x9d, 0xe3, 0xbf, 0x98,	/* save	%sp, -0x68, %sp */
/* 0x1c */	0xc2, 0x27, 0xbf, 0xf8,	/* st	%g1, [%fp + -0x8] */
/* 0x20 */	0x03, 0x00, 0x00, 0x00,	/* sethi %hi(val), %g1 */
/* 0x24 */	0x82, 0x10, 0x60, 0x00, /* or	 %g1, %lo(val), %g1 */
/* 0x28 */	0x40, 0x00, 0x00, 0x00,	/* call  <rel_addr> */
/* 0x2c */	0xc2, 0x27, 0xbf, 0xfc	/* st    %g1, [%fp + -0x4] */
};

int	dyn_plt_ent_size = sizeof (dyn_plt_template) +
		sizeof (uintptr_t) +	/* reflmp */
		sizeof (uintptr_t) +	/* deflmp */
		sizeof (ulong_t) +	/* symndx */
		sizeof (ulong_t) +	/* sb_flags */
		sizeof (Sym);		/* symdef */

/*
 * the dynamic plt entry is:
 *
 *	tst	%fp
 *	be	1f
 *	nop
 *	sub	%sp, %fp, %g1
 *	ba	2f
 *	nop
 * 1:
 *	mov	SA(MINFRAME), %g1	! if %fp is null this is the
 *					!   'minimum stack'.  %fp is null
 *					!   on the initial stack frame
 * 2:
 *	save	%sp, -(SA(MINFRAME) + 2 * CLONGSIZE), %sp
 *	st	%g1, [%fp + -0x8] ! store prev_stack size in [%fp - 8]
 *	sethi	%hi(dyn_data), %g1
 *	or	%g1, %lo(dyn_data), %g1
 *	call	elf_plt_trace
 *	st	%g1, [%fp + -0x4] ! store dyn_data ptr in [%fp - 4]
 * dyn data:
 *	uintptr_t	reflmp
 *	uintptr_t	deflmp
 *	ulong_t		symndx
 *	ulong_t		sb_flags
 *	Sym		symdef
 */
static caddr_t
elf_plt_trace_write(caddr_t addr, Rela *rptr, Rt_map *rlmp, Rt_map *dlmp,
    Sym *sym, ulong_t symndx, ulong_t pltndx, caddr_t to, ulong_t sb_flags,
    int *fail)
{
	extern ulong_t	elf_plt_trace();
	uintptr_t	dyn_plt, *dyndata;

	/*
	 * If both pltenter & pltexit have been disabled there
	 * there is no reason to even create the glue code.
	 */
	if ((sb_flags & (LA_SYMB_NOPLTENTER | LA_SYMB_NOPLTEXIT)) ==
	    (LA_SYMB_NOPLTENTER | LA_SYMB_NOPLTEXIT)) {
		(void) elf_plt_write((uintptr_t)addr, (uintptr_t)addr,
		    rptr, (uintptr_t)to, pltndx);
		return (to);
	}

	/*
	 * We only need to add the glue code if there is an auditing
	 * library that is interested in this binding.
	 */
	dyn_plt = (uintptr_t)AUDINFO(rlmp)->ai_dynplts +
		(pltndx * dyn_plt_ent_size);

	/*
	 * Have we initialized this dynamic plt entry yet?  If we haven't do it
	 * now.  Otherwise this function has been called before, but from a
	 * different plt (ie. from another shared object).  In that case
	 * we just set the plt to point to the new dyn_plt.
	 */
	if (*(uint_t *)dyn_plt == 0) {
		Sym	*symp;
		Xword	symvalue;
		Lm_list	*lml = LIST(rlmp);

		(void) memcpy((void *)dyn_plt, dyn_plt_template,
		    sizeof (dyn_plt_template));
		dyndata = (uintptr_t *)(dyn_plt + sizeof (dyn_plt_template));

		/*
		 * relocating:
		 *	sethi	%hi(dyndata), %g1
		 */
		symvalue = (Xword)dyndata;
		if (do_reloc(R_SPARC_HI22, (uchar_t *)(dyn_plt + 0x20),
		    &symvalue, MSG_ORIG(MSG_SYM_LADYNDATA),
		    MSG_ORIG(MSG_SPECFIL_DYNPLT), lml) == 0) {
			*fail = 1;
			return (0);
		}

		/*
		 * relocating:
		 *	or	%g1, %lo(dyndata), %g1
		 */
		symvalue = (Xword)dyndata;
		if (do_reloc(R_SPARC_LO10, (uchar_t *)(dyn_plt + 0x24),
		    &symvalue, MSG_ORIG(MSG_SYM_LADYNDATA),
		    MSG_ORIG(MSG_SPECFIL_DYNPLT), lml) == 0) {
			*fail = 1;
			return (0);
		}

		/*
		 * relocating:
		 *	call	elf_plt_trace
		 */
		symvalue = (Xword)((uintptr_t)&elf_plt_trace -
		    (dyn_plt + 0x28));
		if (do_reloc(R_SPARC_WDISP30, (uchar_t *)(dyn_plt + 0x28),
		    &symvalue, MSG_ORIG(MSG_SYM_ELFPLTTRACE),
		    MSG_ORIG(MSG_SPECFIL_DYNPLT), lml) == 0) {
			*fail = 1;
			return (0);
		}

		*dyndata++ = (uintptr_t)rlmp;
		*dyndata++ = (uintptr_t)dlmp;
		*(ulong_t *)dyndata++ = symndx;
		*(ulong_t *)dyndata++ = sb_flags;
		symp = (Sym *)dyndata;
		*symp = *sym;
		symp->st_name += (Word)STRTAB(dlmp);
		symp->st_value = (Addr)to;

		iflush_range((void *)dyn_plt, sizeof (dyn_plt_template));
	}

	(void) elf_plt_write((uintptr_t)addr, (uintptr_t)addr,
		rptr, (uintptr_t)dyn_plt, 0);
	return ((caddr_t)dyn_plt);
}


/*
 * Function binding routine - invoked on the first call to a function through
 * the procedure linkage table;
 * passes first through an assembly language interface.
 *
 * Takes the address of the PLT entry where the call originated,
 * the offset into the relocation table of the associated
 * relocation entry and the address of the link map (rt_private_map struct)
 * for the entry.
 *
 * Returns the address of the function referenced after re-writing the PLT
 * entry to invoke the function directly.
 *
 * On error, causes process to terminate with a signal.
 */
ulong_t
elf_bndr(Rt_map *lmp, ulong_t pltoff, caddr_t from)
{
	Rt_map		*nlmp, *llmp;
	ulong_t		addr, vaddr, reloff, symval, rsymndx;
	char		*name;
	Rela		*rptr;
	Sym		*sym, *nsym;
	Xword		pltndx;
	uint_t		binfo, sb_flags = 0;
	Slookup		sl;
	Pltbindtype	pbtype;
	int		entry, lmflags;
	uint_t		dbg_class;
	Lm_list		*lml = LIST(lmp);

	/*
	 * For compatibility with libthread (TI_VERSION 1) we track the entry
	 * value.  A zero value indicates we have recursed into ld.so.1 to
	 * further process a locking request.  Under this recursion we disable
	 * tsort and cleanup activities.
	 */
	entry = enter();

	if ((lmflags = lml->lm_flags) & LML_FLG_RTLDLM) {
		dbg_class = dbg_desc->d_class;
		dbg_desc->d_class = 0;
	}

	/*
	 * Must calculate true plt relocation address from reloc.
	 * Take offset, subtract number of reserved PLT entries, and divide
	 * by PLT entry size, which should give the index of the plt
	 * entry (and relocation entry since they have been defined to be
	 * in the same order).  Then we must multiply by the size of
	 * a relocation entry, which will give us the offset of the
	 * plt relocation entry from the start of them given by JMPREL(lm).
	 */
	addr = pltoff - M_PLT_RESERVSZ;
	pltndx = addr / M_PLT_ENTSIZE;

	/*
	 * Perform some basic sanity checks.  If we didn't get a load map
	 * or the plt offset is invalid then its possible someone has walked
	 * over the plt entries or jumped to plt0 out of the blue.
	 */
	if (!lmp || ((addr % M_PLT_ENTSIZE) != 0)) {
		eprintf(lml, ERR_FATAL, MSG_INTL(MSG_REL_PLTREF),
		    conv_reloc_SPARC_type(R_SPARC_JMP_SLOT),
		    EC_NATPTR(lmp), EC_XWORD(pltoff), EC_NATPTR(from));
		rtldexit(lml, 1);
	}
	reloff = pltndx * sizeof (Rela);

	/*
	 * Use relocation entry to get symbol table entry and symbol name.
	 */
	addr = (ulong_t)JMPREL(lmp);
	rptr = (Rela *)(addr + reloff);
	rsymndx = ELF_R_SYM(rptr->r_info);
	sym = (Sym *)((ulong_t)SYMTAB(lmp) + (rsymndx * SYMENT(lmp)));
	name = (char *)(STRTAB(lmp) + sym->st_name);

	/*
	 * Determine the last link-map of this list, this'll be the starting
	 * point for any tsort() processing.
	 */
	llmp = lml->lm_tail;

	/*
	 * Find definition for symbol.
	 */
	sl.sl_name = name;
	sl.sl_cmap = lmp;
	sl.sl_imap = lml->lm_head;
	sl.sl_hash = 0;
	sl.sl_rsymndx = rsymndx;
	sl.sl_flags = LKUP_DEFT;

	if ((nsym = lookup_sym(&sl, &nlmp, &binfo)) == 0) {
		eprintf(lml, ERR_FATAL, MSG_INTL(MSG_REL_NOSYM), NAME(lmp),
		    demangle(name));
		rtldexit(lml, 1);
	}

	symval = nsym->st_value;
	if (!(FLAGS(nlmp) & FLG_RT_FIXED) &&
	    (nsym->st_shndx != SHN_ABS))
		symval += ADDR(nlmp);
	if ((lmp != nlmp) && ((FLAGS1(nlmp) & FL1_RT_NOINIFIN) == 0)) {
		/*
		 * Record that this new link map is now bound to the caller.
		 */
		if (bind_one(lmp, nlmp, BND_REFER) == 0)
			rtldexit(lml, 1);
	}

	if ((lml->lm_tflags | FLAGS1(lmp)) & LML_TFLG_AUD_SYMBIND) {
		ulong_t	symndx = (((uintptr_t)nsym -
			(uintptr_t)SYMTAB(nlmp)) / SYMENT(nlmp));

		symval = audit_symbind(lmp, nlmp, nsym, symndx, symval,
			&sb_flags);
	}

	if (FLAGS(lmp) & FLG_RT_FIXED)
		vaddr = 0;
	else
		vaddr = ADDR(lmp);

	pbtype = PLT_T_NONE;
	if (!(rtld_flags & RT_FL_NOBIND)) {
		if (((lml->lm_tflags | FLAGS1(lmp)) &
		    (LML_TFLG_AUD_PLTENTER | LML_TFLG_AUD_PLTEXIT)) &&
		    AUDINFO(lmp)->ai_dynplts) {
			int	fail = 0;
			ulong_t	symndx = (((uintptr_t)nsym -
				(uintptr_t)SYMTAB(nlmp)) / SYMENT(nlmp));

			symval = (ulong_t)elf_plt_trace_write((caddr_t)vaddr,
			    rptr, lmp, nlmp, nsym, symndx, pltndx,
			    (caddr_t)symval, sb_flags, &fail);
			if (fail)
				rtldexit(lml, 1);
		} else {
			/*
			 * Write standard PLT entry to jump directly
			 * to newly bound function.
			 */
			pbtype = elf_plt_write((uintptr_t)vaddr,
				(uintptr_t)vaddr, rptr, symval, pltndx);
		}
	}

	/*
	 * Print binding information and rebuild PLT entry.
	 */
	DBG_CALL(Dbg_bind_global(lmp, (Addr)from, (Off)(from - ADDR(lmp)),
	    pltndx, pbtype, nlmp, (Addr)symval, nsym->st_value, name, binfo));

	/*
	 * Complete any processing for newly loaded objects.  Note we don't
	 * know exactly where any new objects are loaded (we know the object
	 * that supplied the symbol, but others may have been loaded lazily as
	 * we searched for the symbol), so sorting starts from the last
	 * link-map know on entry to this routine.
	 */
	if (entry)
		load_completion(llmp, lmp);

	/*
	 * Some operations like dldump() or dlopen()'ing a relocatable object
	 * result in objects being loaded on rtld's link-map, make sure these
	 * objects are initialized also.
	 */
	if ((LIST(nlmp)->lm_flags & LML_FLG_RTLDLM) && LIST(nlmp)->lm_init)
		load_completion(nlmp, 0);

	/*
	 * If the object we've bound to is in the process of being initialized
	 * by another thread, determine whether we should block.
	 */
	is_dep_ready(nlmp, lmp, DBG_WAIT_SYMBOL);

	/*
	 * Make sure the object to which we've bound has had it's .init fired.
	 * Cleanup before return to user code.
	 */
	if (entry) {
		is_dep_init(nlmp, lmp);
		leave(lml);
	}

	if (lmflags & LML_FLG_RTLDLM)
		dbg_desc->d_class = dbg_class;

	return (symval);
}


/*
 * Read and process the relocations for one link object, we assume all
 * relocation sections for loadable segments are stored contiguously in
 * the file.
 */
int
elf_reloc(Rt_map *lmp, uint_t plt)
{
	ulong_t		relbgn, relend, relsiz, basebgn, pltbgn, pltend;
	ulong_t		roffset, rsymndx, psymndx = 0, etext = ETEXT(lmp);
	ulong_t		emap, dsymndx, pltndx;
	uchar_t		rtype;
	long		reladd, value, pvalue;
	Sym		*symref, *psymref, *symdef, *psymdef;
	char		*name, *pname;
	Rt_map		*_lmp, *plmp;
	int		textrel = 0, ret = 1, noplt = 0;
	long		relacount = RELACOUNT(lmp);
	Rela		*rel;
	Pltbindtype	pbtype;
	uint_t		binfo, pbinfo;
	Alist		*bound = 0;

	/*
	 * If an object has any DT_REGISTER entries associated with
	 * it, they are processed now.
	 */
	if ((plt == 0) && (FLAGS(lmp) & FLG_RT_REGSYMS)) {
		if (elf_regsyms(lmp) == 0)
			return (0);
	}

	/*
	 * Although only necessary for lazy binding, initialize the first
	 * procedure linkage table entry to go to elf_rtbndr().  dbx(1) seems
	 * to find this useful.
	 */
	if ((plt == 0) && PLTGOT(lmp)) {
		if ((ulong_t)PLTGOT(lmp) < etext) {
			if (elf_set_prot(lmp, PROT_WRITE) == 0)
				return (0);
			textrel = 1;
		}
		elf_plt_init(PLTGOT(lmp), (caddr_t)lmp);
	}

	/*
	 * Initialize the plt start and end addresses.
	 */
	if ((pltbgn = (ulong_t)JMPREL(lmp)) != 0)
		pltend = pltbgn + (ulong_t)(PLTRELSZ(lmp));

	/*
	 * If we've been called upon to promote an RTLD_LAZY object to an
	 * RTLD_NOW then we're only interested in scaning the .plt table.
	 */
	if (plt) {
		relbgn = pltbgn;
		relend = pltend;
	} else {
		/*
		 * The relocation sections appear to the run-time linker as a
		 * single table.  Determine the address of the beginning and end
		 * of this table.  There are two different interpretations of
		 * the ABI at this point:
		 *
		 *   o	The REL table and its associated RELSZ indicate the
		 *	concatenation of *all* relocation sections (this is the
		 *	model our link-editor constructs).
		 *
		 *   o	The REL table and its associated RELSZ indicate the
		 *	concatenation of all *but* the .plt relocations.  These
		 *	relocations are specified individually by the JMPREL and
		 *	PLTRELSZ entries.
		 *
		 * Determine from our knowledege of the relocation range and
		 * .plt range, the range of the total relocation table.  Note
		 * that one other ABI assumption seems to be that the .plt
		 * relocations always follow any other relocations, the
		 * following range checking drops that assumption.
		 */
		relbgn = (ulong_t)(REL(lmp));
		relend = relbgn + (ulong_t)(RELSZ(lmp));
		if (pltbgn) {
			if (!relbgn || (relbgn > pltbgn))
				relbgn = pltbgn;
			if (!relbgn || (relend < pltend))
				relend = pltend;
		}
	}
	if (!relbgn || (relbgn == relend)) {
		DBG_CALL(Dbg_reloc_run(lmp, 0, plt, DBG_REL_NONE));
		return (1);
	}

	relsiz = (ulong_t)(RELENT(lmp));
	basebgn = ADDR(lmp);
	emap = ADDR(lmp) + MSIZE(lmp);

	DBG_CALL(Dbg_reloc_run(lmp, M_REL_SHT_TYPE, plt, DBG_REL_START));

	/*
	 * If we're processing in lazy mode there is no need to scan the
	 * .rela.plt table.
	 */
	if (pltbgn && ((MODE(lmp) & RTLD_NOW) == 0))
		noplt = 1;

	/*
	 * Loop through relocations.
	 */
	while (relbgn < relend) {
		Addr		vaddr;
		uint_t		sb_flags = 0;

		rtype = ELF_R_TYPE(((Rela *)relbgn)->r_info);

		/*
		 * If this is a RELATIVE relocation in a shared object (the
		 * common case), and if we are not debugging, then jump into a
		 * tighter relocation loop (elf_reloc_relative).  Only make the
		 * jump if we've been given a hint on the number of relocations.
		 */
		if ((rtype == R_SPARC_RELATIVE) &&
		    ((FLAGS(lmp) & FLG_RT_FIXED) == 0) && (DBG_ENABLED == 0)) {
			/*
			 * It's possible that the relative relocation block
			 * has relocations against the text segment as well
			 * as the data segment.  Since our optimized relocation
			 * engine does not check which segment the relocation
			 * is against - just mprotect it now if it's been
			 * marked as containing TEXTREL's.
			 */
			if ((textrel == 0) && (FLAGS1(lmp) & FL1_RT_TEXTREL)) {
				if (elf_set_prot(lmp, PROT_WRITE) == 0) {
					ret = 0;
					break;
				}
				textrel = 1;
			}
			if (relacount) {
				relbgn = elf_reloc_relacount(relbgn, relacount,
				    relsiz, basebgn);
				relacount = 0;
			} else {
				relbgn = elf_reloc_relative(relbgn, relend,
				    relsiz, basebgn, etext, emap);
			}
			if (relbgn >= relend)
				break;
			rtype = ELF_R_TYPE(((Rela *)relbgn)->r_info);
		}

		roffset = ((Rela *)relbgn)->r_offset;

		reladd = (long)(((Rela *)relbgn)->r_addend);
		rsymndx = ELF_R_SYM(((Rela *)relbgn)->r_info);

		rel = (Rela *)relbgn;
		relbgn += relsiz;

		/*
		 * Optimizations.
		 */
		if (rtype == R_SPARC_NONE)
			continue;
		if (noplt && ((ulong_t)rel >= pltbgn) &&
		    ((ulong_t)rel < pltend)) {
			relbgn = pltend;
			continue;
		}

		if (rtype != R_SPARC_REGISTER) {
			/*
			 * If this is a shared object, add the base address
			 * to offset.
			 */
			if (!(FLAGS(lmp) & FLG_RT_FIXED))
				roffset += basebgn;

			/*
			 * If this relocation is not against part of the image
			 * mapped into memory we skip it.
			 */
			if ((roffset < ADDR(lmp)) || (roffset > (ADDR(lmp) +
			    MSIZE(lmp)))) {
				elf_reloc_bad(lmp, (void *)rel, rtype, roffset,
				    rsymndx);
				continue;
			}
		}

		/*
		 * If we're promoting plts determine if this one has already
		 * been written. An uninitialized plts' second instruction is a
		 * branch.
		 */
		if (plt) {
			ulong_t	*_roffset = (ulong_t *)roffset;

			_roffset++;
			if ((*_roffset & (~(S_MASK(22)))) != M_BA_A)
				continue;
		}

		binfo = 0;
		pltndx = (ulong_t)-1;
		pbtype = PLT_T_NONE;
		/*
		 * If a symbol index is specified then get the symbol table
		 * entry, locate the symbol definition, and determine its
		 * address.
		 */
		if (rsymndx) {
			/*
			 * Get the local symbol table entry.
			 */
			symref = (Sym *)((ulong_t)SYMTAB(lmp) +
			    (rsymndx * SYMENT(lmp)));

			/*
			 * If this is a local symbol, just use the base address.
			 * (we should have no local relocations in the
			 * executable).
			 */
			if (ELF_ST_BIND(symref->st_info) == STB_LOCAL) {
				value = basebgn;
				name = (char *)0;

				/*
				 * TLS relocation - value for DTPMOD relocation
				 * is the TLS modid.
				 */
				if (rtype == M_R_DTPMOD)
					value = TLSMODID(lmp);
			} else {
				/*
				 * If the symbol index is equal to the previous
				 * symbol index relocation we processed then
				 * reuse the previous values. (Note that there
				 * have been cases where a relocation exists
				 * against a copy relocation symbol, our ld(1)
				 * should optimize this away, but make sure we
				 * don't use the same symbol information should
				 * this case exist).
				 */
				if ((rsymndx == psymndx) &&
				    (rtype != R_SPARC_COPY)) {
					/* LINTED */
					if (psymdef == 0) {
						DBG_CALL(Dbg_bind_weak(lmp,
						    (Addr)roffset, (Addr)
						    (roffset - basebgn), name));
						continue;
					}
					/* LINTED */
					value = pvalue;
					/* LINTED */
					name = pname;
					symdef = psymdef;
					/* LINTED */
					symref = psymref;
					/* LINTED */
					_lmp = plmp;
					/* LINTED */
					binfo = pbinfo;

					if ((LIST(_lmp)->lm_tflags |
					    FLAGS1(_lmp)) &
					    LML_TFLG_AUD_SYMBIND) {
						value = audit_symbind(lmp, _lmp,
						    /* LINTED */
						    symdef, dsymndx, value,
						    &sb_flags);
					}
				} else {
					Slookup		sl;
					uchar_t		bind;

					/*
					 * Lookup the symbol definition.
					 */
					name = (char *)(STRTAB(lmp) +
					    symref->st_name);

					sl.sl_name = name;
					sl.sl_cmap = lmp;
					sl.sl_imap = 0;
					sl.sl_hash = 0;
					sl.sl_rsymndx = rsymndx;

					if (rtype == R_SPARC_COPY)
						sl.sl_flags = LKUP_COPY;
					else
						sl.sl_flags = LKUP_DEFT;

					sl.sl_flags |= LKUP_ALLCNTLIST;

					if (rtype != R_SPARC_JMP_SLOT)
						sl.sl_flags |= LKUP_SPEC;

					bind = ELF_ST_BIND(symref->st_info);
					if (bind == STB_WEAK)
						sl.sl_flags |= LKUP_WEAK;

					symdef = lookup_sym(&sl, &_lmp, &binfo);

					/*
					 * If the symbol is not found and the
					 * reference was not to a weak symbol,
					 * report an error.  Weak references
					 * may be unresolved.
					 */
					if (symdef == 0) {
					    Lm_list	*lml = LIST(lmp);

					    if (bind != STB_WEAK) {
						if (lml->lm_flags &
						    LML_FLG_IGNRELERR) {
						    continue;
						} else if (lml->lm_flags &
						    LML_FLG_TRC_WARN) {
						    (void) printf(MSG_INTL(
							MSG_LDD_SYM_NFOUND),
							demangle(name),
							NAME(lmp));
						    continue;
						} else {
						    eprintf(lml, ERR_FATAL,
							MSG_INTL(MSG_REL_NOSYM),
							NAME(lmp),
							demangle(name));
						    ret = 0;
						    break;
						}
					    } else {
						psymndx = rsymndx;
						psymdef = 0;

						DBG_CALL(Dbg_bind_weak(lmp,
						    (Addr)roffset, (Addr)
						    (roffset - basebgn), name));
						continue;
					    }
					}

					/*
					 * If symbol was found in an object
					 * other than the referencing object
					 * then record the binding.
					 */
					if ((lmp != _lmp) && ((FLAGS1(_lmp) &
					    FL1_RT_NOINIFIN) == 0)) {
						if (alist_test(&bound, _lmp,
						    sizeof (Rt_map *),
						    AL_CNT_RELBIND) == 0) {
							ret = 0;
							break;
						}
					}

					/*
					 * Calculate the location of definition;
					 * symbol value plus base address of
					 * containing shared object.
					 */
					value = symdef->st_value;
					if (!(FLAGS(_lmp) & FLG_RT_FIXED) &&
					    (symdef->st_shndx != SHN_ABS) &&
					    (ELF_ST_TYPE(symdef->st_info) !=
					    STT_TLS))
						value += ADDR(_lmp);

					/*
					 * Retain this symbol index and the
					 * value in case it can be used for the
					 * subsequent relocations.
					 */
					if (rtype != R_SPARC_COPY) {
						psymndx = rsymndx;
						pvalue = value;
						pname = name;
						psymdef = symdef;
						psymref = symref;
						plmp = _lmp;
						pbinfo = binfo;
					}
					if ((LIST(_lmp)->lm_tflags |
					    FLAGS1(_lmp)) &
					    LML_TFLG_AUD_SYMBIND) {
						dsymndx = (((uintptr_t)symdef -
						    (uintptr_t)SYMTAB(_lmp)) /
						    SYMENT(_lmp));
						value = audit_symbind(lmp, _lmp,
						    symdef, dsymndx, value,
						    &sb_flags);
					}
				}

				/*
				 * If relocation is PC-relative, subtract
				 * offset address.
				 */
				if (IS_PC_RELATIVE(rtype))
					value -= roffset;

				/*
				 * TLS relocation - value for DTPMOD relocation
				 * is the TLS modid.
				 */
				if (rtype == M_R_DTPMOD)
					value = TLSMODID(_lmp);
				else if (rtype == M_R_TPOFF)
					value = -(TLSSTATOFF(_lmp) - value);
			}
		} else {
			/*
			 * Special cases, a register symbol associated with
			 * symbol index 0 is initialized (i.e. relocated) to
			 * a constant in the r_addend field rather than to a
			 * symbol value.
			 *
			 * A DTPMOD relocation is a local binding to a TLS
			 * symbol.  Fill in the TLSMODID for the current object.
			 */
			if (rtype == R_SPARC_REGISTER)
				value = 0;
			else if (rtype == M_R_DTPMOD)
				value = TLSMODID(lmp);
			else
				value = basebgn;
			name = (char *)0;
		}

		/*
		 * If this object has relocations in the text segment, turn
		 * off the write protect.
		 */
		if ((rtype != R_SPARC_REGISTER) && (roffset < etext) &&
		    (textrel == 0)) {
			if (elf_set_prot(lmp, PROT_WRITE) == 0) {
				ret = 0;
				break;
			}
			textrel = 1;
		}

		/*
		 * Call relocation routine to perform required relocation.
		 */
		DBG_CALL(Dbg_reloc_in(LIST(lmp), ELF_DBG_RTLD, M_MACH,
		    M_REL_SHT_TYPE, rel, NULL, name));

		switch (rtype) {
		case R_SPARC_REGISTER:
			/*
			 * The v9 ABI 4.2.4 says that system objects may,
			 * but are not required to, use register symbols
			 * to inidcate how they use global registers. Thus
			 * at least %g6, %g7 must be allowed in addition
			 * to %g2 and %g3.
			 */
			value += reladd;
			if (roffset == STO_SPARC_REGISTER_G1) {
				set_sparc_g1(value);
			} else if (roffset == STO_SPARC_REGISTER_G2) {
				set_sparc_g2(value);
			} else if (roffset == STO_SPARC_REGISTER_G3) {
				set_sparc_g3(value);
			} else if (roffset == STO_SPARC_REGISTER_G4) {
				set_sparc_g4(value);
			} else if (roffset == STO_SPARC_REGISTER_G5) {
				set_sparc_g5(value);
			} else if (roffset == STO_SPARC_REGISTER_G6) {
				set_sparc_g6(value);
			} else if (roffset == STO_SPARC_REGISTER_G7) {
				set_sparc_g7(value);
			} else {
				eprintf(LIST(lmp), ERR_FATAL,
				    MSG_INTL(MSG_REL_BADREG), NAME(lmp),
				    EC_ADDR(roffset));
				ret = 0;
				break;
			}

			DBG_CALL(Dbg_reloc_apply_reg(LIST(lmp), ELF_DBG_RTLD,
			    M_MACH, (Xword)roffset, (Xword)value));
			break;
		case R_SPARC_COPY:
			if (elf_copy_reloc(name, symref, lmp, (void *)roffset,
			    symdef, _lmp, (const void *)value) == 0)
				ret = 0;
			break;
		case R_SPARC_JMP_SLOT:
			pltndx = ((ulong_t)rel -
				(uintptr_t)JMPREL(lmp)) / relsiz;

			if (FLAGS(lmp) & FLG_RT_FIXED)
				vaddr = 0;
			else
				vaddr = ADDR(lmp);

			if (((LIST(lmp)->lm_tflags | FLAGS1(lmp)) &
			    (LML_TFLG_AUD_PLTENTER | LML_TFLG_AUD_PLTEXIT)) &&
			    AUDINFO(lmp)->ai_dynplts) {
				int	fail = 0;
				ulong_t	symndx = (((uintptr_t)symdef -
					(uintptr_t)SYMTAB(_lmp)) /
					SYMENT(_lmp));

				(void) elf_plt_trace_write((caddr_t)vaddr,
				    (Rela *)rel, lmp, _lmp, symdef, symndx,
				    pltndx, (caddr_t)value, sb_flags, &fail);
				if (fail)
					ret = 0;
			} else {
				/*
				 * Write standard PLT entry to jump directly
				 * to newly bound function.
				 */
				DBG_CALL(Dbg_reloc_apply_val(LIST(lmp),
				    ELF_DBG_RTLD, (Xword)roffset,
				    (Xword)value));
				pbtype = elf_plt_write((uintptr_t)vaddr,
				    (uintptr_t)vaddr, (void *)rel, value,
				    pltndx);
			}
			break;
		default:
			value += reladd;

			/*
			 * Write the relocation out.  If this relocation is a
			 * common basic write, skip the doreloc() engine.
			 */
			if ((rtype == R_SPARC_GLOB_DAT) ||
			    (rtype == R_SPARC_32)) {
				if (roffset & 0x3) {
					eprintf(LIST(lmp), ERR_FATAL,
					    MSG_INTL(MSG_REL_NONALIGN),
					    conv_reloc_SPARC_type(rtype),
					    NAME(lmp), demangle(name),
					    EC_OFF(roffset));
					ret = 0;
				} else
					*(uint_t *)roffset += value;
			} else {
				if (do_reloc(rtype, (uchar_t *)roffset,
				    (Xword *)&value, name,
				    NAME(lmp), LIST(lmp)) == 0)
					ret = 0;
			}

			/*
			 * The value now contains the 'bit-shifted' value that
			 * was or'ed into memory (this was set by do_reloc()).
			 */
			DBG_CALL(Dbg_reloc_apply_val(LIST(lmp), ELF_DBG_RTLD,
			    (Xword)roffset, (Xword)value));

			/*
			 * If this relocation is against a text segment, make
			 * sure that the instruction cache is flushed.
			 */
			if (textrel)
				iflush_range((caddr_t)roffset, 0x4);
		}

		if ((ret == 0) &&
		    ((LIST(lmp)->lm_flags & LML_FLG_TRC_WARN) == 0))
			break;

		if (binfo) {
			DBG_CALL(Dbg_bind_global(lmp, (Addr)roffset,
			    (Off)(roffset - basebgn), pltndx, pbtype,
			    _lmp, (Addr)value, symdef->st_value, name, binfo));
		}
	}

	return (relocate_finish(lmp, bound, textrel, ret));
}

/*
 * Provide a machine specific interface to the conversion routine.  By calling
 * the machine specific version, rather than the generic version, we insure that
 * the data tables/strings for all known machine versions aren't dragged into
 * ld.so.1.
 */
const char *
_conv_reloc_type(uint_t rel)
{
	return (conv_reloc_SPARC_type(rel));
}
