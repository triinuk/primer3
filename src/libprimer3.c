/*
Copyright (c) 1996,1997,1998,1999,2000,2001,2004,2006,2007
Whitehead Institute for Biomedical Research, Steve Rozen
(http://jura.wi.mit.edu/rozen), and Helen Skaletsky
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

   * Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
   * Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the following disclaimer
in the documentation and/or other materials provided with the
distribution.
   * Neither the names of the copyright holders nor contributors may
be used to endorse or promote products derived from this software
without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>
#include <float.h>
#include <string.h>
  /* #include "primer3_release.h" */
  /* #include "format_output.h" */
#include "dpal.h"
#include "oligotm.h"
#include "libprimer3.h"
  /* #include "boulder_input.h" */
  /* #include "print_boulder.h" */

/* #define's */

/* 
 * Panic messages for when the program runs out of memory.  pr_program_name and
 * pr_program_name_len must be set at the beginning of main.
 */
#define OOM_MESSAGE      ": out of memory\n"
#define OOM_MESSAGE_LEN  16
#define OOM_STMT1 write(2, pr_program_name, pr_program_name_len)
#define OOM_STMT2 write(2, OOM_MESSAGE, OOM_MESSAGE_LEN), exit(-2)
#define OOM_ERROR OOM_STMT1, OOM_STMT2

#ifndef MAX_PRIMER_LENGTH
#error "Define MAX_PRIMER_LENGTH in Makefile..."
  /* to ensure that MAX_PRIMER_LENGTH <= DPAL_MAX_ALIGN. */
#endif
#if (MAX_PRIMER_LENGTH > DPAL_MAX_ALIGN) 
#error "MAX_PRIMER_LENGTH must be <= DPAL_MAX_ALIGN"
#endif

#define MAX_NN_TM_LENGTH 36 /* The maxium length for which to use the
			       nearest neighbor model when calculating
			       oligo Tms. */

#define MACRO_CAT_2(A,B) A##B
#define MACRO_VALUE_AS_STRING(A) MACRO_STRING(A)

#define PR_POSITION_PENALTY_IS_NULL(PA) \
(PR_DEFAULT_INSIDE_PENALTY == (PA)->inside_penalty \
 && PR_DEFAULT_OUTSIDE_PENALTY == (PA)->outside_penalty)

#define INITIAL_LIST_LEN     2000 /* Initial size of oligo lists. */
#define INITIAL_NUM_RETURN   5    /* Initial space to allocate for pairs to
				     return. */

#define PAIR_OK 1
#define PAIR_FAILED 0

#define OK_OR_MUST_USE(H) ((H)->ok == OV_OK || (H)->must_use)


#define PR_UNDEFINED_INT_OPT          INT_MIN
#define PR_UNDEFINED_DBL_OPT          DBL_MIN

/* Undefined value for alignment score (meaning do not check) used for maximum
   template mispriming or mishyb. */
#define PR_UNDEFINED_ALIGN_OPT        -100

#define TRIMMED_SEQ_LEN(X) ((X)->incl_l)

typedef struct dpal_arg_holder {
  dpal_args *local, *end, *local_end,
    *local_ambig, *local_end_ambig;
} dpal_arg_holder;

/* Function declarations. */
static int    _pr_data_control(primer_args *,  seq_args *);
static int    _pr_need_pair_template_mispriming(const primer_args *pa);
static int    _pr_need_template_mispriming(const primer_args *);

static void   _pr_substr(const char *, int, int, char *);

static void   add_must_use_warnings(seq_args *, const char *,
				    const oligo_stats *);
static void   add_pair(const primer_pair *, pair_array_t *);
static short  align(const char *, const char*, const dpal_args *a);
static int    check_intervals(const char *, const int,
			      interval_array_t, const int, seq_args *);

static int    choose_pair(primer3_state *,
			  const primer_args *,
			  seq_args *, const dpal_arg_holder *,
			  int,
			  pair_array_t *);

static void   check_sequence_quality(const primer_args *, primer_rec *,
				     oligo_type, const seq_args *, int, int,
				     int *, int *);

static int    choose_internal_oligo(primer3_state *,
				    const primer_rec *, const primer_rec *,
				    int *, seq_args *,
				    const primer_args *,
				    const dpal_arg_holder *);

void          compute_position_penalty(const primer_args *, const seq_args *, 
				       primer_rec *, oligo_type);

static void   create_and_print_file(const seq_args *, int, const primer_rec[],
				    const oligo_type, const int, const int,
				    const char *);
static char   dna_to_upper(char *, int);
static int    find_stop_codon(const char *, int, int);
static void   gc_and_n_content(const int, const int, const char *, primer_rec *);

static int    make_primer_lists(primer3_state *,
				primer_args *,
				seq_args *,
				const dpal_arg_holder *);

static int    make_internal_oligos_list(primer3_state *,
					const primer_args *,
					seq_args *,
					const dpal_arg_holder *);

static double obj_fn(const primer_args *, primer_pair *);

static int    oligo_overlaps_interval(const int, const int,
				      interval_array_t, const int);
static int    oligo_pair_seen(const primer_pair *, const pair_array_t *);

static void   oligo_param(const primer_args *pa,
			  primer_rec *, oligo_type,
			  const dpal_arg_holder*,
			  seq_args *, oligo_stats *);

static int    pair_param(primer3_state *p,
			 const primer_args *,
			 seq_args *,
			 int, int, int,
			 primer_pair *,
			 const dpal_arg_holder*);

static int    pair_spans_target(const primer_pair *, const seq_args *);
static void   pr_append_w_sep(pr_append_str *, const char *, const char *);
static int    pr_is_empty(const pr_append_str *x);

static int    primer_pair_comp(const void *, const void*);
static int    primer_rec_comp(const void *, const void *);
static void   print_list(const primer3_state*,  const seq_args *, const primer_args *);
static void   print_list_header(FILE *, oligo_type, int, int);
static void   print_oligo(FILE *, const seq_args *, int, const primer_rec *,
			  oligo_type, int, int);
static char   *strstr_nocase(char *, char *);

static double p_obj_fn(const primer_args *, primer_rec *, int );

static void   oligo_compl(primer_rec *, const primer_args *, seq_args *,
			  oligo_type, const dpal_arg_holder *);

static void   oligo_mispriming(primer_rec *,
			       const primer_args *,
			       seq_args *,
			       oligo_type, 
			       const dpal_args *,
			       const dpal_arg_holder *);

static int    pair_repeat_sim(primer_pair *, const primer_args *);

static void   free_repeat_sim_score(primer3_state *);

/* edited by T. Koressaar for lowercase masking:  */
static void   check_if_lowercase_masked(const int position,
					const char *sequence,
					primer_rec *h);

/* Global static variables. */
static const char * copyright[] = {
"",
"Copyright (c) 1996,1997,1998,1999,2000,2001,2004,2006",
"Whitehead Institute for Biomedical Research, Steve Rozen",
"(http://jura.wi.mit.edu/rozen), and Helen Skaletsky",
"All rights reserved.",
"",
"Redistribution and use in source and binary forms, with or without",
"modification, are permitted provided that the following conditions are",
"met:",
"",
"   * Redistributions of source code must retain the above copyright",
"notice, this list of conditions and the following disclaimer.",
"   * Redistributions in binary form must reproduce the above",
"copyright notice, this list of conditions and the following disclaimer",
"in the documentation and/or other materials provided with the",
"distribution.",
"   * Neither the names of the copyright holders nor contributors may",
"be used to endorse or promote products derived from this software",
"without specific prior written permission.",
"",
"THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS",
"\"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT",
"LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR",
"A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT",
"OWNERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,",
"SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT",
"LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,",
"DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY",
"THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT",
"(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE",
"OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.",
NULL
};

/* Other global variables. */
const char *pr_program_name;
int pr_program_name_len;

typedef struct oligo_array {
  int len;
  primer_rec *data;
} oligo_array;

#define FREE_STUFF { \
    free(sa);  \
    if (0 != best_pairs.storage_size) free(best_pairs.pairs); \
    free_seq_lib(&pa->repeat_lib);                            \
    free_seq_lib(&pa->io_mishyb_library); free(pa);           \
}

/*
 * ==========================================================================
 * External APIs
 * ==========================================================================
 */

/* Default parameter values.  */
#define OPT_SIZE            20
#define MIN_SIZE             18
#define MAX_SIZE             27

#define OPT_TM             60.0
#define MIN_TM             57.0
#define MAX_TM             63.0
#define MAX_DIFF_TM       100.0

/* 
Added by T.Koressaar for updated table thermodynamics.  Specifies
details of melting temperature calculation.  (New in v. 1.1.0, added
by Maido Remm and Triinu Koressaar.)

A value of 1 (recommended) directs primer3 to use the table of
thermodynamic values and the method for melting temperature
calculation suggested in the paper [SantaLucia JR (1998) "A unified
view of polymer, dumbbell and oligonucleotide DNA nearest-neighbor
thermodynamics", Proc Natl Acad Sci 95:1460-65
http://dx.doi.org/10.1073/pnas.95.4.1460].

A value of 0 directs primer3 to a backward compatible calculation
(in other words, the only calculation availble in previous
version of primer3).

This backward compatible calculation uses the table of
thermodynamic parameters in the paper [Breslauer KJ, Frank R,
Bl�cker H and Marky LA (1986) "Predicting DNA duplex stability
from the base sequence" Proc Natl Acad Sci 83:4746-50
http://dx.doi.org/10.1073/pnas.83.11.3746],
and the method in the paper [Rychlik W, Spencer WJ and Rhoads
RE (1990) "Optimization of the annealing temperature for DNA
amplification in vitro", Nucleic Acids Res 18:6409-12
http://www.pubmedcentral.nih.gov/articlerender.fcgi?tool=pubmed&pubmedid=2243783].

The default value is 0 only for backward compatibility.
*/
#define TM_SANTALUCIA       0

/* 
Added by T.Koressaar for salt correction for Tm calculation.
A value of 1 (recommended) directs primer3 to use the salt
correction formula in the paper [SantaLucia JR (1998) "A unified view
of polymer, dumbbell and oligonucleotide DNA nearest-neighbor
thermodynamics", Proc Natl Acad Sci 95:1460-65
http://dx.doi.org/10.1073/pnas.95.4.1460]

A value of 0 directs primer3 to use the the salt correction
formula in the paper [Schildkraut, C, and Lifson, S (1965)
"Dependence of the melting temperature of DNA on salt
concentration", Biopolymers 3:195-208 (not available on-line)].
This was the formula used in previous version of primer3.

A value of 2 directs primer3 to use the salt correction formula
in the paper [Owczarzy R, You Y, Moreira BG, Manthey JA, Huang L,
Behlke MA and Walder JA (2004) "Effects of sodium ions on DNA
duplex oligomers: Improved predictions of melting temperatures",
Biochemistry 43:3537-54 http://dx.doi.org/10.1021/bi034621r].

The default is 0 only for backward compatibility.
*/
#define SALT_CORRECTIONS    0

#define DEFAULT_OPT_GC_PERCENT PR_UNDEFINED_INT_OPT
#define MIN_GC             20.0
#define MAX_GC             80.0
#define SALT_CONC          50.0

/*
 DIVALENT_CONC and DNTP_CONC are both needed for enabling to use divalent cations for 
 calculation of melting temperature of short and long oligos. 
 The formula for converting the divalent cations to monovalent cations is in the paper 
 [Ahsen von N, Wittwer CT, Schutz E (2001) "Oligonucleotide Melting Temperatures under PCR Conditions:
 Nearest-Neighbor Corrections for Mg^2+, Deoxynucleotide Triphosphate, and Dimethyl Sulfoxide Concentrations
 with Comparision to Alternative Empirical Formulas", Clinical Chemistry 47:1956-61 
 http://www.clinchem.org/cgi/content/full/47/11/1956]
 The default is 0. (New in v. 1.1.0, added by Maido Remm and Triinu Koressaar.)
 */
#define DIVALENT_CONC       0.0
#define DNTP_CONC           0.0
#define DNA_CONC           50.0
#define NUM_NS_ACCEPTED       0
#define MAX_POLY_X            5
#define SELF_ANY            800
#define SELF_END            300
#define PAIR_COMPL_ANY      800
#define PAIR_COMPL_END      300
#define FILE_FLAG             0
#define EXPLAIN_FLAG          0
#define GC_CLAMP              0
#define LIBERAL_BASE          0
#define PICK_INTERNAL_OLIGO   0
#define PRIMER_TASK           0
#define INTERNAL_OLIGO_OPT_SIZE   20
#define INTERNAL_OLIGO_MIN_SIZE   18
#define INTERNAL_OLIGO_MAX_SIZE   27
#define INTERNAL_OLIGO_OPT_TM     60.0
#define INTERNAL_OLIGO_MIN_TM     57.0
#define INTERNAL_OLIGO_MAX_TM     63.0
#define INTERNAL_OLIGO_MIN_GC     20.0
#define INTERNAL_OLIGO_MAX_GC     80.0
#define INTERNAL_OLIGO_SALT_CONC         50.0
#define INTERNAL_OLIGO_DIVALENT_CONC      0.0
#define INTERNAL_OLIGO_DNTP_CONC          0.0
#define INTERNAL_OLIGO_DNA_CONC          50.0
#define INTERNAL_OLIGO_NUM_NS               0
#define INTERNAL_OLIGO_MAX_POLY_X           5 
#define INTERNAL_OLIGO_SELF_ANY          1200
#define INTERNAL_OLIGO_SELF_END          1200
#define INTERNAL_OLIGO_REPEAT_SIMILARITY 1200
#define REPEAT_SIMILARITY                1200
#define PAIR_REPEAT_SIMILARITY           2400
#define FIRST_BASE_INDEX                    0
#define NUM_RETURN                          5
#define MIN_QUALITY                         0
#define QUALITY_RANGE_MIN                   0
#define QUALITY_RANGE_MAX                 100
#define DEFAULT_MAX_END_STABILITY         100.0

/* 
Added by T.Koressaar. Enables design of primers from lowercase masked
template.  A value of 1 directs primer3 to reject primers overlapping
lowercase a base exactly at the 3' end.

This property relies on the assumption that masked features
(e.g. repeats) can partly overlap primer, but they cannot overlap the
3'-end of the primer.  In other words, lowercase bases at other
positions in the primer are accepted, assuming that the masked
features do not influence the primer performance if they do not
overlap the 3'-end of primer.
*/
#define LOWERCASE_MASKING                   0

#define PRIMER_PRODUCT_OPT_SIZE      PR_UNDEFINED_INT_OPT
#define PRIMER_PRODUCT_OPT_TM        PR_UNDEFINED_DBL_OPT
#define MAX_TEMPLATE_MISPRIMING      PR_UNDEFINED_ALIGN_OPT
#define PAIR_MAX_TEMPLATE_MISPRIMING PR_UNDEFINED_ALIGN_OPT
#define IO_MAX_TEMPLATE_MISHYB       PR_UNDEFINED_ALIGN_OPT

#define LIB_AMBIGUITY_CODES_CONSENSUS 1
/*  For backward compatibility. It turns out that
    this _not_ what one normally wants, since many
    libraries contain strings of N, which then match
    every oligo (very bad).
*/

/* Weights for objective functions for oligos and pairs. */
#define PRIMER_WT_TM_GT          1
#define PRIMER_WT_TM_LT          1
#define PRIMER_WT_SIZE_LT        1
#define PRIMER_WT_SIZE_GT        1
#define PRIMER_WT_GC_PERCENT_LT  0
#define PRIMER_WT_GC_PERCENT_GT  0
#define PRIMER_WT_COMPL_ANY      0
#define PRIMER_WT_COMPL_END      0
#define PRIMER_WT_NUM_NS         0
#define PRIMER_WT_REP_SIM        0
#define PRIMER_WT_SEQ_QUAL       0
#define PRIMER_WT_END_QUAL       0
#define PRIMER_WT_POS_PENALTY    1
#define PRIMER_WT_END_STABILITY  0

#define IO_WT_TM_GT          1
#define IO_WT_TM_LT          1
#define IO_WT_SIZE_LT        1
#define IO_WT_SIZE_GT        1
#define IO_WT_GC_PERCENT_LT  0
#define IO_WT_GC_PERCENT_GT  0
#define IO_WT_COMPL_ANY      0
#define IO_WT_COMPL_END      0
#define IO_WT_NUM_NS         0
#define IO_WT_REP_SIM        0
#define IO_WT_SEQ_QUAL       0
#define IO_WT_END_QUAL       0

#define PAIR_WT_PRIMER_PENALTY      1
#define PAIR_WT_IO_PENALTY          0
#define PAIR_WT_DIFF_TM             0
#define PAIR_WT_COMPL_ANY           0
#define PAIR_WT_COMPL_END           0
#define PAIR_WT_REP_SIM             0
#define PAIR_WT_PRODUCT_TM_LT       0
#define PAIR_WT_PRODUCT_TM_GT       0
#define PAIR_WT_PRODUCT_SIZE_LT     0
#define PAIR_WT_PRODUCT_SIZE_GT     0

void
pr_set_default_global_args(a)
    primer_args *a;
{
    memset(a, 0, sizeof(*a));  
    a->primer_opt_size  = OPT_SIZE;
    a->primer_min_size  = MIN_SIZE;
    a->primer_max_size  = MAX_SIZE;
    a->opt_tm           = OPT_TM;
    a->min_tm           = MIN_TM;
    a->max_tm           = MAX_TM;
    a->max_diff_tm      = MAX_DIFF_TM;
    a->tm_santalucia    = TM_SANTALUCIA; /* added by T.Koressaar */
    a->salt_corrections = SALT_CORRECTIONS; /* added by T.Koressaar */
    a->min_gc           = MIN_GC;
    a->opt_gc_content   = DEFAULT_OPT_GC_PERCENT;
    a->max_gc           = MAX_GC;
    a->salt_conc        = SALT_CONC;
    a->divalent_conc    = DIVALENT_CONC;
    a->dntp_conc        = DNTP_CONC;
    a->dna_conc         = DNA_CONC;
    a->num_ns_accepted  = NUM_NS_ACCEPTED;
    a->self_any         = SELF_ANY;
    a->self_end         = SELF_END;
    a->pair_compl_any   = PAIR_COMPL_ANY;
    a->pair_compl_end   = PAIR_COMPL_END;
    a->file_flag        = FILE_FLAG;
    a->explain_flag     = EXPLAIN_FLAG;
    a->gc_clamp         = GC_CLAMP;
    a->max_poly_x       = MAX_POLY_X;
    a->liberal_base      = LIBERAL_BASE;
    a->primer_task       = PRIMER_TASK;
    a->first_base_index  = FIRST_BASE_INDEX;
    a->num_return        = NUM_RETURN;
    a->pr_min[0]         = 100;
    a->pr_max[0]         = 300;
    a->num_intervals     = 1;
    a->repeat_compl      = REPEAT_SIMILARITY;
    a->pair_repeat_compl = PAIR_REPEAT_SIMILARITY;
    a->min_quality       = MIN_QUALITY;
    a->min_end_quality   = MIN_QUALITY;
    a->quality_range_min = QUALITY_RANGE_MIN;
    a->quality_range_max = QUALITY_RANGE_MAX;
    a->outside_penalty   = PR_DEFAULT_OUTSIDE_PENALTY;
    a->inside_penalty    = PR_DEFAULT_INSIDE_PENALTY;
    a->max_end_stability = DEFAULT_MAX_END_STABILITY;
    a->lowercase_masking = LOWERCASE_MASKING; /* added by T.Koressaar */
    a->product_max_tm    = PR_DEFAULT_PRODUCT_MAX_TM;
    a->product_min_tm    = PR_DEFAULT_PRODUCT_MIN_TM;
    a->product_opt_tm    = PRIMER_PRODUCT_OPT_TM;
    a->product_opt_size  = PRIMER_PRODUCT_OPT_SIZE;
    a->max_template_mispriming
                          = MAX_TEMPLATE_MISPRIMING;
    a->pair_max_template_mispriming
                          = PAIR_MAX_TEMPLATE_MISPRIMING;

    a->io_primer_opt_size = INTERNAL_OLIGO_OPT_SIZE;
    a->io_primer_min_size = INTERNAL_OLIGO_MIN_SIZE;
    a->io_primer_max_size = INTERNAL_OLIGO_MAX_SIZE;
    a->io_opt_tm          = INTERNAL_OLIGO_OPT_TM;
    a->io_min_tm          = INTERNAL_OLIGO_MIN_TM;
    a->io_max_tm          = INTERNAL_OLIGO_MAX_TM;
    a->io_min_gc          = INTERNAL_OLIGO_MIN_GC;
    a->io_opt_gc_content  = DEFAULT_OPT_GC_PERCENT;
    a->io_max_gc          = INTERNAL_OLIGO_MAX_GC;
    a->io_max_poly_x      = INTERNAL_OLIGO_MAX_POLY_X;
    a->io_salt_conc       = INTERNAL_OLIGO_SALT_CONC;
    a->io_divalent_conc   = INTERNAL_OLIGO_DIVALENT_CONC;
    a->io_dntp_conc       = INTERNAL_OLIGO_DNTP_CONC;
    a->io_dna_conc        = INTERNAL_OLIGO_DNA_CONC;
    a->io_num_ns_accepted = INTERNAL_OLIGO_NUM_NS;
    a->io_self_any        = INTERNAL_OLIGO_SELF_ANY;
    a->io_self_end        = INTERNAL_OLIGO_SELF_END;
    a->io_repeat_compl    = INTERNAL_OLIGO_REPEAT_SIMILARITY;
    a->io_min_quality     = MIN_QUALITY;
    a->io_min_end_quality = MIN_QUALITY;
    a->io_max_template_mishyb
                          = IO_MAX_TEMPLATE_MISHYB;

    a->primer_weights.temp_gt       = PRIMER_WT_TM_GT;
    a->primer_weights.temp_lt       = PRIMER_WT_TM_LT;
    a->primer_weights.length_gt     = PRIMER_WT_SIZE_GT;
    a->primer_weights.length_lt     = PRIMER_WT_SIZE_LT;
    a->primer_weights.gc_content_gt = PRIMER_WT_GC_PERCENT_GT;
    a->primer_weights.gc_content_lt = PRIMER_WT_GC_PERCENT_LT;
    a->primer_weights.compl_any     = PRIMER_WT_COMPL_ANY;
    a->primer_weights.compl_end     = PRIMER_WT_COMPL_END;
    a->primer_weights.num_ns        = PRIMER_WT_NUM_NS;
    a->primer_weights.repeat_sim    = PRIMER_WT_REP_SIM;
    a->primer_weights.seq_quality   = PRIMER_WT_SEQ_QUAL;
    a->primer_weights.end_quality   = PRIMER_WT_END_QUAL;
    a->primer_weights.pos_penalty   = PRIMER_WT_POS_PENALTY;
    a->primer_weights.end_stability = PRIMER_WT_END_STABILITY;

    a->io_weights.temp_gt     = IO_WT_TM_GT;
    a->io_weights.temp_lt     = IO_WT_TM_LT;
    a->io_weights.length_gt   = IO_WT_SIZE_GT;
    a->io_weights.length_lt   = IO_WT_SIZE_LT;
    a->io_weights.gc_content_gt = IO_WT_GC_PERCENT_GT;
    a->io_weights.gc_content_lt = IO_WT_GC_PERCENT_LT;
    a->io_weights.compl_any   = IO_WT_COMPL_ANY;
    a->io_weights.compl_end   = IO_WT_COMPL_END;
    a->io_weights.num_ns      = IO_WT_NUM_NS;
    a->io_weights.repeat_sim  = IO_WT_REP_SIM;
    a->io_weights.seq_quality = IO_WT_SEQ_QUAL;
    a->io_weights.end_quality = IO_WT_END_QUAL;

    a->pr_pair_weights.primer_quality  = PAIR_WT_PRIMER_PENALTY;
    a->pr_pair_weights.io_quality      = PAIR_WT_IO_PENALTY;
    a->pr_pair_weights.diff_tm         = PAIR_WT_DIFF_TM;
    a->pr_pair_weights.compl_any       = PAIR_WT_COMPL_ANY;
    a->pr_pair_weights.compl_end       = PAIR_WT_COMPL_END;
    a->pr_pair_weights.repeat_sim      = PAIR_WT_REP_SIM;
    a->pr_pair_weights.product_tm_lt   = PAIR_WT_PRODUCT_TM_LT;
    a->pr_pair_weights.product_tm_gt   = PAIR_WT_PRODUCT_TM_GT;
    a->pr_pair_weights.product_size_lt = PAIR_WT_PRODUCT_SIZE_LT;
    a->pr_pair_weights.product_size_gt = PAIR_WT_PRODUCT_SIZE_GT;
    a->lib_ambiguity_codes_consensus   = LIB_AMBIGUITY_CODES_CONSENSUS;
}

/* ------------------------------------------------------------------------ */
/* The main primer3 interface */

/* Allocate a new primer3 state */
primer3_state *
create_primer3_state(void)
{
  primer3_state *state = (primer3_state *)malloc(sizeof(*state));
  if (!state)
    return NULL;

  state->f   = malloc(sizeof(*state->f) * INITIAL_LIST_LEN);
  state->r   = malloc(sizeof(*state->r) * INITIAL_LIST_LEN);
  state->mid = malloc(sizeof(*state->mid) * INITIAL_LIST_LEN);

  if (state->f == NULL || state->r == NULL || state->mid == NULL)
    return NULL;

  state->f_len = state->r_len = state->mid_len = INITIAL_LIST_LEN;

  state->n_f = state->n_r = state->n_m = 0;

  state->best_pairs.storage_size = 0;
  state->best_pairs.pairs = NULL;
  state->best_pairs.num_pairs = 0;

  state->err.system_errno = 0;
  state->err.local_errno = 0;
  state->err.error_msg = NULL;

  return state;
}

/* Deallocate a primer3 state */
void
destroy_primer3_state(primer3_state *state)
{
    if (!state)
	return;

    free_repeat_sim_score(state);

    if (state->f)
	free(state->f);
    if (state->r)
	free(state->r);
    if (state->mid)
	free(state->mid);
    if (state->best_pairs.storage_size != 0 && state->best_pairs.pairs)
	free(state->best_pairs.pairs);

    free(state);
}

dpal_arg_holder *
create_dpal_arg_holder () {

  dpal_arg_holder *h = pr_safe_malloc(sizeof(dpal_arg_holder));

  h->local = pr_safe_malloc(sizeof(*h->local));
  set_dpal_args(h->local);
  h->local->flag = DPAL_LOCAL;

  h->end = pr_safe_malloc(sizeof(*h->end));
  set_dpal_args(h->end);
  h->end->flag = DPAL_GLOBAL_END;

  h->local_end = pr_safe_malloc(sizeof(*h->local_end));
  set_dpal_args(h->local_end);
  h->local_end->flag = DPAL_LOCAL_END;

  h->local_ambig  = pr_safe_malloc(sizeof(*h->local_ambig));
  *h->local_ambig = *h->local;
  PR_ASSERT(dpal_set_ambiguity_code_matrix(h->local_ambig));

  h->local_end_ambig = pr_safe_malloc(sizeof(*h->local_end_ambig));
  *h->local_end_ambig = *h->local_end;
  PR_ASSERT(dpal_set_ambiguity_code_matrix(h->local_end_ambig));

  return h;
}

void
destroy_dpal_arg_holder(dpal_arg_holder *h) {
  free(h->local);
  free(h->end);
  free(h->local_end);
  free(h->local_ambig);
  free(h->local_end_ambig);
  free(h);
}

void
destroy_seq_args(seq_args *sa) {
  if (NULL != sa->internal_input) free(sa->internal_input);
  if (NULL != sa->left_input) free(sa->left_input);
  if (NULL != sa->right_input) free(sa->right_input);
  if (NULL != sa->sequence) free(sa->sequence);
  if (NULL != sa->quality)  free(sa->quality);
  if (NULL != sa->trimmed_seq) free(sa->trimmed_seq);

  /* edited by T. Koressaar for lowercase masking */
  if (NULL != sa->trimmed_orig_seq) free(sa->trimmed_orig_seq);

  if (NULL != sa->upcased_seq) free(sa->upcased_seq);
  if (NULL != sa->upcased_seq_r) free(sa->upcased_seq_r);
  if (NULL != sa->sequence_name) free(sa->sequence_name);
  if (NULL != sa->error.data) free(sa->error.data);
  if (NULL != sa->warning.data) free(sa->warning.data);
  free(sa);
}

static dpal_arg_holder *dpal_arg_to_use = NULL;


/* 
 * Find up to pa->num_return primer pairs for the sequence seq with t targets.
 * Set sa->error and return 1 on error; otherwise return 0.
 */
void
choose_primers(primer3_state *p3state,
	       primer_args *pa,
	       seq_args *sa)
{
    int i;       /* Loop index. */
    int int_num; /* Product size range counter. */
    pair_array_t p;
    pair_array_t *best_pairs = &p3state->best_pairs;

    PR_ASSERT(NULL != p3state);
    PR_ASSERT(NULL != sa);
    PR_ASSERT(NULL != sa);
    
    if (_pr_data_control(pa, sa) !=0 ) return;

    if (dpal_arg_to_use == NULL)
      dpal_arg_to_use = create_dpal_arg_holder();

    if (make_primer_lists(p3state, pa, sa, dpal_arg_to_use) != 0) {
	return;
    }

    if ((pa->primer_task == pick_hyb_probe_only 
	 || pa->primer_task == pick_pcr_primers_and_hyb_probe)
        && make_internal_oligos_list(p3state, pa, sa,
				     dpal_arg_to_use) != 0)
	return;

    /* Creates files with left, right, and internal oligos. */
    if (pa->file_flag) print_list(p3state, sa, pa);

    /* We sort _after_ printing lists to maintain the order of test output. */
    if (pa->primer_task != pick_left_only 
	&& pa->primer_task != pick_hyb_probe_only) 
      qsort(&p3state->r[0], p3state->n_r, sizeof(*p3state->r),
	    primer_rec_comp);

    if(pa->primer_task != pick_right_only
       && pa->primer_task != pick_hyb_probe_only) 
      qsort(&p3state->f[0], p3state->n_f, sizeof(*p3state->f),
	    primer_rec_comp);

    if(pa->primer_task == pick_hyb_probe_only)
      qsort(&p3state->mid[0], 
	    p3state->n_m, 
	    sizeof(*p3state->mid), 
	    primer_rec_comp);

    p.storage_size = p.num_pairs = 0;
    if (pa->primer_task == pick_pcr_primers 
	|| pa->primer_task == pick_pcr_primers_and_hyb_probe) {

      /* Look for pa->num_return best primer pairs. */
      for(int_num=0; int_num < pa->num_intervals; int_num++) {
	if(choose_pair(p3state, pa, sa, dpal_arg_to_use, int_num, &p)!=0)
	  continue;

	for (i = 0; i < p.num_pairs && best_pairs->num_pairs < pa->num_return;
	     i++)
	  if (!oligo_pair_seen(&p.pairs[i], best_pairs))
	    add_pair(&p.pairs[i], best_pairs);

	if (pa->num_return == best_pairs->num_pairs) break;
	p.num_pairs = 0;
      }
    }

    /* If it was necessary to use a left_input, right_input,
       or internal_oligo_input primer that was
       unacceptable, then add warnings. */
    if (pa->pick_anyway) {
      if (sa->left_input) {
	add_must_use_warnings(sa, "Left primer", &sa->left_expl);
      }
      if (sa->right_input) {
	add_must_use_warnings(sa, "Right primer", &sa->right_expl);
      }
      if (sa->internal_input) {
	add_must_use_warnings(sa, "Hybridization probe", &sa->intl_expl);
      }
    }

    if (0 != p.storage_size) free(p.pairs);
    return;
}

/* Call this function only if the 'stat's contains
   the _errors_ associated with a given primer
   i.e. that primer was supplied by the caller
   and pick_anyway is set. */
static void
add_must_use_warnings(sa, text, stats)
  seq_args *sa;
  const char* text;
  const oligo_stats *stats;
{
  const char *sep = "/";
  pr_append_str s;

  s.data = NULL;
  s.storage_size = 0;

  if (stats->ns) pr_append_w_sep(&s, sep, "Too many Ns");
  if (stats->target) pr_append_w_sep(&s, sep, "Overlaps Target");
  if (stats->excluded) pr_append_w_sep(&s, sep, "Overlaps Excluded Region");
  if (stats->gc) pr_append_w_sep(&s, sep, "Unacceptable GC content");
  if (stats->gc_clamp) pr_append_w_sep(&s, sep, "No GC clamp");
  if (stats->temp_min) pr_append_w_sep(&s, sep, "Tm too low");
  if (stats->temp_max) pr_append_w_sep(&s, sep, "Tm too high");
  if (stats->compl_any) pr_append_w_sep(&s, sep, "High self complementarity");
  if (stats->compl_end) 
    pr_append_w_sep(&s, sep, "High end self complementarity");
  if (stats->repeat_score)
    pr_append_w_sep(&s, sep, "High similarity to mispriming or mishyb library");
  if (stats->poly_x) pr_append_w_sep(&s, sep, "Long poly-X");
  if (stats->seq_quality) pr_append_w_sep(&s, sep, "Low sequence quality");
  if (stats->stability) pr_append_w_sep(&s, sep, "High 3' stability");
  if (stats->no_orf) pr_append_w_sep(&s, sep, "Would not amplify any ORF");

  /* edited by T. Koressaar for lowercase masking: */
  if (stats->gmasked)
    pr_append_w_sep(&s, sep, "Masked with lowercase letter");

  if (s.data) {
    pr_append_new_chunk(&sa->warning, text);
    pr_append(&sa->warning, " is unacceptable: ");
    pr_append(&sa->warning, s.data);
    free(s.data);
  }

}

/* Return 1 iff pair is already in the first num_pairs elements of retpair. */
static int
oligo_pair_seen(pair, retpair)
    const primer_pair *pair;
    const pair_array_t *retpair;
{
  const primer_pair *q, *stop;

  /* retpair might not have any pairs in it yet (add_pair
     allocates memory for retpair->pairs. */
  if (retpair->num_pairs == 0)
    return 0;

  q = &retpair->pairs[0];
  stop = &retpair->pairs[retpair->num_pairs];  
  
  for (; q < stop; q++) {
    if (q->left->start == pair->left->start
	&& q->left->length == pair->left->length
	&& q->right->start == pair->right->start
	&& q->right->length == pair->right->length) return 1;
  }
  return 0;
}

/* Add 'pair' to 'retpair'. */
static void
add_pair(pair, retpair)
    const primer_pair *pair;
    pair_array_t *retpair;
{
    if (0 == retpair->storage_size) {
	retpair->storage_size = INITIAL_NUM_RETURN;
	retpair->pairs 
	    = pr_safe_malloc(retpair->storage_size * sizeof(*retpair->pairs));
    } else if (retpair->storage_size == retpair->num_pairs) {
	retpair->storage_size *= 2;
	retpair->pairs
	    = pr_safe_realloc(retpair->pairs,
			      retpair->storage_size * sizeof(*retpair->pairs));
    }
    retpair->pairs[retpair->num_pairs] = *pair;
    retpair->num_pairs++;
}

/* Make lists of acceptable left and right primers. */
static int
make_primer_lists(p3state, pa, sa, dpal_arg_to_use)
     primer3_state *p3state;
     primer_args *pa;
     seq_args *sa;
     const dpal_arg_holder *dpal_arg_to_use;
{
    int left, right;
    int i,j,n,k,pr_min;
    int tar_l, tar_r, f_b, r_b;

    /* 
     * The position of the intial base of the rightmost stop codon that is
     * to the left of sa->start_codon_pos; valid only if sa->start_codon_pos
     * is "not null".  We will not want to include a stop codon to the right
     * of the the start codon in the amplicon.
     */
    int stop_codon1 = -1; 
       
    char s[MAX_PRIMER_LENGTH+1],s1[MAX_PRIMER_LENGTH+1];
    primer_rec h;

    left = right = 0;
    if (!PR_START_CODON_POS_IS_NULL(sa)) {
      stop_codon1 = find_stop_codon(sa->trimmed_seq, 
				    sa->start_codon_pos, -1);

      sa->stop_codon_pos = find_stop_codon(sa->trimmed_seq, 
					   sa->start_codon_pos,  1);
      sa->stop_codon_pos += sa->incl_s;
    }

    pr_min = INT_MAX;
    for(i=0;i<pa->num_intervals;i++)if(pa->pr_min[i]<pr_min)
       pr_min= pa->pr_min[i];

    PR_ASSERT(INT_MAX > (n=strlen(sa->trimmed_seq)));
    tar_r = 0;
    tar_l = n;
    for(i=0;i<sa->num_targets;i++) {
	if(sa->tar[i][0]>tar_r)tar_r = sa->tar[i][0];
	if(sa->tar[i][0]+sa->tar[i][1]-1<tar_l)tar_l=
	    sa->tar[i][0]+sa->tar[i][1]-1;
    }

    if (_PR_DEFAULT_POSITION_PENALTIES(pa)) {
      if (0 == tar_r) tar_r = n;
      if (tar_l == n) tar_l = 0;
    } else {
      tar_r = n;
      tar_l = 0;
    }

    if (pa->primer_task == pick_left_only)
      f_b = n - 1;
    else if (tar_r - 1 < n - pr_min + pa->primer_max_size - 1 
	&& !(pa->pick_anyway && sa->left_input))
      f_b=tar_r - 1;
    else 
      f_b = n - pr_min + pa->primer_max_size-1;
    k = 0;
    if(pa->primer_task != pick_right_only && pa->primer_task != pick_hyb_probe_only){
    left=n; right=0;
    for (i = f_b; i >= pa->primer_min_size - 1; i--) {
	s[0]='\0';
	for (j = pa->primer_min_size; j <= pa->primer_max_size; j++) {
	    if (i-j > n-pr_min-1 && pick_left_only != pa->primer_task) continue;
	    if (i-j+1>=0) {
		if (k >= p3state->f_len) {
		    p3state->f_len += (p3state->f_len >> 1);
		    p3state->f 
		      = pr_safe_realloc(p3state->f, 
					p3state->f_len * sizeof(*p3state->f));
		}
		h.start=i-j+1;
		h.length=j;
		h.repeat_sim.score = NULL;
		_pr_substr(sa->trimmed_seq,h.start,h.length,s);

		if (sa->left_input && strcmp_nocase(sa->left_input, s))
		  continue;

		h.must_use = (sa->left_input && pa->pick_anyway);

		if (pa->explain_flag) sa->left_expl.considered++;

		if (!PR_START_CODON_POS_IS_NULL(sa)
		/* Make sure the primer would amplify at least part of
		   the ORF. */
		    && (0 != (h.start - sa->start_codon_pos) % 3
			|| h.start <= stop_codon1
			|| (sa->stop_codon_pos != -1 
			    && h.start >= sa->stop_codon_pos))) {
		  if (pa->explain_flag) sa->left_expl.no_orf++;
		  if (!pa->pick_anyway) continue;
		}

		h.repeat_sim.score = NULL;
		oligo_param(pa, &h, OT_LEFT, dpal_arg_to_use, 
			    sa, &sa->left_expl);
		if (OK_OR_MUST_USE(&h)) {
		  h.quality = p_obj_fn(pa, &h, 0);
		  p3state->f[k] = h;
		  if (p3state->f[k].start < left)
		    left=p3state->f[k].start;
		  k++;
		} else if (h.ok==OV_TOO_MANY_NS || h.ok==OV_INTERSECT_TARGET
			   || h.ok==OV_SELF_ANY || h.ok==OV_END_STAB
			   || h.ok==OV_POLY_X || h.ok==OV_EXCL_REGION || h.ok==OV_GC_CLAMP
			   || h.ok == OV_SEQ_QUALITY || h.ok == OV_LIB_SIM ) {
		  /* Break from the inner for loop, because there is no
		     legal longer oligo with the same 3' sequence. */
		  break;
		}
	    }
	    else break;
	}
    }
    }
    p3state->n_f = k;


    if (pa->primer_task == pick_right_only)
      r_b = 0;
    else if (tar_l+1>pr_min - pa->primer_max_size
	&& !(pa->pick_anyway && sa->right_input))
      r_b = tar_l+1;
    else 
      r_b = pr_min - pa->primer_max_size;
    k = 0;
    if(pa->primer_task != pick_left_only 
       && pa->primer_task != pick_hyb_probe_only) {
    for(i=r_b; i<=n-pa->primer_min_size; i++) {
	s[0]='\0';
	for(j = pa->primer_min_size; j <= pa->primer_max_size; j++) {
	    if (i+j<pr_min && pa->primer_task != pick_right_only) continue;
	    if(i+j-1<n) {
		if (k >= p3state->r_len) {
		    p3state->r_len += (p3state->r_len >> 1);
		    p3state->r 
		      = pr_safe_realloc(p3state->r, 
					p3state->r_len * sizeof(*p3state->r));
		}
		h.start=i+j-1;
		h.length=j;
		h.repeat_sim.score = NULL;
		_pr_substr(sa->trimmed_seq,  i, j, s);
		_pr_reverse_complement(s, s1);

		if (sa->right_input && strcmp_nocase(sa->right_input, s1))
		  continue;
		h.must_use = (sa->right_input && pa->pick_anyway);

		h.repeat_sim.score = NULL;
		oligo_param(pa, &h, OT_RIGHT, dpal_arg_to_use,
			    sa, &sa->right_expl);
		sa->right_expl.considered++;
		if (OK_OR_MUST_USE(&h)) {
		  h.quality = p_obj_fn(pa, &h, 0);
		  p3state->r[k] = h;
		  if (p3state->r[k].start > right)
		    right = p3state->r[k].start;
		  k++;
		} else if (h.ok==OV_TOO_MANY_NS || h.ok==OV_INTERSECT_TARGET
		    || h.ok==OV_SELF_ANY || h.ok==OV_END_STAB
		    || h.ok==OV_POLY_X || h.ok==OV_EXCL_REGION || h.ok==OV_GC_CLAMP
		    || h.ok == OV_SEQ_QUALITY || h.ok == OV_LIB_SIM ) {
		  /* Break from the inner for loop, because there is no
		     legal longer oligo with the same 3' sequence. */
		  break;
		}
	    }
	    else break;
	}
    }
    }
    p3state->n_r=k;

    /* 
     * Return 1 if one of lists is empty or if leftmost left primer and
     * rightmost right primer do not provide sufficient product size.
     */
    sa->left_expl.ok = p3state->n_f;
    sa->right_expl.ok = p3state->n_r;
    if ((pa->primer_task != pick_right_only 
	 && pa->primer_task != pick_hyb_probe_only 
	 && 0 == p3state->n_f)
	|| 
	((pa->primer_task != pick_left_only && 
	  pa->primer_task != pick_hyb_probe_only) 
	 && 0 == p3state->n_r))
      return 1;
    else if ((pa->primer_task == pick_pcr_primers || 
		  pa->primer_task == pick_pcr_primers_and_hyb_probe) 
		  && right - left < pr_min - 1) {
	sa->pair_expl.product    = 1;
	sa->pair_expl.considered = 1;
	return 1;
    } else return 0;
}

/* 
 * Make complete list of acceptable internal oligos in mid.  Place the number
 * of valid elements in mid in *n_m.  Return 1 if there are no acceptable
 * internal oligos; otherwise return 0.
 */
static int
make_internal_oligos_list(p3state, pa, sa, dpal_arg_to_use)
     primer3_state *p3state;
     const primer_args *pa;
     seq_args *sa;
     const dpal_arg_holder *dpal_arg_to_use;
{
  int i, j, n, k;

  char s[MAX_PRIMER_LENGTH+1];
  primer_rec h;

  if (NULL == p3state->mid) {
    p3state->mid_len = INITIAL_LIST_LEN;
    p3state->mid = pr_safe_malloc(sizeof(*p3state->mid) * p3state->mid_len);
  }

  n = strlen(sa->trimmed_seq);
  k = 0;
  for(i = n - 1; i >= pa->io_primer_min_size-1; i--) {
    s[0] = '\0';
    for(j = pa->io_primer_min_size; j <=pa->io_primer_max_size; j++) {
      if(i-j < -1) break;
      if (k >= p3state->mid_len) {
	p3state->mid_len += (p3state->mid_len >> 1);
	p3state->mid = pr_safe_realloc(p3state->mid, p3state->mid_len * sizeof(*p3state->mid));
      }
      h.start = i - j +1;
      h.length = j;
      h.repeat_sim.score = NULL;
      _pr_substr(sa->trimmed_seq, h.start, h.length,s);

      if (sa->internal_input && strcmp_nocase(sa->internal_input, s))
	continue;
      h.must_use = (sa->internal_input && pa->pick_anyway);

      h.repeat_sim.score = NULL;
      oligo_param(pa, &h, OT_INTL, dpal_arg_to_use,
		  sa, &sa->intl_expl);
      sa->intl_expl.considered++;
      if (OK_OR_MUST_USE(&h)) {
	h.quality = p_obj_fn(pa, &h, 2);
	p3state->mid[k] = h;
	k++;
      } else if (h.ok==OV_TOO_MANY_NS || h.ok==OV_INTERSECT_TARGET
		 || h.ok==OV_SELF_ANY || h.ok==OV_POLY_X
		 || h.ok==OV_EXCL_REGION || h.ok==OV_GC_CLAMP
		 || h.ok==OV_SEQ_QUALITY || h.ok==OV_LIB_SIM ) {
	/* Break from the inner for loop, because there is no
           legal longer oligo with the same 3' sequence. */
	break;
      }
    }
  }
  p3state->n_m = k;
  sa->intl_expl.ok = p3state->n_m;
  if (p3state->n_m==0) return 1;
  else return 0;
}

/*
 * Compute various characteristics of the oligo, and determine
 * if it is acceptable.
 */
#define OUTSIDE_START_WT  30.0
#define INSIDE_START_WT   20.0
#define INSIDE_STOP_WT   100.0
#define OUTSIDE_STOP_WT    0.5
static void
oligo_param(pa, h, l, dpal_arg_to_use, sa, stats)
    const primer_args *pa;
    primer_rec *h;
    oligo_type l;
    const dpal_arg_holder *dpal_arg_to_use;
    seq_args *sa;
    oligo_stats *stats;
{
    int i,j,k, min_q, min_end_q;
    int poly_x, max_poly_x;
    int must_use = h->must_use;
    const char *seq = sa->trimmed_seq;
    char s1[MAX_PRIMER_LENGTH+1], s1_rev[MAX_PRIMER_LENGTH+1];



    h->ok = OV_UNINITIALIZED;
    h->target = h->gc_content = h->num_ns=h->excl=0;

    h->template_mispriming = h->template_mispriming_r = ALIGN_SCORE_UNDEF;

    PR_ASSERT(OT_LEFT == l || OT_RIGHT == l || OT_INTL == l);
    
    if (OT_LEFT == l || OT_INTL == l) {j = h->start; k=j+h->length-1;}
    else {j = h->start-h->length+1; k=h->start;}

    PR_ASSERT(k >= 0);
    PR_ASSERT(k < TRIMMED_SEQ_LEN(sa));
   
    /* edited by T. Koressaar for lowercase masking */
    if(pa->lowercase_masking==1) {
      if(l==OT_LEFT) {
	 check_if_lowercase_masked(k, sa->trimmed_orig_seq,h);
      }
      if(l==OT_RIGHT) {
	 check_if_lowercase_masked(j, sa->trimmed_orig_seq,h);
      }
      if(h->ok==OV_GMASKED) {
	 stats->gmasked++;
	 if (!must_use) return;
      }
    }
    /* end T. Koressar's changes */

    gc_and_n_content(j, k-j+1, sa->trimmed_seq, h);

    if (((OT_LEFT == l || OT_RIGHT == l) && 
	 h->num_ns > pa->num_ns_accepted) || 
	(OT_INTL == l && h->num_ns > pa->io_num_ns_accepted) ) {
      h->ok = OV_TOO_MANY_NS;
      stats->ns++;
      if (!must_use) return;
    }

    /* Upstream error checking has ensured that we use non-default position
       penalties only when there is 0 or 1 target. */
    PR_ASSERT(sa->num_targets <= 1 || _PR_DEFAULT_POSITION_PENALTIES(pa));
    if (l < 2 
	&& _PR_DEFAULT_POSITION_PENALTIES(pa)
	&& oligo_overlaps_interval(j, k-j+1, sa->tar, sa->num_targets)) {
      h->position_penalty = 0.0;
      h->position_penalty_infinite = '\1';
      h->target = 1;
    } else if (l < 2 && !_PR_DEFAULT_POSITION_PENALTIES(pa)
	     && 1 == sa->num_targets) {
      compute_position_penalty(pa, sa, h, l);
      if (h->position_penalty_infinite) h->target = 1;
    } else {
      h->position_penalty = 0.0;
      h->position_penalty_infinite = '\0';
    }

    if (!PR_START_CODON_POS_IS_NULL(sa)) {
      if (OT_LEFT == l) {
	if (sa->start_codon_pos > h->start)
	  h->position_penalty
	    = (sa->start_codon_pos - h->start) * OUTSIDE_START_WT;
	else
	  h->position_penalty 
	    = (h->start - sa->start_codon_pos) * INSIDE_START_WT;
      }
      else if (OT_RIGHT == l) {
	if (-1 == sa->stop_codon_pos) {
	  h->position_penalty = (TRIMMED_SEQ_LEN(sa) - h->start - 1) * INSIDE_STOP_WT;
	} else if (sa->stop_codon_pos < h->start)
	  h->position_penalty
	    = (h->start - sa->stop_codon_pos) * OUTSIDE_STOP_WT;
	else
	  h->position_penalty
	    = (sa->stop_codon_pos - h->start) * INSIDE_STOP_WT;
      }
    }

    if (l < 2 && oligo_overlaps_interval(j, k-j+1, sa->excl, sa->num_excl))
	h->excl = 1;

    if (l == 2 && oligo_overlaps_interval(j, k-j+1, sa->excl_internal,
					  sa->num_internal_excl))
	h->excl = 1;

    if(l < 2 && h->target==1) {
	h->ok = OV_INTERSECT_TARGET;
	stats->target++;
	if (!must_use) return;
    }

    if(h->excl==1){
	h->ok = OV_EXCL_REGION;
	stats->excluded++;
	if (!must_use) return;
    }

    if((l<2 && (h->gc_content< pa->min_gc || h->gc_content > pa->max_gc)) ||
       (l==2 && (h->gc_content< pa->io_min_gc || h->gc_content > pa->io_max_gc))){
	h->ok = OV_GC_CONTENT;
	stats->gc++;
	if (!must_use) return;
    }
    if(pa->gc_clamp != 0){
       if(OT_LEFT == l){
	 for(i=k-pa->gc_clamp+1; i<= k; i++)if(seq[i] !='G'&&seq[i] !='C'){
	   h->ok = OV_GC_CLAMP;
	   stats->gc_clamp++;
	   if (!must_use) return; else break;
	 }
       }
       if(OT_RIGHT == l){
	 for(i=j; i<j+pa->gc_clamp; i++)if(seq[i] != 'G' && seq[i] != 'C'){
	   h->ok = OV_GC_CLAMP;
	   stats->gc_clamp++;
	   if (!must_use) return; else break;
	 }
       }
    }
            
    check_sequence_quality(pa, h, l, sa, j, k, &min_q, &min_end_q);
    if(OT_LEFT == l || OT_RIGHT == l) {
      if (min_q < pa->min_quality) {
	h->ok = OV_SEQ_QUALITY;
	stats->seq_quality++;
	if (!must_use) return;
      } else if (min_end_q < pa->min_end_quality) {
	h->ok = OV_SEQ_QUALITY;
	stats->seq_quality++;
	if (!must_use) return;
      }
    } else if (OT_INTL == l) {
      if (min_q < pa->io_min_quality) {
	h->ok = OV_SEQ_QUALITY;
	stats->seq_quality++;
	if (!must_use) return;
      }
    } else {
      PR_ASSERT(0); /* Programming error. */
    }

    if(OT_LEFT == l || OT_RIGHT == l) max_poly_x = pa->max_poly_x;     
    else max_poly_x = pa->io_max_poly_x;
    if(max_poly_x > 0) {
          poly_x = 1;
	  for(i=j+1;i<=k;i++){
                if(seq[i] == seq[i-1]||seq[i] == 'N'){
		       poly_x++;
		       if(poly_x > max_poly_x){
			     h->ok = OV_POLY_X;
			     stats->poly_x++;
			     if (!must_use) return; else break;
                       }
                }
		else poly_x = 1;
          }
     }

    _pr_substr(seq,j,k-j+1,s1);
                   
   if(OT_LEFT == l || OT_RIGHT == l)
     h->temp 
     = seqtm(s1, pa->dna_conc, pa->salt_conc, pa->divalent_conc, pa->dntp_conc, 
	     MAX_NN_TM_LENGTH,
	     pa->tm_santalucia,
	     pa->salt_corrections); 
   else
     h->temp
     = seqtm(s1, pa->io_dna_conc, pa->io_salt_conc, pa->io_divalent_conc, pa->io_dntp_conc,
	     MAX_NN_TM_LENGTH,
	     pa->tm_santalucia,
	     pa->salt_corrections);
         
    if (((l == OT_LEFT || l == OT_RIGHT) && h->temp < pa->min_tm)
	|| (l==OT_INTL && h->temp<pa->io_min_tm)) {
	h->ok = OV_TM_LOW;
	stats->temp_min++;
	if (!must_use) return;
    }
    if (((OT_LEFT == l || OT_RIGHT == l) && h->temp>pa->max_tm) 
	|| (OT_INTL == l && h->temp>pa->io_max_tm)) {
	h->ok = OV_TM_HIGH;
	stats->temp_max++;
	if (!must_use) return;
    }
    if (OT_LEFT == l) {
      if ((h->end_stability = end_oligodg(s1, 5,
					  pa->tm_santalucia))
	  > pa->max_end_stability) {
	h->ok = OV_END_STAB;
	stats->stability++;
	if (!must_use) return;
      }
    } else if (OT_RIGHT == l) {
      _pr_reverse_complement(s1, s1_rev);
      if ((h->end_stability = end_oligodg(s1_rev, 5,
					  pa->tm_santalucia))
	  > pa->max_end_stability) {
	  h->ok = OV_END_STAB;
	  stats->stability++;
	  if (!must_use) return;
      }
    }

    if (must_use
	|| pa->file_flag 
	|| (pa->primer_task != pick_pcr_primers && 
	    pa->primer_task != pick_pcr_primers_and_hyb_probe)
	|| ((OT_RIGHT == l || OT_LEFT == l) &&
	    (pa->primer_weights.compl_any || pa->primer_weights.compl_end))
	|| (OT_INTL == l 
	    && (pa->io_weights.compl_any || pa->io_weights.compl_end))) {

      oligo_compl(h, pa, sa, l, dpal_arg_to_use);

      if (h->ok != OV_UNINITIALIZED && !must_use) {
	PR_ASSERT(h->ok != OV_OK);
	return; 
      }

    } else
      h->self_any = h->self_end  = ALIGN_SCORE_UNDEF;

    if (must_use
	|| pa->file_flag
	||(pa->primer_task != pick_pcr_primers && 
	   pa->primer_task != pick_pcr_primers_and_hyb_probe)
	|| ((OT_RIGHT == l || OT_LEFT == l) && pa->primer_weights.repeat_sim)
	|| ((OT_RIGHT == l || OT_LEFT == l) 
	    && pa->primer_weights.template_mispriming)
	|| (OT_INTL == l && pa->io_weights.repeat_sim)) {

      oligo_mispriming(h, pa, sa, l, 
		       dpal_arg_to_use->local_end, 
		       dpal_arg_to_use);

    }

    if (OV_UNINITIALIZED == h->ok) h->ok = OV_OK;
}
#undef OUTSIDE_START_WT
#undef INSIDE_START_WT
#undef INSIDE_STOP_WT
#undef OUTSIDE_STOP_WT

static void
check_sequence_quality(pa, h, l, sa, j, k, r_min_q, r_min_q_end)
    const primer_args *pa;
    primer_rec *h;
    oligo_type l;
    const seq_args *sa;
    int j, k;
    int *r_min_q, *r_min_q_end;
{
  int i,min_q, min_q_end, m, q;

  q = min_q = min_q_end = pa->quality_range_max;

  if (NULL != sa->quality) {

    if(OT_LEFT == l || OT_RIGHT == l){
      min_q = pa->min_quality;
      min_q_end = pa->min_end_quality;
    }
    else {min_q = min_q_end = pa->io_min_quality;} 

    if (OT_LEFT == l || OT_INTL == l) {

      for(i = k-4; i <= k; i++) {
	if(i < j) continue;
	m = sa->quality[i + sa->incl_s];
	if (m < q) q = m;
      }
      min_q_end = q;

      for(i = j; i<=k-5; i++) {
	m = sa->quality[i + sa->incl_s];
	if (m < q) q = m;
      }
      min_q = q;

    } else if (OT_RIGHT == l) {
      for(i = j; i < j+5; i++) {
	if(i > k) break;
	m = sa->quality[i + sa->incl_s];
	if (m < q) q = m;
      }
      min_q_end = q;
       
      for(i = j+5; i <= k; i++) {
	m = sa->quality[i + sa->incl_s];
	if (m < q) q = m;
      }
      min_q = q;
    } else {
      PR_ASSERT(0); /* Programming error. */
    }
  }
  h->seq_quality = *r_min_q = min_q;
  *r_min_q_end = min_q_end;
}

/* 
 * Set h->gc_content to the GC % of the 'len' bases starting at 'start' in
 * 'sequence' (excluding 'N's).  Set h->num_ns to the number of 'N's in that
 * subsequence.
 */
static void
gc_and_n_content(start, len, sequence, h)
    const int start, len;
    const char *sequence;
    primer_rec *h;
{
    const char* p = &sequence[start];
    const char* stop = p + len;
    int num_gc = 0, num_gcat = 0, num_n = 0;
    while (p < stop) {
	if ('N' == *p) 
	    num_n++;
	else {
	    num_gcat++;
	    if ('C' == *p || 'G' == *p) num_gc++;
	}
	p++;
    }
    h->num_ns = num_n;
    if (0 == num_gcat) h->gc_content= 0.0;
    else h->gc_content = 100.0 * ((double)num_gc)/num_gcat;
}

static int
oligo_overlaps_interval(start, len, intervals, num_intervals)
    const int start, len;
    interval_array_t intervals;
    const int num_intervals;
{
    int i;
    int last = start + len - 1;
    for (i = 0; i < num_intervals; i++)
	if (!(last < intervals[i][0] 
	      || start > (intervals[i][0] + intervals[i][1] - 1)))
	    return 1;
    return 0;
}

/* Calculate the part of the objective function due to one primer. */
static double
p_obj_fn(pa, h, j)
    const primer_args *pa;
    primer_rec *h;
    int  j;
{
  double sum;

  sum = 0;
  if (j == OT_LEFT || j == OT_RIGHT) {
      if(pa->primer_weights.temp_gt && h->temp > pa->opt_tm)
	   sum += pa->primer_weights.temp_gt * (h->temp - pa->opt_tm);
      if(pa->primer_weights.temp_lt && h->temp < pa->opt_tm)
	   sum += pa->primer_weights.temp_lt * (pa->opt_tm - h->temp);

      if(pa->primer_weights.gc_content_gt && h->gc_content > pa->opt_gc_content)
	   sum += pa->primer_weights.gc_content_gt 
	     * (h->gc_content - pa->opt_gc_content);
      if(pa->primer_weights.gc_content_lt && h->gc_content < pa->opt_gc_content)
	   sum += pa->primer_weights.gc_content_lt 
	     * (pa->opt_gc_content - h->gc_content);

      if(pa->primer_weights.length_lt && h->length < pa->primer_opt_size)
	   sum += pa->primer_weights.length_lt * (pa->primer_opt_size - h->length);
      if(pa->primer_weights.length_gt && h->length > pa->primer_opt_size)
	   sum += pa->primer_weights.length_gt * (h->length - pa->primer_opt_size);
      if(pa->primer_weights.compl_any)
	   sum += pa->primer_weights.compl_any * h->self_any
	     / PR_ALIGN_SCORE_PRECISION;
      if(pa->primer_weights.compl_end)
	   sum += pa->primer_weights.compl_end * h->self_end
	     / PR_ALIGN_SCORE_PRECISION;
      if(pa->primer_weights.num_ns)
	   sum += pa->primer_weights.num_ns * h->num_ns;
      if(pa->primer_weights.repeat_sim)
	   sum += pa->primer_weights.repeat_sim 
	     * h->repeat_sim.score[h->repeat_sim.max]
	     / PR_ALIGN_SCORE_PRECISION;
      if (!h->target) {
	/* We might be evaluating p_obj_fn with h->target if
	   the client supplied 'pick_anyway' and specified
	   a primer or oligo. */
	PR_ASSERT(!h->position_penalty_infinite);
	if(pa->primer_weights.pos_penalty)
	  sum += pa->primer_weights.pos_penalty * h->position_penalty;
      }
      if(pa->primer_weights.end_stability)
	   sum += pa->primer_weights.end_stability * h->end_stability;
      if(pa->primer_weights.seq_quality)
	   sum += pa->primer_weights.seq_quality * 
			     (pa->quality_range_max - h->seq_quality);

      if (pa->primer_weights.template_mispriming) {
	PR_ASSERT(oligo_max_template_mispriming(h) != ALIGN_SCORE_UNDEF);
	sum += pa->primer_weights.template_mispriming * 
	  oligo_max_template_mispriming(h);
      }

      return sum;
  } else if (j == OT_INTL) {
      if(pa->io_weights.temp_gt && h->temp > pa->io_opt_tm)
	 sum += pa->io_weights.temp_gt * (h->temp - pa->io_opt_tm);
      if(pa->io_weights.temp_lt && h->temp < pa->io_opt_tm)
	 sum += pa->io_weights.temp_lt * (pa->io_opt_tm - h->temp);

      if(pa->io_weights.gc_content_gt && h->gc_content > pa->io_opt_gc_content)
	   sum += pa->io_weights.gc_content_gt
	     * (h->gc_content - pa->io_opt_gc_content);
      if(pa->io_weights.gc_content_lt && h->gc_content < pa->io_opt_gc_content)
	   sum += pa->io_weights.gc_content_lt
	     * (pa->io_opt_gc_content - h->gc_content);

      if(pa->io_weights.length_lt && h->length < pa->io_primer_opt_size)
	 sum += pa->io_weights.length_lt * (pa->io_primer_opt_size - h->length);
      if(pa->io_weights.length_gt && h->length  > pa->io_primer_opt_size)
	 sum += pa->io_weights.length_gt * (h->length - pa->io_primer_opt_size);
      if(pa->io_weights.compl_any)
	 sum += pa->io_weights.compl_any * h->self_any / PR_ALIGN_SCORE_PRECISION;
      if(pa->io_weights.compl_end)
	 sum += pa->io_weights.compl_end * h->self_end / PR_ALIGN_SCORE_PRECISION;
      if(pa->io_weights.num_ns)
	 sum += pa->io_weights.num_ns * h->num_ns;
      if(pa->io_weights.repeat_sim)
	 sum += pa->io_weights.repeat_sim
	   * h->repeat_sim.score[h->repeat_sim.max]
	   / PR_ALIGN_SCORE_PRECISION;
      if(pa->io_weights.seq_quality)
	 sum += pa->io_weights.seq_quality * 
		     (pa->quality_range_max - h->seq_quality);
      return sum;
  } else {
    PR_ASSERT(0); /* Programmig error. */
  }
}

/* Return max of h->template_mispriming and h->template_mispriming_r (max
   template mispriming on either strand). */
short
oligo_max_template_mispriming(h)
  const primer_rec *h;
{
  return h->template_mispriming > h->template_mispriming_r ?
    h->template_mispriming : h->template_mispriming_r;
}

static void
print_list(p3state, sa, pa)
     const primer3_state *p3state;
     const seq_args *sa;
     const primer_args *pa;
{
    int first_base_index = pa->first_base_index;

    if(pa->primer_task != pick_right_only && pa->primer_task != pick_hyb_probe_only)
       create_and_print_file(sa, p3state->n_f, p3state->f, OT_LEFT, first_base_index, 
			  NULL != pa->repeat_lib.repeat_file, ".for");

    if(pa->primer_task != pick_left_only && pa->primer_task != pick_hyb_probe_only)
       create_and_print_file(sa, p3state->n_r, p3state->r, OT_RIGHT, first_base_index,
			  NULL != pa->repeat_lib.repeat_file, ".rev");

    if ( pa->primer_task == pick_pcr_primers_and_hyb_probe 
				|| pa->primer_task == pick_hyb_probe_only)
      create_and_print_file(sa, p3state->n_m, p3state->mid, OT_INTL,
			    first_base_index,
			    NULL != pa->io_mishyb_library.repeat_file,
			    ".int");
}

static void
create_and_print_file(sa, n, oligo_arr, o_type,
		      first_base_index, print_lib_sim, ext)
    const seq_args *sa;
    int n;
    const primer_rec oligo_arr[];
    const oligo_type o_type;
    const int first_base_index, print_lib_sim;
    const char *ext;
{
    int i;
    char *file = pr_safe_malloc(strlen(sa->sequence_name) + strlen(ext) + 1);
    FILE *fh;

    strcpy(file, sa->sequence_name);
    strcat(file,ext);
    fh = safe_fopen(file,"w");
    print_list_header(fh, o_type, first_base_index, print_lib_sim);
    for(i=0; i<n; i++) 
	print_oligo(fh, sa, i, &oligo_arr[i], o_type,
		    first_base_index, print_lib_sim);
    fclose(fh);
    free(file);
}

static void
print_list_header(fh, type, first_base_index, print_lib_sim)
    FILE *fh;
    oligo_type type;
    int first_base_index, print_lib_sim;
{
    fprintf(fh, "ACCEPTABLE %s\n",
            OT_LEFT == type ? "LEFT PRIMERS"
            : OT_RIGHT == type ? "RIGHT PRIMERS" : "INTERNAL OLIGOS");
    fprintf(fh, "                               %4d-based     ", 
	    first_base_index);
    if (print_lib_sim)
	fprintf(fh, "#               self  self   lib  qual-\n");
    else
	fprintf(fh, "#               self  self  qual-\n");
    fprintf(fh, "   # sequence                       start ln  "); 
    if (print_lib_sim)
	fprintf(fh, "N   GC%%     Tm   any   end   sim   lity\n");
    else 
	fprintf(fh, "N   GC%%     Tm   any   end   lity\n");
}

static void
print_oligo(fh, sa, index, h, type, first_base_index, print_lib_sim)
    FILE *fh;
    const seq_args *sa;
    int index;
    const primer_rec *h;
    oligo_type type;
    int first_base_index, print_lib_sim;
{
    char *p = (OT_RIGHT != type) 
	? pr_oligo_sequence(sa, h) : pr_oligo_rev_c_sequence(sa, h);
    if (print_lib_sim)
	fprintf(fh,
		"%4d %-30s %5d %2d %2d %5.2f %5.3f %5.2f %5.2f %5.2f %6.3f\n",
		index, p, h->start+sa->incl_s + first_base_index, 
		h->length,
		h->num_ns, h->gc_content, h->temp,
		h->self_any / PR_ALIGN_SCORE_PRECISION,
		h->self_end / PR_ALIGN_SCORE_PRECISION,
		h->repeat_sim.score[h->repeat_sim.max] /PR_ALIGN_SCORE_PRECISION,
		h->quality);
    else
	fprintf(fh,
		"%4d %-30s %5d %2d %2d %5.2f %5.3f %5.2f %5.2f %6.3f\n",
		index, p, h->start+sa->incl_s + first_base_index, 
		h->length,
		h->num_ns, h->gc_content, h->temp,
		h->self_any / PR_ALIGN_SCORE_PRECISION,
		h->self_end / PR_ALIGN_SCORE_PRECISION,
		h->quality);
}

static int
choose_pair(p3state, pa, sa,  dpal_arg_to_use, int_num, p)
     primer3_state *p3state;
     const primer_args *pa;
     seq_args *sa;
     const dpal_arg_holder *dpal_arg_to_use;
     int int_num;
     pair_array_t *p;
{
  int i,j;
  int k; /* 
	  * The number of acceptable primer pairs saved in p.
	  * (k <= pa->num_return.)
	  */
  int i0, n_last, n_int;
  primer_pair worst_pair; /* The worst pair among those being "remembered". */
  int i_worst;             /* The index within p of worst_pair. */

  primer_pair h;

  k=0; 

  i_worst = 0;
  n_last = p3state->n_f;
  for(i=0; i<p3state->n_r; i++) {
    /* 
     * Make a quick cut based on the the quality of the best left
     * primer.
     *
     * worst_pair must be defined if k >= pa->num_return.
     */
    if (!OK_OR_MUST_USE(&p3state->r[i])) continue;
    if (k >= pa->num_return 
	&& (p3state->r[i].quality + p3state->f[0].quality > worst_pair.pair_quality 
	    || worst_pair.pair_quality == 0))
      break;

    for(j=0;j<n_last;j++) {
      /* 
       * Invariant: if 2 pairs in p have the same pair_quality, then the
       * pair discovered second will have a higher index within p.
       *
       * This invariant is needed to produce consistent results when 2
       * pairs have the same pair_quality.
       */

      if (!OK_OR_MUST_USE(&p3state->r[i])) break;
      if (!OK_OR_MUST_USE(&p3state->f[j])) continue;
      if(k>= pa->num_return 
	 && (p3state->f[j].quality + p3state->r[i].quality > worst_pair.pair_quality
	     || worst_pair.pair_quality == 0)) {
	/* worst_pair must be defined if k >= pa->num_return. */
	n_last=j;
	break;
      }

      if (PAIR_OK ==
	  pair_param(p3state, pa, sa, j, i, int_num, &h, dpal_arg_to_use)) {

	if (!pa->pr_pair_weights.io_quality) {
	  h.pair_quality = obj_fn(pa, &h);
	  PR_ASSERT(h.pair_quality >= 0.0);
	}


	if ( pa->primer_task == pick_pcr_primers_and_hyb_probe
	     && choose_internal_oligo(p3state,
				      h.left, h.right,
				      &n_int, sa, pa, 
				      dpal_arg_to_use)!=0) {
	  sa->pair_expl.internal++;
	  continue;
	}
	sa->pair_expl.ok++;
	if (k < pa->num_return) {
	  if ( pa->primer_task == pick_pcr_primers_and_hyb_probe) 
	    h.intl = &p3state->mid[n_int];
	  if(pa->pr_pair_weights.io_quality) {
	    h.pair_quality = obj_fn(pa, &h);
	    PR_ASSERT(h.pair_quality >= 0.0);
	  }

	  add_pair(&h, p);
	  /* p->pairs[k] = h; */
	  if (k == 0 || primer_pair_comp(&h, &worst_pair) > 0){
	    worst_pair = h;
	    i_worst = k;
	  }
	  k++;
	} else {
	  if ( pa->primer_task == pick_pcr_primers_and_hyb_probe)
	    h.intl = &p3state->mid[n_int];
	  if(pa->pr_pair_weights.io_quality) {
	    h.pair_quality = obj_fn(pa, &h);
	    PR_ASSERT(h.pair_quality >= 0.0);
	  }

	  if (primer_pair_comp(&h, &worst_pair) < 0) {
	    /* 
	     * There are already pa->num_return results, and vl is better than
	     * the pa->num_return the quality found so far.
	     */
	    p->pairs[i_worst] = h;
	    worst_pair = h; /* h is a lower bound on the worst pair. */
	    for (i0 = 0; i0<pa->num_return; i0++)
	      if(primer_pair_comp(&p->pairs[i0], &worst_pair) > 0) {
		i_worst = i0;
		worst_pair = p->pairs[i0];

	      }
	  }
	}
      }
    }	
  }
  if(k!=0) qsort(p->pairs, k, sizeof(primer_pair), primer_pair_comp);
  p->num_pairs = k;
  if (k==0) return 1;
  else return 0;
}

/* Choose best internal oligo for given pair of left and right primers. */
static int
choose_internal_oligo(p3state, left, right, nm, sa, pa, dpal_arg_to_use)
     primer3_state *p3state;
     const primer_rec *left, *right;
     int *nm;
     seq_args *sa;
     const primer_args *pa;
     const dpal_arg_holder *dpal_arg_to_use;
{
   int i,k;
   double min;

   min = 1000000.;

   i = -1;

   for (k=0; k < p3state->n_m; k++) {

     if ((p3state->mid[k].start > (left->start + (left->length-1))) 
	&& ((p3state->mid[k].start + (p3state->mid[k].length-1))
	    < (right->start-right->length+1)) 
	&& (p3state->mid[k].quality < min) 
	&& (OK_OR_MUST_USE(&p3state->mid[k]))) {
       
       if (p3state->mid[k].self_any == ALIGN_SCORE_UNDEF){
	 oligo_compl(&p3state->mid[k], pa, sa, OT_INTL, dpal_arg_to_use);
	 if (!OK_OR_MUST_USE(&p3state->mid[k])) continue;
       }

       if (p3state->mid[k].repeat_sim.score == NULL) {
         oligo_mispriming(&p3state->mid[k], pa, sa, OT_INTL,
			  dpal_arg_to_use->local, dpal_arg_to_use);
	 if (!OK_OR_MUST_USE(&p3state->mid[k])) continue;
       }

       min = p3state->mid[k].quality;
       i=k;

     } /* if ((p3state->mid[k].start.... */

   }  /* for (k=0;..... */

   *nm = i;
   if(*nm < 0) return 1;
   return 0;
}

/* Compare function for sorting primer records. */
static int
primer_rec_comp(x1, x2)
    const void *x1, *x2;
{
    const primer_rec *a1 = x1, *a2 = x2;

    if(a1->quality < a2->quality) return -1;
    if (a1->quality > a2->quality) return 1;

    /* 
     * We want primer_rec_comp to always return a non-0 result, because
     * different implementations of qsort differ in how they treat "equal"
     * elements, making it difficult to compare test output on different
     * systems.
     */
     if(a1->start > a2->start) return -1;
     if(a1->start < a2->start) return 1;
     if(a1->length < a2->length) return -1;
     return 1;
}

/* Compare function for sorting primer records. */
static int
primer_pair_comp(x1, x2)
    const void *x1, *x2;
{
    const primer_pair *a1 = x1, *a2 = x2;
    int y1, y2;

    if(a1->pair_quality < a2->pair_quality) return -1;
    if (a1->pair_quality > a2->pair_quality) return 1;

    if (a1->compl_measure < a2->compl_measure) return -1;
    if (a1->compl_measure > a2->compl_measure) return 1;

    /* 
     * The following statements ensure that sorting
     * produces the same order on all systems regardless
     * of whether the sorting function is stable.
     */

    y1 = a1->left->start;
    y2 = a2->left->start;
    if (y1 > y2) return -1;  /* prefer left primers to the right. */
    if (y1 < y2) return 1;
    
    y1 = a1->right->start;
    y2 = a2->right->start;
    if (y1 < y2) return -1; /* prefer right primers to the left. */
    if (y1 > y2) return 1; 

    y1 = a1->left->length;
    y2 = a2->left->length;
    if (y1 < y2) return -1;  /* prefer shorter primers. */
    if (y1 > y2) return 1;
    
    y1 = a1->right->length;
    y2 = a2->right->length;
    if (y1 < y2) return -1; /* prefer shorter primers. */
    if (y1 > y2) return 1; 
    
    return 0;
}

/* 
 * Defines parameter values for given primer pair. Returns PAIR_OK if the pair is
 * acceptable; PAIR_FAILED otherwise.
 */
static int
pair_param(p3state, pa, sa, m, n, int_num, h, dpal_arg_to_use)
     primer3_state *p3state;
     const primer_args *pa;
     seq_args *sa;
     int m, n, int_num;
     primer_pair *h;
     const dpal_arg_holder *dpal_arg_to_use;
{
    char s1[MAX_PRIMER_LENGTH+1], s2[MAX_PRIMER_LENGTH+1], 
    s1_rev[MAX_PRIMER_LENGTH+1], s2_rev[MAX_PRIMER_LENGTH+1];
    short compl_end;

    /* FUTURE CODE: we must use the pair if the caller specifed
       both the left and the right primer. */
    int must_use = 0;

    int pair_failed_flag = 0;
    double min_oligo_tm;

    h->left = &p3state->f[m];
    h->right = &p3state->r[n];
    h->product_size = p3state->r[n].start - p3state->f[m].start+1;
    h->target = 0;
    h->compl_any = h->compl_end = 0;

    sa->pair_expl.considered++;

    if(h->product_size < pa->pr_min[int_num] || 
		h->product_size > pa->pr_max[int_num]) {
	sa->pair_expl.product++;
	h->product_size = -1;
	if (!must_use) return PAIR_FAILED;
	else pair_failed_flag = 1;
    }

    if (sa->num_targets > 0) {
	if (pair_spans_target(h, sa))
	    h->target = 1;
	else {
	    h->target = -1;
	    sa->pair_expl.target++;
	    if (!must_use) return PAIR_FAILED;
	    else pair_failed_flag = 1;
	}
    }

    /* ============================================================= */
    /* Compute product Tm and related parameters; check constraints. */

    h->product_tm 
     = long_seq_tm(sa->trimmed_seq, h->left->start,
		   h->right->start - h->left->start + 1, pa->salt_conc, pa->divalent_conc, pa->dntp_conc);
      
    PR_ASSERT(h->product_tm != OLIGOTM_ERROR);

    min_oligo_tm 
      = h->left->temp > h->right->temp ? h->right->temp : h->left->temp;
    h->product_tm_oligo_tm_diff = h->product_tm - min_oligo_tm;
    h->t_opt_a  = 0.3 * min_oligo_tm + 0.7 * h->product_tm - 14.9;

    if (pa->product_min_tm != PR_DEFAULT_PRODUCT_MIN_TM
	&& h->product_tm < pa->product_min_tm) {
      sa->pair_expl.low_tm++;
      if (!must_use) return PAIR_FAILED;
      else pair_failed_flag = 1;
    }

    if (pa->product_max_tm != PR_DEFAULT_PRODUCT_MAX_TM
	&& h->product_tm > pa->product_max_tm) {
      sa->pair_expl.high_tm++;
      if (!must_use) return PAIR_FAILED;
      else pair_failed_flag = 1;
    }
      
    h->diff_tm = fabs(p3state->f[m].temp - p3state->r[n].temp);
    if (h->diff_tm > pa->max_diff_tm) {
	sa->pair_expl.temp_diff++;
	if (!must_use) return PAIR_FAILED;
	else pair_failed_flag = 1;
    }

    /* End of product-temperature related computations. */
    /* ============================================================= */


    /* ============================================================= */
    /* Secondary structure and primer-dimer. */

    /* s1 is the forward oligo. */
    _pr_substr(sa->trimmed_seq,
	       p3state->f[m].start, 
	       p3state->f[m].length,
	       s1);

    /* s2 is the reverse oligo. */
    _pr_substr(sa->trimmed_seq,
	       p3state->r[n].start - p3state->r[n].length + 1,
	       p3state->r[n].length,
	       s2);

    if (p3state->f[m].self_any == ALIGN_SCORE_UNDEF) {
      /* We have not yet computed the  'self_any' paramter,
         which is an attempt at self primer-dimer and secondary
         structure. */
      oligo_compl(&p3state->f[m], pa, sa, OT_LEFT, dpal_arg_to_use);

      if (!OK_OR_MUST_USE(&p3state->f[m])) {
	sa->pair_expl.considered--;
	return PAIR_FAILED;
      }

    }

    if(p3state->r[n].self_any == ALIGN_SCORE_UNDEF){
      oligo_compl(&p3state->r[n], pa, sa, OT_RIGHT, 
		  dpal_arg_to_use);
       if (!OK_OR_MUST_USE(&p3state->r[n])) {
	  sa->pair_expl.considered--;
	  return PAIR_FAILED;
       }
    }

    /* End of secondary structure and primer-dimer. */
    /* ============================================================= */


    /* ============================================================= */
    /* Mispriming to template and mispriming to repeat libraries. */

    if (p3state->f[m].repeat_sim.score == NULL) {
      /* We have not yet checked the olgio against the repeat library. */
       oligo_mispriming(&p3state->f[m], pa, sa, OT_LEFT,
			dpal_arg_to_use->local_end,
			dpal_arg_to_use);
       if (!OK_OR_MUST_USE(&p3state->f[m])) {
	   sa->pair_expl.considered--;
	   return PAIR_FAILED;
       }
    }

    if(p3state->r[n].repeat_sim.score == NULL){
       oligo_mispriming(&p3state->r[n], pa, sa, OT_RIGHT,
			dpal_arg_to_use->local_end,
			dpal_arg_to_use);
       if (!OK_OR_MUST_USE(&p3state->r[n])) {
	  sa->pair_expl.considered--;
	  return PAIR_FAILED;
       }
    }
	
    /* 
     * Similarity between s1 and s2 is equivalent to complementarity between
     * s2's complement and s1.  (Both s1 and s2 are taken from the same strand.)
     */
    h->compl_any = align(s1,s2, dpal_arg_to_use->local);
    if (h->compl_any > pa->self_any) {
	sa->pair_expl.compl_any++;
	return PAIR_FAILED;
    }

    if ((h->compl_end = align(s1, s2, dpal_arg_to_use->end))
	> pa->self_end) {
	    sa->pair_expl.compl_end++;
	    return PAIR_FAILED;
    }

    /*
     * It is conceivable (though very unlikely in practice) that
     * align(s2_rev, s1_rev, end_args) > align(s1,s2,end_args).
     */
    _pr_reverse_complement(s1, s1_rev);
    _pr_reverse_complement(s2, s2_rev);

    if((compl_end = align(s2_rev, s1_rev, dpal_arg_to_use->end))
       > h->compl_end)  {
	if (compl_end > pa->self_end) {
	    sa->pair_expl.compl_end++;
	    return PAIR_FAILED;
	}
	h->compl_end = compl_end;
    }

    h->compl_measure = 	
	(h->right->self_end  + h->left->self_end + h->compl_end) * 1.1
	    + h->right->self_any + h->left->self_any + h->compl_any;

    if((h->repeat_sim = pair_repeat_sim(h, pa)) > pa->pair_repeat_compl){
	 sa->pair_expl.repeat_sim++;
	 return PAIR_FAILED;
    }

    if (!_pr_need_pair_template_mispriming(pa))  /* FIX ME -- this belongs above.... */
      h->template_mispriming = ALIGN_SCORE_UNDEF;
    else {
      PR_ASSERT(h->left->template_mispriming != ALIGN_SCORE_UNDEF);
      PR_ASSERT(h->left->template_mispriming_r != ALIGN_SCORE_UNDEF);
      PR_ASSERT(h->right->template_mispriming != ALIGN_SCORE_UNDEF);
      PR_ASSERT(h->right->template_mispriming_r != ALIGN_SCORE_UNDEF);
      h->template_mispriming =
	h->left->template_mispriming + h->right->template_mispriming_r;
      if ((h->left->template_mispriming_r + h->right->template_mispriming)
	  > h->template_mispriming)
      h->template_mispriming 
	= h->left->template_mispriming_r + h->right->template_mispriming;

      if (pa->pair_max_template_mispriming >= 0.0
	  && h->template_mispriming > pa->pair_max_template_mispriming) {
	sa->pair_expl.template_mispriming++;
	return PAIR_FAILED;
      }

    }
    return PAIR_OK;
}

void
compute_position_penalty(pa, sa, h, o_type)
  const primer_args *pa;
  const seq_args *sa;
  primer_rec *h;
  oligo_type o_type;
{
  int three_prime_base;
  int inside_flag = 0;
  int target_begin, target_end;

  PR_ASSERT(OT_LEFT == o_type || OT_RIGHT == o_type);
  PR_ASSERT(1 == sa->num_targets);
  target_begin = sa->tar[0][0];
  target_end = target_begin + sa->tar[0][1] - 1;

  three_prime_base = OT_LEFT == o_type
    ? h->start + h->length - 1 : h->start - h->length + 1;
  h->position_penalty_infinite = '\1';
  h->position_penalty = 0.0;
  
  if (OT_LEFT == o_type) {
    if (three_prime_base <= target_end) {
      h->position_penalty_infinite = '\0';
      if (three_prime_base < target_begin)
	h->position_penalty = target_begin - three_prime_base - 1;
      else {
	h->position_penalty = three_prime_base - target_begin + 1;
	inside_flag = 1;
      }
    }
  } else { /* OT_RIGHT == o_type */
    if (three_prime_base >= target_begin) {
      h->position_penalty_infinite = '\0';
      if (three_prime_base > target_end) {
	h->position_penalty = three_prime_base - target_end - 1;
      } else {
	h->position_penalty = target_end - three_prime_base + 1;
	inside_flag = 1;
      }
    }
  }
  if (!inside_flag)
    h->position_penalty *= pa->outside_penalty;
  else {
    h->position_penalty *= pa->inside_penalty;
  }
}

/* 
 * Return 1 if 'pair' spans any target (in sa->tar); otherwise return 0; An
 * oligo pair spans a target, t, if the last base of the left primer is to
 * left of the last base of t and the first base of the right primer is to
 * the right of the first base of t.  Of course the primers must
 * still be in a legal position with respect to each other.
 */
static int
pair_spans_target(pair, sa)
    const primer_pair *pair;
    const seq_args *sa;
{
    int i;
    int last_of_left = pair->left->start + pair->left->length - 1;
    int first_of_right = pair->right->start - pair->right->length + 1;
    int target_first, target_last;
    for (i = 0; i < sa->num_targets; i++) {
      target_first = sa->tar[i][0];
      target_last = target_first + sa->tar[i][1] - 1;
	if (last_of_left <= target_last
	    && first_of_right >= target_first
	    && last_of_left < first_of_right)
	    return 1;
    }
    return 0;
}

/* 
 * Return the value of the objective function value for given primer pair.
 * We must know that the pair is acceptable before calling obj_fn.
 */
static double
obj_fn(pa, h)
    const primer_args *pa;
    primer_pair *h;
{
    double sum;

    sum = 0.0;

    if(pa->pr_pair_weights.primer_quality)
       sum += pa->pr_pair_weights.primer_quality * (h->left->quality + h->right->quality);

    if(pa->pr_pair_weights.io_quality && 
        pa->primer_task == pick_pcr_primers_and_hyb_probe)
       sum += pa->pr_pair_weights.io_quality * h->intl->quality;

    if(pa->pr_pair_weights.diff_tm)
       sum += pa->pr_pair_weights.diff_tm * h->diff_tm;

    if(pa->pr_pair_weights.compl_any)
       sum += pa->pr_pair_weights.compl_any * h->compl_any / PR_ALIGN_SCORE_PRECISION;

    if(pa->pr_pair_weights.compl_end)
       sum += pa->pr_pair_weights.compl_end * h->compl_end / PR_ALIGN_SCORE_PRECISION;

    if(pa->pr_pair_weights.product_tm_lt && h->product_tm < pa->product_opt_tm)
	sum += pa->pr_pair_weights.product_tm_lt * 
			      (pa->product_opt_tm - h->product_tm);

    if(pa->pr_pair_weights.product_tm_gt && h->product_tm > pa->product_opt_tm)
	sum += pa->pr_pair_weights.product_tm_gt *
			       (h->product_tm - pa->product_opt_tm);

    if(pa->pr_pair_weights.product_size_lt &&
	    h->product_size < pa->product_opt_size) 
	    sum += pa->pr_pair_weights.product_size_lt * 
		 (pa->product_opt_size - h->product_size);

    if(pa->pr_pair_weights.product_size_gt &&
	    h->product_size > pa->product_opt_size)
	    sum += pa->pr_pair_weights.product_size_gt *
		  (h->product_size - pa->product_opt_size);

    if(pa->pr_pair_weights.repeat_sim)
      sum += pa->pr_pair_weights.repeat_sim * h->repeat_sim;

    if (pa->pr_pair_weights.template_mispriming) {
      PR_ASSERT(pa->pr_pair_weights.template_mispriming >= 0.0);
      PR_ASSERT(h->template_mispriming >= 0);
      sum += pa->pr_pair_weights.template_mispriming * h->template_mispriming;
    }

    PR_ASSERT(sum >= 0.0);

    return sum;
}

char *
pr_gather_warnings(sa, pa)
    const seq_args *sa;
    const primer_args *pa;
{
    pr_append_str warning;

    PR_ASSERT(NULL != sa);
    PR_ASSERT(NULL != pa);

    warning.data = NULL;
    warning.storage_size = 0;

    if (pa->repeat_lib.warning.data)
	pr_append_new_chunk(&warning, pa->repeat_lib.warning.data);

    if(pa->io_mishyb_library.warning.data != NULL) {
	pr_append_new_chunk(&warning, pa->io_mishyb_library.warning.data); 
	pr_append(&warning, " (for internal oligo)");
    }

    if (sa->warning.data) pr_append_new_chunk(&warning, sa->warning.data);
    return pr_is_empty(&warning) ? NULL : warning.data;
}

void
pr_append(x, s)
    pr_append_str *x;
    const char *s;
{
    int xlen, slen;
    if (NULL == x->data) {
	x->storage_size = 24;
	x->data = pr_safe_malloc(x->storage_size);
	*x->data = '\0';
    }
    xlen = strlen(x->data);
    slen = strlen(s);
    if (xlen + slen + 1 > x->storage_size) {
	x->storage_size += 2 * (slen + 1);
	x->data = pr_safe_realloc(x->data, x->storage_size);
    }
    strcpy(x->data + xlen, s);
}

void
pr_append_new_chunk(x, s)
    pr_append_str *x;
    const char *s;
{
  pr_append_w_sep(x, "; ", s);
}

void
pr_append_w_sep(x, sep, s)
    pr_append_str *x;
    const char *sep;
    const char *s;
{
    if (pr_is_empty(x))
	pr_append(x, s);
    else {
	pr_append(x, sep);
	pr_append(x, s);
    }
}

void
pr_set_empty(x)
    pr_append_str *x;
{
    PR_ASSERT(NULL != x);
    if (NULL != x->data) *x->data = '\0';
}

int
pr_is_empty(x)
    const pr_append_str *x;
{
    PR_ASSERT(NULL != x);
    return  NULL == x->data || '\0' == *x->data;
}

static short
align(s1, s2, a)
    const char *s1, *s2;
    const dpal_args *a;
{
    dpal_results r;

    if(a->flag == DPAL_LOCAL || a->flag == DPAL_LOCAL_END) {
      if (strlen(s2) < 3) {
	/* For extremely short alignments we simply
	   max out the score, because the dpal subroutines
           for these cannot handle this case. 
           FIX: this can probably be corrected in dpal. */
	return (short) (100 * strlen(s2));
      }
    }
    dpal(s1, s2, a, &r);
    PR_ASSERT(r.score <= SHRT_MAX);
    if (r.score == DPAL_ERROR_SCORE) {
      /* There was an error. */
      if (errno == ENOMEM) {
	OOM_ERROR;
      } else {
	fprintf(stderr, r.msg);
	/* Fix this later, when error handling
	   in "primer_choice" is updated to
	   no longer exit. */
	PR_ASSERT(r.score != DPAL_ERROR_SCORE);
      }
    }
    return ((r.score<0) ? 0 : (short)r.score);
}

char *
pr_oligo_sequence(sa, o)
    const seq_args *sa;
    const primer_rec *o;
{
    static char s[MAX_PRIMER_LENGTH+1];
    int seq_len;
    PR_ASSERT(NULL != sa);
    PR_ASSERT(NULL != o);
    seq_len = strlen(sa->sequence);
    PR_ASSERT(o->start + sa->incl_s >= 0);
    PR_ASSERT(o->start + sa->incl_s + o->length <= seq_len);
    _pr_substr(sa->sequence, sa->incl_s + o->start, o->length, s);
    return &s[0];
}

char *
pr_oligo_rev_c_sequence(sa, o)
    const seq_args *sa;
    const primer_rec *o;
{
    static char s[MAX_PRIMER_LENGTH+1], s1[MAX_PRIMER_LENGTH+1];
    int seq_len, start;
    PR_ASSERT(NULL != sa);
    PR_ASSERT(NULL != o);
    seq_len = strlen(sa->sequence);
    start = sa->incl_s + o->start - o->length + 1;
    PR_ASSERT(start >= 0);
    PR_ASSERT(start + o->length <= seq_len);
    _pr_substr(sa->sequence, start, o->length, s);
    _pr_reverse_complement(s,s1);
    return &s1[0];
}

/* Calculate self complementarity. */
static void
oligo_compl(h, ha, sa, l, dpal_arg_to_use)
    primer_rec *h;
    const primer_args *ha;
    seq_args *sa;
    oligo_type l;
    const dpal_arg_holder *dpal_arg_to_use;
{
    char s[MAX_PRIMER_LENGTH+1], s1[MAX_PRIMER_LENGTH+1];
    int j;
    short self_any, self_end;

    PR_ASSERT(h != NULL);

    if (OT_INTL == l) {
      self_any = ha->io_self_any;
      self_end = ha->io_self_end;
    } else {
      self_any = ha->self_any;
      self_end = ha->self_end;
    }

    j =  (OT_LEFT == l || OT_INTL == l) ? h->start : h->start-h->length+1;

    _pr_substr(sa->trimmed_seq, j, h->length, s1);
    _pr_reverse_complement(s1, s);

    h->self_any = align(s1, s, dpal_arg_to_use->local);
    if(h->self_any > self_any){
	h->ok = OV_SELF_ANY;
	if      (OT_LEFT  == l) {
	     sa->left_expl.compl_any++;
	     sa->left_expl.ok--;
        }
	else if (OT_RIGHT == l) {
	     sa->right_expl.compl_any++;
	     sa->right_expl.ok--;
        }
	else {
	     sa->intl_expl.compl_any++;
	     sa->intl_expl.ok--;
        }
	if (!h->must_use) return;
    }

    h->self_end = (l != OT_RIGHT) 
      ? align(s1, s, dpal_arg_to_use->end)
      : align(s, s1, dpal_arg_to_use->end);
    if(h->self_end > self_end) {
	h->ok = OV_SELF_END;
	if      (OT_LEFT  == l) {
	    sa->left_expl.compl_end++;
	    sa->left_expl.ok--;
        }
	else if (OT_RIGHT == l) {
	    sa->right_expl.compl_end++;
	    sa->right_expl.ok--;
        }
	else {
	    sa->intl_expl.compl_end++;
	    sa->intl_expl.ok--;
        }
	return;
    }
}

static void 
primer_mispriming_to_template(primer_rec *h,
			      const primer_args *pa,
			      seq_args *sa,
			      oligo_type l,
			      int first,
			      int last, 
			      /* The oligo sequence: */
			      const char *s, 
			      /* s reverse complemented: */
			      const char *s_r,
			      const dpal_args *align_args
			      ) {
  const char *oseq;
  char *target, *target_r;
  int tmp, seqlen;
  int debug = 0;
  int first_untrimmed, last_untrimmed;  
                  /* Indexes of first and last bases of the oligo in sa->seq,
		     that is, WITHIN THE TOTAL SEQUENCE INPUT. */

  /* first, last are indexes of first and last bases of the oligo in
     sa->trimmed_seq, that is, WITHIN THE INCLUDED REGION. */

  char   tmp_char;
  short  tmp_score;

  seqlen = strlen(sa->upcased_seq);
  first_untrimmed = sa->incl_s + first;
  last_untrimmed = sa->incl_s + last;

  if (l == OT_LEFT) {
    oseq = &s[0];
    target = &sa->upcased_seq[0];
    target_r = &sa->upcased_seq_r[0];
  } else {  /* l == OT_RIGHT */
    if (debug) 
      fprintf(stderr, "first_untrimmed = %d, last_untrimmed = %d\n",
	      first_untrimmed, last_untrimmed);
    oseq = &s_r[0];
    target = &sa->upcased_seq_r[0];
    target_r = &sa->upcased_seq[0];
    /* We need to adjust first_untrimmed and last_untrimmed so that
       they are correct in the reverse-complemented
       sequence.
    */
    tmp = (seqlen - last_untrimmed) - 1;
    last_untrimmed  = (seqlen - first_untrimmed) - 1;
    first_untrimmed = tmp;
  }

  /* 1. Align to the template 5' of the oligo. */
  tmp_char = target[first_untrimmed];
  target[first_untrimmed] = '\0';

  tmp_score = align(oseq, target, align_args);

  if (debug) {
    if (l == OT_LEFT) fprintf(stderr, "\n************ OLIGO = LEFT\n");
    else fprintf(stderr,              "\n************ OLIGO = RIGHT\n");
    fprintf(stderr, "first_untrimmed = %d, last_untrimmed = %d\n",
	    first_untrimmed, last_untrimmed);
			
    fprintf(stderr, "5' of oligo: Score %d aligning %s against %s\n\n", tmp_score,
	    oseq, target);
  }

  target[first_untrimmed] = tmp_char;

  /* 2. Align to the template 3' of the oligo. */
  h->template_mispriming
    = align(oseq, &target[0] + last_untrimmed + 1, align_args);

  if (debug)
    fprintf(stderr, "3' of oligo Score %d aligning %s against %s\n\n",
	    h->template_mispriming, oseq, &target[0] + last_untrimmed + 1);

  /* 3. Take the max of 1. and 2. */
  if (tmp_score > h->template_mispriming)
    h->template_mispriming = tmp_score;

  /* 4. Align to the reverse strand of the template. */
  h->template_mispriming_r
    = align(oseq, target_r, align_args);

  if (debug)
    fprintf(stderr, "other strand Score %d aligning %s against %s\n\n", 
	    h->template_mispriming_r, oseq, target_r);

  if (pa->max_template_mispriming >= 0 
      && oligo_max_template_mispriming(h) > pa->max_template_mispriming) {
    h->ok = OV_TEMPLATE_MISPRIMING;
    if (OT_LEFT == l) {
      sa->left_expl.template_mispriming++;
      sa->left_expl.ok--;
    } else if (OT_RIGHT == l) {
      sa->right_expl.template_mispriming++;
      sa->right_expl.ok--;
    } else PR_ASSERT(0); /* Should not get here. */
  }
}

static void  /* FIX ME, split this into two, slot in... 
		move primer update outside... */
primer_mispriming_to_library(h, pa, sa, l, align_args,  dpal_arg_to_use)
   primer_rec *h;
   const primer_args *pa;
   seq_args *sa;
   oligo_type l;
   const dpal_args *align_args;
   const dpal_arg_holder *dpal_arg_to_use;
{
  char 
    s[MAX_PRIMER_LENGTH+1],     /* Will contain the oligo sequence. */
    s_tmp[MAX_PRIMER_LENGTH+1], /* Scratch buffer. */
    s_r[MAX_PRIMER_LENGTH+1];   /* Will contain s reverse complemented. */

  double w;
  const seq_lib *lib;
  int i;
  int first, last; /* Indexes of first and last bases of the oligo in sa->trimmed_seq,
		     that is, WITHIN THE INCLUDED REGION. */
  int min, max;
  int match_length;
  short  lib_compl;

  if (OT_INTL == l) {
    lib = &(pa->io_mishyb_library);
    lib_compl = pa->io_repeat_compl;
  } else {
    lib = &(pa->repeat_lib);
    lib_compl = pa->repeat_compl;
  }

  first =  (OT_LEFT == l || OT_INTL == l)
    ? h->start 
    : h->start - h->length + 1;
  last  =  (OT_LEFT == l || OT_INTL == l)
    ? h->start + h->length - 1
    : h->start;

  match_length = h->length;

  _pr_substr(sa->trimmed_seq, first, h->length, s_tmp);
  _pr_substr(s_tmp, 0, match_length, s);
  _pr_reverse_complement(s, s_tmp);  /* FIX ME -- is s_tmp needed? */
  _pr_substr(s_tmp, 0, match_length, s_r);

  /*
   * Calculate maximum similarity to sequences from user defined repeat
   * library. Compare it with maximum allowed repeat similarity.
   */

  if(lib->seq_num > 0) {
    h->repeat_sim.score = 
      pr_safe_malloc(lib->seq_num * sizeof(short));
    h->repeat_sim.max = h->repeat_sim.min = 0;
    max = min = 0;
    h->repeat_sim.name = lib->names[0];
    for(i = 0; i < lib->seq_num; i++){
      if (OT_LEFT == l)
	w = lib->weight[i] *
	  align(s, lib->seqs[i], 
		(pa->lib_ambiguity_codes_consensus
		 ? dpal_arg_to_use->local_end_ambig
		 : dpal_arg_to_use->local_end));

      else if (OT_INTL == l)
	w = lib->weight[i] *
	  align(s, lib->seqs[i], 
		(pa->lib_ambiguity_codes_consensus
		 ? dpal_arg_to_use->local_ambig
		 : dpal_arg_to_use->local));

      else 
	w = lib->weight[i] *
	  align(s_r, lib->rev_compl_seqs[i], 
		(pa->lib_ambiguity_codes_consensus
		 ? dpal_arg_to_use->local_end_ambig
		 : dpal_arg_to_use->local));

      h->repeat_sim.score[i] = w;
      if(w > max){
	max = w;
	h->repeat_sim.max = i;
	h->repeat_sim.name = lib->names[i];
      }
      if(w < min){
	min = w;
	h->repeat_sim.min = i;
      }
      if (w > lib_compl) {
	h->ok = OV_LIB_SIM;
	if (OT_LEFT  == l) {
	  sa->left_expl.repeat_score++;
	  sa->left_expl.ok--;
	}
	else if (OT_RIGHT == l) {
	  sa->right_expl.repeat_score++;
	  sa->right_expl.ok--;
	}
	else {
	  sa->intl_expl.repeat_score++;
	  sa->intl_expl.ok--;
	}
	if (!h->must_use) return;
      }
    }
  }
}


static void 
oligo_mispriming(h, pa, sa, l, align_args,  dpal_arg_to_use)
   primer_rec *h;
   const primer_args *pa;
   seq_args *sa;
   oligo_type l;
   const dpal_args *align_args;
   const dpal_arg_holder *dpal_arg_to_use;
{
  char 
    s[MAX_PRIMER_LENGTH+1],     /* Will contain the oligo sequence. */
    s_tmp[MAX_PRIMER_LENGTH+1], /* Scratch buffer. */
    s_r[MAX_PRIMER_LENGTH+1];   /* Will contain s reverse complemented. */

  double w;
  const seq_lib *lib;
  int i;
  int first, last; /* Indexes of first and last bases of the oligo in sa->trimmed_seq,
		     that is, WITHIN THE INCLUDED REGION. */
  int min, max;
  int match_length;
  short  lib_compl;

  if (OT_INTL == l) {
    lib = &(pa->io_mishyb_library);
    lib_compl = pa->io_repeat_compl;
  } else {
    lib = &(pa->repeat_lib);
    lib_compl = pa->repeat_compl;
  }

  first =  (OT_LEFT == l || OT_INTL == l)
    ? h->start 
    : h->start - h->length + 1;
  last  =  (OT_LEFT == l || OT_INTL == l)
    ? h->start + h->length - 1
    : h->start;

  match_length = h->length;

  _pr_substr(sa->trimmed_seq, first, h->length, s_tmp);
  _pr_substr(s_tmp, 0, match_length, s);
  _pr_reverse_complement(s, s_tmp);  /* FIX ME -- is s_tmp needed? */
  _pr_substr(s_tmp, 0, match_length, s_r);

  /*
   * Calculate maximum similarity to sequences from user defined repeat
   * library. Compare it with maximum allowed repeat similarity.
   */

  if(lib->seq_num > 0) {
    h->repeat_sim.score = 
      pr_safe_malloc(lib->seq_num * sizeof(short));
    h->repeat_sim.max = h->repeat_sim.min = 0;
    max = min = 0;
    h->repeat_sim.name = lib->names[0];
    for(i = 0; i < lib->seq_num; i++){
      if (OT_LEFT == l)
	w = lib->weight[i] *
	  align(s, lib->seqs[i], 
		(pa->lib_ambiguity_codes_consensus
		 ? dpal_arg_to_use->local_end_ambig
		 : dpal_arg_to_use->local_end));

      else if (OT_INTL == l)
	w = lib->weight[i] *
	  align(s, lib->seqs[i], 
		(pa->lib_ambiguity_codes_consensus
		 ? dpal_arg_to_use->local_ambig
		 : dpal_arg_to_use->local));

      else 
	w = lib->weight[i] *
	  align(s_r, lib->rev_compl_seqs[i], 
		(pa->lib_ambiguity_codes_consensus
		 ? dpal_arg_to_use->local_end_ambig
		 : dpal_arg_to_use->local));

      h->repeat_sim.score[i] = w;
      if(w > max){
	max = w;
	h->repeat_sim.max = i;
	h->repeat_sim.name = lib->names[i];
      }
      if(w < min){
	min = w;
	h->repeat_sim.min = i;
      }
      if (w > lib_compl) {
	h->ok = OV_LIB_SIM;
	if (OT_LEFT  == l) {
	  sa->left_expl.repeat_score++;
	  sa->left_expl.ok--;
	}
	else if (OT_RIGHT == l) {
	  sa->right_expl.repeat_score++;
	  sa->right_expl.ok--;
	}
	else {
	  sa->intl_expl.repeat_score++;
	  sa->intl_expl.ok--;
	}
	if (!h->must_use) return;
      }
    }
  }

  if (_pr_need_template_mispriming(pa) && (l == OT_RIGHT || l == OT_LEFT)) {
    /* Calculate maximum similarity to ectopic sites in the template. */
    primer_mispriming_to_template(h, pa, sa, l, first, last, s, s_r, align_args);

  }
}

static int
pair_repeat_sim(h, pa)
  primer_pair *h;
  const primer_args *pa;
{
  int i, n, max, w;
  primer_rec *fw, *rev;
     
  fw = h->left;
  rev = h->right;

  max = 0;
  n = pa->repeat_lib.seq_num;
  if(n == 0) return 0;
  h->rep_name =  pa->repeat_lib.names[0] ;
  for(i = 0; i < n; i++) {
    if((w=(fw->repeat_sim.score[i] +
	   rev->repeat_sim.score[i])) > max) {
      max = w;
      h->rep_name =  pa->repeat_lib.names[i] ;
    }
  }
  return max;
}

/* 
 * 's' is the sequence in which to find the stop codon.
 * 'start' is the position of a start codon.
 * 
 * There are two modes depending on 'direction'.
 *
 * If direction is 1:
 *
 * Return the index of the first stop codon to the right of
 * 'start' in 's' (in same frame).
 * If there is no such stop codon return -1.
 *
 * If direction is -1:
 *
 * If 'start' is negative then return -1.
 * Otherwise return the index the first stop codon to left of 'start'.
 * If there is no such stop codon return -1.
 *  
 * Note: we don't insist that the start codon be ATG, since in some
 * cases the caller will not have the full sequence in 's', nor even
 * know the postion of the start codon relative to s.
 */
int
find_stop_codon(s, start, direction)
  const char* s;
{
  const char *p, *q;
  int increment = 3 * direction;
  int len = strlen(s);

  PR_ASSERT(s != NULL);
  PR_ASSERT(direction == 1 || direction == -1);
  PR_ASSERT(len >= 3);
  PR_ASSERT(start <= (len - 3));

  if (start < 0) {
    if (direction == 1)
      while (start < 0) start += increment;
    else
      return -1;
  }

  for (p = &s[start];
       p >= &s[0]
	 && *p
	 && *(p + 1)
	 && *(p + 2);
       p += increment) {
    if ('T' != *p && 't' != *p) continue;
    q = p + 1;
    if ('A' == *q || 'a' == *q) {
      q++;
      if  ('G' == *q || 'g' == *q || 'A' == *q || 'a' == *q)
	return p - &s[0];
    } else if ('G' == *q || 'g' == *q) {
      q++;
      if ('A' == *q || 'a' == *q)
	return p - &s[0];
    }
  }
  return -1;
}

int
strcmp_nocase(s1, s2)
char *s1, *s2;
{
   static char M[UCHAR_MAX];
   static int f = 0;
   int i;
   char *p, *q;

   if(f != 1){
      for(i = 0; i < UCHAR_MAX; i++) M[i] = i;
      i = 'a'; M[i] = 'A'; i = 'b'; M[i] = 'B'; i = 'c'; M[i] = 'C';
      i = 'A'; M[i] = 'a'; i = 'B'; M[i] = 'b'; i = 'C'; M[i] = 'c';
      i = 'd'; M[i] = 'D'; i = 'e'; M[i] = 'E'; i = 'f'; M[i] = 'F';
      i = 'D'; M[i] = 'd'; i = 'E'; M[i] = 'e'; i = 'F'; M[i] = 'f';
      i = 'g'; M[i] = 'G'; i = 'h'; M[i] = 'H'; i = 'i'; M[i] = 'I';
      i = 'G'; M[i] = 'g'; i = 'H'; M[i] = 'h'; i = 'I'; M[i] = 'i';
      i = 'k'; M[i] = 'K'; i = 'l'; M[i] = 'L'; i = 'm'; M[i] = 'M';
      i = 'K'; M[i] = 'k'; i = 'L'; M[i] = 'l'; i = 'M'; M[i] = 'm';
      i = 'n'; M[i] = 'N'; i = 'o'; M[i] = 'O'; i = 'p'; M[i] = 'P';
      i = 'N'; M[i] = 'n'; i = 'O'; M[i] = 'o'; i = 'P'; M[i] = 'p';
      i = 'q'; M[i] = 'Q'; i = 'r'; M[i] = 'R'; i = 's'; M[i] = 'S';
      i = 'Q'; M[i] = 'q'; i = 'R'; M[i] = 'r'; i = 'S'; M[i] = 's';
      i = 't'; M[i] = 'T'; i = 'u'; M[i] = 'U'; i = 'v'; M[i] = 'V';
      i = 'T'; M[i] = 't'; i = 'U'; M[i] = 'u'; i = 'V'; M[i] = 'v';
      i = 'w'; M[i] = 'W'; i = 'x'; M[i] = 'X'; i = 'y'; M[i] = 'Y';
      i = 'W'; M[i] = 'w'; i = 'X'; M[i] = 'x'; i = 'Y'; M[i] = 'y';
      i = 'z'; M[i] = 'Z'; i = 'Z'; M[i] = 'z'; i = 'j'; M[i] = 'J';
      i = 'J'; M[i] = 'j';
      f = 1;
   }

   if(s1 == NULL || s2 == NULL) return 1;
   if(strlen(s1) != strlen(s2)) return 1;
   p = s1; q = s2;
   while(*p != '\0' && *p != '\n' && *q != '\0' && *q != '\n'){
      i = *p;
      if(*p == *q || M[i] == *q ) {p++; q++; continue;}

      return 1;
   }
   return 0;
}

static void
free_repeat_sim_score(state)
     primer3_state *state;
{
   int i;

   for (i = 0; i < state->n_f; i++) {
       if (state->f[i].repeat_sim.score != NULL) {
	   free(state->f[i].repeat_sim.score);
	   state->f[i].repeat_sim.score = NULL;
       }
   }

   for (i = 0; i < state->n_r; i++) {
       if (state->r[i].repeat_sim.score != NULL) {
	   free(state->r[i].repeat_sim.score);
	   state->r[i].repeat_sim.score = NULL;
       }
   }

   for (i = 0; i < state->n_m; i++) {
       if (state->mid[i].repeat_sim.score != NULL) {
	   free(state->mid[i].repeat_sim.score);
	   state->mid[i].repeat_sim.score = NULL;
       }
   }
}

/*  Edited by T. Koressaar for lowercase masking. This function checks
 if the 3' end of the primer has been masked by lowercase letter.
 Function created/Added by Eric Reppo, July 9, 2002
 */
static void
check_if_lowercase_masked(position, sequence, h)
     const int position;
     const char *sequence;
     primer_rec *h;
{   
   const char* p = &sequence[position];
   if ('a' == *p || 'c' == *p ||'g' == *p || 't' == *p) {
      h->ok=OV_GMASKED;
   }
}

/*
 * Return 1 on error, 0 on success.  Set sa->trimmed_seq and possibly modify
 * sa->tar.  Upcase and check all bases in sa->trimmed_seq
 */
int
_pr_data_control(pa, sa)
    primer_args *pa;
    seq_args *sa;
{
    static char s1[MAX_PRIMER_LENGTH+1];
    int i, pr_min;
    int seq_len = strlen(sa->sequence);
    char offending_char = '\0';

    if (pa->io_max_template_mishyb >= 0)
      pr_append_new_chunk(&pa->glob_err,
			  "PRIMER_INTERNAL_OLIGO_MAX_TEMPLATE_MISHYB is not supported");

    if (pa->primer_min_size < 1)
      pr_append_new_chunk(&pa->glob_err, "PRIMER_MIN_SIZE must be >= 1");

    if (pa->primer_max_size > MAX_PRIMER_LENGTH) {
      pr_append_new_chunk(&pa->glob_err,
			  "PRIMER_MAX_SIZE exceeds built-in maximum of ");
      pr_append(&pa->glob_err, MACRO_VALUE_AS_STRING(MAX_PRIMER_LENGTH));
      return 1;
    }

    if (pa->primer_opt_size > pa->primer_max_size) {
	pr_append_new_chunk(&pa->glob_err,
			    "PRIMER_{OPT,DEFAULT}_SIZE > PRIMER_MAX_SIZE");
	return 1;
    }

    if (pa->primer_opt_size < pa->primer_min_size) {
	pr_append_new_chunk(&pa->glob_err,
			    "PRIMER_{OPT,DEFAULT}_SIZE < PRIMER_MIN_SIZE");
	return 1;
    }

    if (pa->io_primer_max_size > MAX_PRIMER_LENGTH) {
	pr_append_new_chunk(&pa->glob_err,
		  "PRIMER_INTERNAL_OLIGO_MAX_SIZE exceeds built-in maximum");
        return 1;
    }

    if (pa->io_primer_opt_size > pa->io_primer_max_size) {
	pr_append_new_chunk(&pa->glob_err,
		  "PRIMER_INTERNAL_OLIGO_{OPT,DEFAULT}_SIZE > MAX_SIZE");
        return 1;
    }

    if (pa->io_primer_opt_size < pa->io_primer_min_size) {
	pr_append_new_chunk(&pa->glob_err,
		  "PRIMER_INTERNAL_OLIGO_{OPT,DEFAULT}_SIZE < MIN_SIZE");
        return 1;
    }

    if (pa->gc_clamp > pa->primer_min_size) {
	pr_append_new_chunk(&pa->glob_err,
			    "PRIMER_GC_CLAMP > PRIMER_MIN_SIZE");
	return 1;
    }

    if (NULL == sa->sequence_name && pa->file_flag) {
	pr_append_new_chunk(&sa->error,
			    "Need PRIMER_SEQUENCE_ID if PRIMER_FILE_FLAG != 0");
	return 1;
    }

    if (0 == pa->num_intervals) {
	pr_append_new_chunk(&pa->glob_err,
			    "Empty value for PRIMER_PRODUCT_SIZE_RANGE");
	return 1;
    }
    for (i = 0; i < pa->num_intervals; i++) {
	if (pa->pr_min[i] > pa->pr_max[i] || pa->pr_min[i] < 0) {
	    pr_append_new_chunk(&pa->glob_err,
				"Illegal element in PRIMER_PRODUCT_SIZE_RANGE");
	    return 1;
	}
    }

    pr_min = INT_MAX;
    for(i=0;i<pa->num_intervals;i++)
	if(pa->pr_min[i]<pr_min) pr_min=pa->pr_min[i];

    if (pa->primer_max_size > pr_min) {
	pr_append_new_chunk(&pa->glob_err,
			    "PRIMER_MAX_SIZE > min PRIMER_PRODUCT_SIZE_RANGE");
	return 1;
    }

    if ((pick_pcr_primers_and_hyb_probe == pa->primer_task 
	 || pick_hyb_probe_only == pa->primer_task)
	&& pa->io_primer_max_size > pr_min) {
	pr_append_new_chunk(&pa->glob_err,
		 "PRIMER_INTERNAL_OLIGO_MAX_SIZE > min PRIMER_PRODUCT_SIZE_RANGE");
        return 1;
    }

    if (pa->num_return < 1) {
	pr_append_new_chunk(&pa->glob_err,
			    "PRIMER_NUM_RETURN < 1");
        return 1;
    }

    if (sa->incl_l >= INT_MAX) {
	pr_append_new_chunk(&sa->error, "Value for INCLUDED_REGION too large");
	return 1;
    }

    if (sa->incl_s < 0 || sa->incl_l < 0 
	|| sa->incl_s + sa->incl_l > seq_len) {
	pr_append_new_chunk(&sa->error, "Illegal value for INCLUDED_REGION");
	return 1;
    }
    
    if (sa->incl_l < pr_min && pa->primer_task != pick_hyb_probe_only
	&& pa->primer_task != pick_left_only
	&& pa->primer_task != pick_right_only) {
	pr_append_new_chunk(&sa->error,
	   "INCLUDED_REGION length < min PRIMER_PRODUCT_SIZE_RANGE");
	return 1;
    }

    if (pa->max_end_stability < 0) {
        pr_append_new_chunk(&sa->error,
			    "PRIMER_MAX_END_STABILITY must be non-negative");
	return 1;
    }

    if (!PR_START_CODON_POS_IS_NULL(sa)) {
      if (!PR_POSITION_PENALTY_IS_NULL(pa)) {
	pr_append_new_chunk(&sa->error,
	   "Cannot accept both PRIMER_START_CODON_POSITION and non-default ");
	pr_append(&sa->error,
	   "arguments for PRIMER_INSIDE_PENALTY or PRIMER_OUTSIDE_PENALTY");
      }
      if (sa->start_codon_pos  > (sa->incl_s + sa->incl_l - 3))
	pr_append_new_chunk(&sa->error,
	   "Start codon position not contained in INCLUDED_REGION");
      else {
	if (sa->start_codon_pos >= 0
	    && ((sa->sequence[sa->start_codon_pos] != 'A'
		 && sa->sequence[sa->start_codon_pos] != 'a')
		|| (sa->sequence[sa->start_codon_pos + 1] != 'T'
		    && sa->sequence[sa->start_codon_pos + 1] != 't')
		|| (sa->sequence[sa->start_codon_pos + 2] != 'G'
		    && sa->sequence[sa->start_codon_pos + 2] != 'g')))
	  pr_append_new_chunk(&sa->error,
			      "No start codon at PRIMER_START_CODON_POSITION");
      }
    }

    sa->trimmed_seq = pr_safe_malloc(sa->incl_l + 1);
    _pr_substr(sa->sequence, sa->incl_s, sa->incl_l, sa->trimmed_seq);
   
    /* edited by T. Koressaar for lowercase masking */
    sa->trimmed_orig_seq = pr_safe_malloc(sa->incl_l + 1);
    _pr_substr(sa->sequence, sa->incl_s, sa->incl_l, sa->trimmed_orig_seq);
   
    sa->upcased_seq = pr_safe_malloc(strlen(sa->sequence) + 1);
    strcpy(sa->upcased_seq, sa->sequence);
    if ((offending_char = dna_to_upper(sa->upcased_seq, 1))) {
      offending_char = '\0';
      /* TODO add warning or error (depending on liberal base)
         here. */
    }
    sa->upcased_seq_r = pr_safe_malloc(strlen(sa->sequence) + 1);
    _pr_reverse_complement(sa->upcased_seq, sa->upcased_seq_r);

    if (check_intervals("TARGET", sa->num_targets, sa->tar, seq_len, sa)
	== 1) return 1;
    sa->start_codon_pos -= sa->incl_s;

    if (check_intervals("EXCLUDED_REGION", sa->num_excl, sa->excl,
			seq_len, sa)
	== 1) return 1;

    if (check_intervals("PRIMER_INTERNAL_OLIGO_EXCLUDED_REGION",
			sa->num_internal_excl, sa->excl_internal,
			seq_len, sa)
	== 1) return 1;

    if (NULL != sa->quality){
	if(pa->min_quality != 0 && pa->min_quality < pa->quality_range_min) {
	   pr_append_new_chunk(&pa->glob_err,
	       "PRIMER_MIN_QUALITY < PRIMER_QUALITY_RANGE_MIN");
           return 1;
        }
	if(pa->min_quality != 0 && pa->min_quality > pa->quality_range_max) {
	   pr_append_new_chunk(&pa->glob_err,
	       "PRIMER_MIN_QUALITY > PRIMER_QUALITY_RANGE_MAX");
           return 1;
        }
	if(pa->io_min_quality != 0 && pa->io_min_quality <pa->quality_range_min) {
	   pr_append_new_chunk(&pa->glob_err,
	    "PRIMER_INTERNAL_OLIGO_MIN_QUALITY < PRIMER_QUALITY_RANGE_MIN");
           return 1;
        }
	if(pa->io_min_quality != 0 && pa->io_min_quality > pa->quality_range_max) {
	   pr_append_new_chunk(&pa->glob_err,
	     "PRIMER_INTERNAL_OLIGO_MIN_QUALITY > PRIMER_QUALITY_RANGE_MAX");
           return 1;
        }
	for(i=0; i< seq_len; i++) {
	   if(sa->quality[i] < pa->quality_range_min ||
		 sa->quality[i] > pa->quality_range_max) {
             pr_append_new_chunk(&sa->error,
		"Sequence quality score out of range");
             return 1;
           }
        }
    }
    else if (pa->primer_weights.seq_quality || pa->io_weights.seq_quality) {
	 pr_append_new_chunk(&sa->error,
	      "Sequence quality is part of objective function but sequence quality is not defined");
         return 1;
    }

    if ((offending_char = dna_to_upper(sa->trimmed_seq, 0))) {
      if (pa->liberal_base) {
	pr_append_new_chunk(&sa->warning,
			    "Unrecognized base in input sequence");
      }
      else {
	pr_append_new_chunk(&sa->error, "Unrecognized base in input sequence");
	return 1;
      }
    }

    if(pa->opt_tm < pa->min_tm || pa->opt_tm > pa->max_tm) {
	 pr_append_new_chunk(&pa->glob_err,
			     "Optimum primer Tm lower than minimum or higher than maximum");
	 return 1;
    }
    if(pa->io_opt_tm < pa->io_min_tm || pa->io_opt_tm > pa->io_max_tm) {
	 pr_append_new_chunk(&pa->glob_err,
			     "Illegal values for PRIMER_INTERNAL_OLIGO_TM");
	 return 1;
    }
    if(pa->min_gc>pa->max_gc||pa->min_gc>100||pa->max_gc<0){
	 pr_append_new_chunk(&pa->glob_err,
			     "Illegal value for PRIMER_MAX_GC and PRIMER_MIN_GC");
	 return 1;
    }
    if(pa->io_min_gc>pa->io_max_gc||pa->io_min_gc>100||pa->io_max_gc<0){
	 pr_append_new_chunk(&pa->glob_err,
			     "Illegal value for PRIMER_INTERNAL_OLIGO_GC");
	 return 1;
    }
    if(pa->num_ns_accepted<0){
	 pr_append_new_chunk(&pa->glob_err,
			     "Illegal value for PRIMER_NUM_NS_ACCEPTED");
	 return 1;
    }
    if(pa->io_num_ns_accepted<0){ 
	 pr_append_new_chunk(&pa->glob_err,
			     "Illegal value for PRIMER_INTERNAL_OLIGO_NUM_NS");
	 return 1;
    }
    if(pa->self_any<0||pa->self_end<0
		       ||pa->pair_compl_any<0||pa->pair_compl_end<0){
         pr_append_new_chunk(&pa->glob_err,
	     "Illegal value for primer complementarity restrictions");
	 return 1;
    }
    if(pa->io_self_any<0||pa->io_self_end<0){
	  pr_append_new_chunk(&pa->glob_err,
	      "Illegal value for internal oligo complementarity restrictions");
	  return 1;
    }
    if(pa->salt_conc<=0||pa->dna_conc<=0){
	  pr_append_new_chunk(&pa->glob_err,
	      "Illegal value for primer salt or dna concentration");
	  return 1;
    }
   if((pa->dntp_conc<0 && pa->divalent_conc!=0)||pa->divalent_conc<0){ /* added by T.Koressaar */
      pr_append_new_chunk(&pa->glob_err, "Illegal value for primer divalent salt or dNTP concentration");
      return 1;
   }
   
    if(pa->io_salt_conc<=0||pa->io_dna_conc<=0){ 
	  pr_append_new_chunk(&pa->glob_err,
	      "Illegal value for internal oligo salt or dna concentration");
	  return 1;
    }
   if((pa->io_dntp_conc<0 && pa->io_divalent_conc!=0)||pa->io_divalent_conc<0) { /* added by T.Koressaar */
      pr_append_new_chunk(&pa->glob_err,
			  "Illegal value for internal oligo divalent salt or dNTP concentration");
      return 1;
   }
   
    if (!_PR_DEFAULT_POSITION_PENALTIES(pa) && sa->num_targets > 1) {
      pr_append_new_chunk(&sa->error,
			  "Non-default inside penalty or outside penalty ");
      pr_append(&sa->error,
		"is valid only when number of targets <= 1");
    }
    if (!_PR_DEFAULT_POSITION_PENALTIES(pa) && 0 == sa->num_targets) {
      pr_append_new_chunk(&sa->warning,
			  "Non-default inside penalty or outside penalty ");
      pr_append(&sa->warning,
		"has no effect when number of targets is 0");
    }
    if (pa->primer_task != pick_pcr_primers_and_hyb_probe 
	&& pa->primer_task != pick_hyb_probe_only
	&& sa->internal_input) {
      pr_append_new_chunk(&sa->error,
			  "Not specified to pick internal oligos");
      pr_append(&sa->error,
		" but a specific internal oligo is provided");
    }
    if (sa->internal_input) {
      if (strlen(sa->internal_input) > pa->io_primer_max_size)
	pr_append_new_chunk(&sa->error, "Specified internal oligo too long");

      if (strlen(sa->internal_input) < pa->io_primer_min_size)
	pr_append_new_chunk(&sa->error, "Specified internal oligo too short");

      if (!strstr_nocase(sa->sequence, sa->internal_input))
	pr_append_new_chunk(&sa->error,
			    "Specified internal oligo not in sequence");
      else if (!strstr_nocase(sa->trimmed_seq, sa->internal_input))
	pr_append_new_chunk(&sa->error,
			    "Specified internal oligo not in Included Region");
    }
    if (sa->left_input) {
      if (strlen(sa->left_input) > pa->primer_max_size)
	pr_append_new_chunk(&sa->error, "Specified left primer too long");
      if (strlen(sa->left_input) < pa->primer_min_size)
	pr_append_new_chunk(&sa->error, "Specified left primer too short");
      if (!strstr_nocase(sa->sequence, sa->left_input))
	pr_append_new_chunk(&sa->error,
			    "Specified left primer not in sequence");
      else if (!strstr_nocase(sa->trimmed_seq, sa->left_input))
	pr_append_new_chunk(&sa->error,
			    "Specified left primer not in Included Region");
    }
    if (sa->right_input) {
      if (strlen(sa->right_input) < pa->primer_min_size)
	pr_append_new_chunk(&sa->error, "Specified right primer too short");
      if (strlen(sa->right_input) > pa->primer_max_size) {
	pr_append_new_chunk(&sa->error, "Specified right primer too long");
       } else { /* We do not want to overflow s1. */
	_pr_reverse_complement(sa->right_input,s1);
	if (!strstr_nocase(sa->sequence, s1))
	  pr_append_new_chunk(&sa->error,
			      "Specified right primer not in sequence");
	else if (!strstr_nocase(sa->trimmed_seq, s1))
	  pr_append_new_chunk(&sa->error,
			      "Specified right primer not in Included Region");
      }
    }

    if ((pa->pr_pair_weights.product_tm_lt || 
	 pa->pr_pair_weights.product_tm_gt)
	&& pa->product_opt_tm == PR_UNDEFINED_DBL_OPT) {
        pr_append_new_chunk(&pa->glob_err, 
	   "Product temperature is part of objective function while optimum temperature is not defined");
        return 1;
     }
	
    if((pa->pr_pair_weights.product_size_lt ||
	pa->pr_pair_weights.product_size_gt) 
       && pa->product_opt_size == PR_UNDEFINED_INT_OPT){
       pr_append_new_chunk(&pa->glob_err,
	  "Product size is part of objective function while optimum size is not defined");
       return 1;
    }

    if ((pa->primer_weights.gc_content_lt || 
	 pa->primer_weights.gc_content_gt)
	&& pa->opt_gc_content == DEFAULT_OPT_GC_PERCENT) {
        pr_append_new_chunk(&pa->glob_err, 
	   "Primer GC content is part of objective function while optimum gc_content is not defined");
        return 1;
     }
	
    if ((pa->io_weights.gc_content_lt || 
	 pa->io_weights.gc_content_gt)
	&& pa->io_opt_gc_content == DEFAULT_OPT_GC_PERCENT) {
        pr_append_new_chunk(&pa->glob_err, 
	   "Hyb probe GC content is part of objective function while optimum gc_content is not defined");
        return 1;
     }
	
    if ((pa->primer_task != pick_pcr_primers_and_hyb_probe 
	 && pa->primer_task != pick_hyb_probe_only ) &&
			(pa->pr_pair_weights.io_quality)) {
       pr_append_new_chunk(&pa->glob_err,
	  "Internal oligo quality is part of objective function while internal oligo choice is not required");
       return 1;
    }

    if (pa->primer_weights.repeat_sim && (!pa->repeat_lib.seq_num)) {
       pr_append_new_chunk(&pa->glob_err,
	  "Mispriming score is part of objective function, but mispriming library is not defined");
       return 1;
    }

    if(pa->io_weights.repeat_sim && (!pa->io_mishyb_library.seq_num)){
      pr_append_new_chunk(&pa->glob_err,
      "Internal oligo mispriming score is part of objective function while mishyb library is not defined");
      return 1;
    }

    if(pa->pr_pair_weights.repeat_sim && (!pa->repeat_lib.seq_num)){
      pr_append_new_chunk(&pa->glob_err,
	"Mispriming score is part of objective function, but mispriming library is not defined");
      return 1;
    }

    if(pa->pr_pair_weights.io_quality 
	&& pa->primer_task != pick_pcr_primers_and_hyb_probe ) {
	  pr_append_new_chunk(&pa->glob_err,
	   "Internal oligo quality is part of objective function while internal oligo choice is not required");
        return 1;
    }

    return (NULL == sa->error.data && NULL == pa->glob_err.data) ? 0 : 1;
}

/* Takes substring of seq starting from n with length m and puts it to s.    */
void
_pr_substr(const char *seq, int n, int m, char *s)
{
	int i;
	for(i=n;i<n+m;i++)s[i-n]=seq[i];
	s[m]='\0';
}

/* Reverse and complement the sequence seq and put the result in s. */ 
void
_pr_reverse_complement(const char *seq, char *s)
{
    const char *p = seq;
    char *q = s;

    while (*p != '\0') p++;
    p--;
    while (p >= seq) {
	switch (*p)
	{
	case 'A': *q='T'; break;
	case 'C': *q='G'; break;
	case 'G': *q='C'; break;
	case 'T': *q='A'; break;
	case 'U': *q='A'; break;

	case 'B': *q='V'; break;
	case 'D': *q='H'; break;
        case 'H': *q='D'; break;
        case 'V': *q='B'; break;
        case 'R': *q='Y'; break;
        case 'Y': *q='R'; break;
	case 'K': *q='M'; break;
        case 'M': *q='K'; break;
        case 'S': *q='S'; break;
        case 'W': *q='W'; break;

	case 'N': *q='N'; break;

	case 'a': *q='t'; break;
	case 'c': *q='g'; break;
	case 'g': *q='c'; break;
	case 't': *q='a'; break;
	case 'u': *q='a'; break;

	case 'b': *q='v'; break;
	case 'd': *q='h'; break;
        case 'h': *q='d'; break;
        case 'v': *q='b'; break;
        case 'r': *q='y'; break;
        case 'y': *q='r'; break;
	case 'k': *q='m'; break;
        case 'm': *q='k'; break;
        case 's': *q='s'; break;
        case 'w': *q='w'; break;

	case 'n': *q='n'; break;
	}
	p--; q++;
    }
    *q = '\0';
}

int
_pr_need_template_mispriming(pa)
  const primer_args *pa;
{
  return 
    pa->max_template_mispriming >= 0
    || pa->primer_weights.template_mispriming > 0.0
    || _pr_need_pair_template_mispriming(pa);
}

int
_pr_need_pair_template_mispriming(pa)
  const primer_args *pa;
{
  return 
    pa->pair_max_template_mispriming >= 0
    || pa->pr_pair_weights.template_mispriming > 0.0;
}

/* Upcase a DNA string, s, in place.  If amibiguity_code_ok is false the
   string can consist of acgtnACGTN.  If it is true then the IUB/IUPAC
   ambiguity codes are are allowed.  Return the first unrecognized letter if
   any is seen (and turn it to 'N' in s).  Otherwise return '\0'.  */
static char
dna_to_upper(s, ambiguity_code_ok)
    char * s;
    int ambiguity_code_ok;
{
  char *p = s;
  int unrecognized_base = '\0';
  while (*p) {
    switch (*p) {
      case 'a': case 'A': *p='A'; break;
      case 'c': case 'C': *p='C'; break;
      case 'g': case 'G': *p='G'; break;
      case 't': case 'T': *p='T'; break;
      case 'n': case 'N': *p='N'; break;
      default: 
	if (ambiguity_code_ok) {
	  switch (*p) {
	  case 'r': case 'R': *p = 'R'; break;
	  case 'y': case 'Y': *p = 'Y'; break;
	  case 'm': case 'M': *p = 'M'; break;
	  case 'w': case 'W': *p = 'W'; break;
	  case 's': case 'S': *p = 'S'; break;
	  case 'k': case 'K': *p = 'K'; break;
	  case 'd': case 'D': *p = 'D'; break;
	  case 'h': case 'H': *p = 'H'; break;
	  case 'v': case 'V': *p = 'V'; break;
	  case 'b': case 'B': *p = 'B'; break;
	  }
	} else {
	  if (!unrecognized_base) unrecognized_base = *p;
	  *p = 'N';
	}
	break;
      }
    p++;
  }
  return unrecognized_base;
}

/* 
 * Check intervals, and add to sa->error.  Update the start of each interval to
 * be relative to the start of the included region.
 */ 
static int
check_intervals(tag_name, num_intervals, intervals, seq_len, sa)
    const char *tag_name;
    const int num_intervals;
    interval_array_t intervals;
    const int seq_len;
    seq_args *sa;
{
    int i;
    int outside_warning_issued = 0;
    for (i=0; i < num_intervals; i++) {
	if (intervals[i][0] + intervals[i][1] > seq_len) {
	    pr_append_new_chunk(&sa->error, tag_name);
	    pr_append(&sa->error, " beyond end of sequence");
	    return 1;
	}
	/* Cause the interval start to be relative to the included region. */
	intervals[i][0] -= sa->incl_s;
	/* Check that intervals are within the included region. */
	if (intervals[i][0] < 0
	    || intervals[i][0] + intervals[i][1] > sa->incl_l) {
	    if (!outside_warning_issued) {
		pr_append_new_chunk(&sa->warning, tag_name);
		pr_append(&sa->warning,
			  " outside of INCLUDED_REGION");
		outside_warning_issued = 1;
	    }
	}
	if (intervals[i][1] < 0) {
	    pr_append_new_chunk(&sa->error, "Negative ");
	    pr_append(&sa->error, tag_name);
	    pr_append(&sa->error, " length");
	    return 1;
	}
    }
    return 0;
}

static char * strstr_nocase(s1, s2)
char *s1, *s2;
{
   int  n1, n2;
   char *p, q, *tmp;

   if(s1 == NULL || s2 == NULL) return NULL;
   n1 = strlen(s1); n2 = strlen(s2);
   if(n1 < n2) return NULL;

   tmp = pr_safe_malloc(n1 + 1);
   strcpy(tmp, s1);

   q = *tmp; p = tmp;
   while(q != '\0' && q != '\n'){
      q = *(p + n2);
      *(p + n2) = '\0';
      if(strcmp_nocase(p, s2)){
	 *(p + n2) = q; p++; continue;
      }
      else {free(tmp); return p;}
   }
   free(tmp); return NULL;
}

void
pr_print_pair_explain(f, sa)
  FILE *f;
  const seq_args *sa;
{
    fprintf(f, "considered %d",sa->pair_expl.considered);
    if (sa->pair_expl.target)
      fprintf(f, ", no target %d", sa->pair_expl.target);
    if (sa->pair_expl.product)
      fprintf(f, ", unacceptable product size %d", sa->pair_expl.product);
    if (sa->pair_expl.low_tm)
      fprintf(f, ", low product Tm %d", sa->pair_expl.low_tm);
    if (sa->pair_expl.high_tm)
      fprintf(f, ", high product Tm %d", sa->pair_expl.high_tm);
    if (sa->pair_expl.temp_diff) 
      fprintf(f, ", tm diff too large %d",sa->pair_expl.temp_diff);
    if (sa->pair_expl.compl_any) 
      fprintf(f, ", high any compl %d", sa->pair_expl.compl_any);
    if (sa->pair_expl.compl_end) 
      fprintf(f, ", high end compl %d", sa->pair_expl.compl_end);
    if (sa->pair_expl.internal) 
      fprintf(f, ", no internal oligo %d", sa->pair_expl.internal);
    if (sa->pair_expl.repeat_sim)
      fprintf(f, ", high mispriming library similarity %d",
	      sa->pair_expl.repeat_sim);
    if (sa->pair_expl.template_mispriming)
      fprintf(f, ", high template mispriming score %d",
	      sa->pair_expl.template_mispriming);
    fprintf(f, ", ok %d\n", sa->pair_expl.ok);
}

char *
libprimer3_release(void) {
  return "libprimer3 release 2.0.0";
}

/* =========================================================== */
/* Various fail-stop wrappers for standard library functions.  */

void *
pr_safe_malloc(x)
    size_t x;
{
    void *r = malloc(x);
    if (NULL == r) OOM_ERROR;
    return r;
}

void *
pr_safe_realloc(p, x)
    void *p;
    size_t x;
{
    void *r = realloc(p, x);
    if (NULL == r) OOM_ERROR;
    return r;
}

FILE *
safe_fopen(path, mode)
    const char *path, *mode;
{
    FILE *r = fopen(path, mode);
    if (NULL == r) {
	fprintf(stderr, "%s: unable to open file %s:",
		pr_program_name, path);
	perror("");
	exit (-1);
    }
    return r;
}

/* End of fail-stop wrappers. */
/* =========================================================== */