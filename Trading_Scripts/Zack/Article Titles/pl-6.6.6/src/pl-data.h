/*  $Id$

    Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        wielemak@science.uva.nl
    WWW:           http://www.swi-prolog.org
    Copyright (C): 1985-2007, University of Amsterdam

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Aim
===

Flexibel adaption to different memory model.   Possible  to make `clean'
programs, i.e. programs that donot make assumptions on the memory model.
The latter appears necessary on some systems to put Prolog into a DLL.

Fast comparison and checking. The hope  is   that  the  result will have
comparable or better speed.

Approach
========

	* No direct pointers in Prolog machine words anymore

	* Tags in the low bits to exploit SPARC and possible other
	  machines fixed-width instruction, so masks can be loaded
	  in one instead of two instructions.

	* Explicit encoding of the `user' data-types in the word,
	  so PL_term_type() can be much faster.

	* Explicit encoding of the storage regime used, so more code
	  can be generic.

Types:
======

Sorted to standard order of terms:

Storage places:

	S	Static (global variable)
	L	Local
	G	Global
	T	Trail
	-	Inline

	      INDEX  STORAGE  L  G  T  S  -  I
-------------------------------------------------------------
Var		0      -                 00
Integer		1      G-       01       00
Float		2      G        01
Atom		3      S              00
String		4      G        01
List		5      G        01
Term		6      G        01
Reference	7      LG    10 01
----------------------------------------------------------------

Adding 2 bits for the garbage collector, this adds up to 7-bits tag info,
leaving us with 32-7 is 25 bits data, or:

	* Tagged ints from -16M to +16M
	* 128 MB per memory area, assuming all data is 4-byte aligned.

Giving this, stacks can be freely shifted!

Bit layout
==========

	* Value are the top-bits, so extracting the value is just a
	  shift.

	* GC masks follow, so, as they are normally both 0, shifting
	  suffices for this too.

	* Type is the low 3-bits, so a simple mask yields the type.

	* Storage in bits 4 and 5

Indirect data
=============

	* Using normal tag, but the storage-specifier is 0x3 (11).  Tag
	  is only INTEGER, STRING or FLOAT

	* Using value: size in words of the object * 4

	* String uses the low-order 2 bits for specifying the amount of
	  padding bytes (0-3, 0 means 4).

NOTE: the tag-numbers are  mapped  to   public  constants  (PL_*) in the
type_map array in pl-fli.c.  Make  sure   this  is  consistent  with the
definitions below. Also the tagtypeex[] array defined in pl-setup.c must
be kept consistent.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "os/pl-buffer.h"

#define LMASK_BITS	7		/* total # mask bits */

#define TAG_MASK	0x00000007L	/* mask for tag */
#define TAG_VAR		0x00000000L	/* tag for variable (= 0L) */
#define TAG_ATTVAR	0x00000001L	/* tag for attributed variable */
#define TAG_FLOAT	0x00000002L	/* Floating point number */
#define TAG_INTEGER	0x00000003L	/* Tagged or indirect integer */
#define TAG_ATOM	0x00000004L	/* an atom */
#define TAG_STRING	0x00000005L	/* String */
#define TAG_COMPOUND	0x00000006L	/* Compound term */
#define TAG_REFERENCE	0x00000007L	/* Reference pointer */

					/* Trail tag-bits */
#define TAG_TRAILMASK	0x00000001L	/* mask for tag */
#define TAG_TRAILADDR	0x00000000L	/* Trail-only: address */
#define TAG_TRAILVAL	0x00000001L	/* Trail-only: value */
#define tagTrailPtr(p)	((Word)((uintptr_t)(p)|TAG_TRAILVAL))
#define isTrailVal(p)	((uintptr_t)(p)&TAG_TRAILVAL)
#define trailValP(p)	((Word)((uintptr_t)(p)&~TAG_TRAILMASK))
#define trailVal(p)	(*trailValP(p))

#define STG_MASK	(0x3<<3)
#define STG_STATIC	(0x0<<3)	/* storage masks */
#define STG_GLOBAL	(0x1<<3)	/* global stack */
#define STG_LOCAL	(0x2<<3)	/* local stack */
#define STG_RESERVED	(0x3<<3)

#define STG_INLINE	STG_STATIC
#define STG_TRAIL	STG_STATIC

#define MARK_MASK	(0x1<<5)	/* GC mark */
#define FIRST_MASK	(0x2<<5)	/* GC first mark */

#define set_marked(p)	do { *(p) |= MARK_MASK; } while(0)
#define set_first(p)	do { *(p) |= FIRST_MASK; } while(0)
#define clear_marked(p)	do { *(p) &= ~MARK_MASK; } while(0)
#define clear_first(p)	do { *(p) &= ~FIRST_MASK; } while(0)
#define clear_both(p)	do { *(p) &= ~(FIRST_MASK|MARK_MASK); } while(0)
#define is_marked(p)	(*(p) & MARK_MASK)
#define is_first(p)	(*(p) & FIRST_MASK)
#define is_marked_or_first(p) (*(p) & (MARK_MASK|FIRST_MASK))

#define tag(w)		((w) & TAG_MASK)
#define storage(w)	((w) & STG_MASK)
#define valPtr2(w, s)	((Word)(((w) >> 5) + base_addresses[s]))
#define valPtr(w)	valPtr2(w, storage(w))
#define valInt(w)	((intptr_t)(w) >> LMASK_BITS)
#define valUInt(w)	((uintptr_t)(w) >> LMASK_BITS)

		 /*******************************
		 *	  EXTENDED TAG		*
		 *******************************/

#define EXBIT(w)	(1<<(w))
#define INDIRECT_BM	( EXBIT(STG_GLOBAL|TAG_INTEGER) | \
			  EXBIT(STG_LOCAL|TAG_INTEGER) | \
			  EXBIT(STG_GLOBAL|TAG_FLOAT) | \
			  EXBIT(STG_LOCAL|TAG_FLOAT) | \
			  EXBIT(STG_GLOBAL|TAG_STRING) | \
			  EXBIT(STG_LOCAL|TAG_STRING) \
			)

#define tagex(w)	((w) & (TAG_MASK|STG_MASK))
#define isIndirect(w)	(EXBIT(tagex(w)) & INDIRECT_BM)


		 /*******************************
		 *	 BASIC TYPE TESTS	*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
For atom, we use tagex() to avoid detecting functor_t on stacks. This is
only important for the  atom-garbage  collector   that  must  make  this
distinction while scanning the global stack   as well as for record-keys
and while loading .wic files.  It comes at no price.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/*#define isVar(w)	(tag(w)   == TAG_VAR)*/
#define isVar(w)	((w)      == (word)0)
#define isAtom(w)	(tagex(w) == (TAG_ATOM|STG_STATIC))
#define isTextAtom(w)	(isAtom(w) && true(atomValue(w)->type, PL_BLOB_TEXT))
#define isInteger(w)	(tag(w)   == TAG_INTEGER)
#define isFloat(w)	(tag(w)   == TAG_FLOAT)
#define isString(w)	(tag(w)   == TAG_STRING)
#define isTerm(w)	(tag(w)   == TAG_COMPOUND)
#define isConst(w)	(isAtom(w) || isTaggedInt(w)) /* H_ATOM, B_ATOM, H_SMALLINT, B_SMALLINT */


		 /*******************************
		 *	    REFERENCES		*
		 *******************************/

#define isRef(w)	(tag(w) == TAG_REFERENCE)
#define isRefL(w)	(tagex(w) == (TAG_REFERENCE|STG_LOCAL))
#define unRef(w)	((Word)valPtr(w))
#define unRefL(w)	((Word)valPtr2(w, STG_LOCAL))
#define deRef(p)	{ while(isRef(*(p))) (p) = unRef(*(p)); }
#define deRef2(p, d)	{ (d) = (p); deRef(d); }
#define makeRefL(p)	consPtr(p, TAG_REFERENCE|STG_LOCAL)
#define makeRefG(p)	consPtr(p, TAG_REFERENCE|STG_GLOBAL)
#define makeRef(p)	((void*)(p) >= (void*)lBase ? makeRefL(p) : makeRefG(p))
#ifdef O_ATTVAR
#define needsRef(w)	(tag(w) <= TAG_ATTVAR)
#else
#define needsRef(w)	isVar(w)
#endif


		 /*******************************
		 *	COMPOUNDS AND LISTS	*
		 *******************************/

#define functorTerm(w)	valueTerm(w)->definition
#define arityTerm(w)	arityFunctor(valueTerm(w)->definition)
#define valueTerm(w)	((Functor)valPtr2(w, STG_GLOBAL))
#define hasFunctor(w,f) (isTerm(w) && valueTerm(w)->definition == (f))
#define argTerm(w, n)	(valueTerm(w)->arguments[n])
#define argTermP(w, n)	(&argTerm(w, n))

#define isList(w)	hasFunctor(w, FUNCTOR_dot2)
#define isNil(w)	((w) == ATOM_nil)

		 /*******************************
		 *     ATTRIBUTED VARIABLES	*
		 *******************************/

#define isAttVar(w)	(tag(w) == TAG_ATTVAR)
#define valPAttVar(w)	((Word)valPtr2(w, STG_GLOBAL))

#define canBind(w)	needsRef(w)


		 /*******************************
		 *	      INDIRECTS		*
		 *******************************/

#if SIZEOF_VOIDP == 4			/* extend as needed */
#define PADBITS 2
#else
#if SIZEOF_VOIDP == 8
#define PADBITS 3
#endif
#endif

#define PADMASK (sizeof(word)-1)

#define mkIndHdr(n, t)	(((n)<<(LMASK_BITS+PADBITS)) | (t) | STG_LOCAL)
#define wsizeofInd(iw)	((iw)>>(LMASK_BITS+PADBITS))
#define addressIndirect(w) valPtr(w)
#define valIndirectP(w)	(((Word)valPtr(w))+1)

#define padHdr(iw)	(((iw)>>LMASK_BITS & PADMASK) ? \
			 ((iw)>>LMASK_BITS & PADMASK) : sizeof(intptr_t))
#define mkPadHdr(n)	(((n)&PADMASK) << LMASK_BITS)
#define mkStrHdr(n,p)	(mkIndHdr(n, TAG_STRING)|mkPadHdr(pad))
#define wsizeofIndirect(w) (wsizeofInd(*addressIndirect(w)))

#define isTaggedInt(w)	(tagex(w) == (TAG_INTEGER|STG_INLINE))
			/* == (isInteger(w) && storage(w) == STG_INLINE) */
#define isBignum(w)	(tagex(w) == (TAG_INTEGER|STG_GLOBAL) && \
			 wsizeofIndirect(w) == sizeof(int64_t)/sizeof(word))
#define isMPZNum(w)	(tagex(w) == (TAG_INTEGER|STG_GLOBAL) && \
			 wsizeofIndirect(w) > sizeof(int64_t)/sizeof(word))
#if ALIGNOF_INT64_T == ALIGNOF_VOIDP
#define valBignum(w)	(*(int64_t *)valIndirectP(w))
#else
#define valBignum(w)	valBignum__LD(w PASS_LD)
#endif
#if ALIGNOF_DOUBLE == ALIGNOF_VOIDP
#define valFloat(w)	(*(double *)valIndirectP(w))
#else
#define valFloat(w)	valFloat__LD(w PASS_LD)
#endif

#define isBString(w)	(isString(w) && ((char *)valIndirectP(w))[0] == 'B')
#define isWString(w)	(isString(w) && ((char *)valIndirectP(w))[0] == 'W')

		 /*******************************
		 *	       VALUES		*
		 *******************************/

#define indexAtom(w)	((w)>>LMASK_BITS)
#define atomValue(w)	fetchAtomArray(indexAtom(w))
#define stringAtom(w)	(atomValue(w)->name)
#define valInteger(w)	(storage(w) == STG_INLINE ? valInt(w) : valBignum(w))

		 /*******************************
		 *	      FUNCTORS		*
		 *******************************/

#define F_ARITY_BITS	5		/* upto 32 inlined arity */
#define F_ARITY_MASK	((1<<F_ARITY_BITS)-1)
#define MK_FUNCTOR(n, a) (functor_t)(((((n)<<F_ARITY_BITS)|(a))<<LMASK_BITS) | \
			  TAG_ATOM|STG_GLOBAL)
#define functorHashValue(f, n)	((f)>>(LMASK_BITS) & ((n)-1))
#define indexFunctor(w)	((w)>>(LMASK_BITS+F_ARITY_BITS))
#define valueFunctor(w) fetchFunctorArray(indexFunctor(w))
#define _arityFunc_(w)	((int)(((w) >> LMASK_BITS) & F_ARITY_MASK))
#define arityFunctor(w) (unlikely(_arityFunc_(w) == F_ARITY_MASK) \
				? valueFunctor(w)->arity \
				: _arityFunc_(w) )
#define isAtomFunctor(w) (arityFunctor(w) == 0)
#define nameFunctor(w)	(valueFunctor(w)->name)

		 /*******************************
		 *	  DERIVED TESTS		*
		 *******************************/

#define nonvar(w)	(!isVar(w))
#define isNumber(w)	(isInteger(w) || isFloat(w))
#define isAtomic(w)	(!canBind(w) && !isTerm(w))


		 /*******************************
		 *	   CREATING WORDS	*
		 *******************************/

#define MAXTAGGEDPTR	(((word)1<<((8*sizeof(word))-5)) - 1)

#define consInt(n)	(((word)(n)<<LMASK_BITS) | TAG_INTEGER)
#define consUInt(n)	(((word)(n)<<LMASK_BITS) | TAG_INTEGER)

