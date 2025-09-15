/***************************************************************************
 * msmod.c - Mini-SEED modifier.
 *
 * Opens user specified file(s), parses records, makes specified
 * modifications and writes the data back out.
 *
 * Written by Chad Trabant, IRIS Data Management Center.
 ***************************************************************************/

/* Note to future hackers:
 *
 * The current framework does not allow easily adding the modification
 * of key fields like byte-order and record length because modifying
 * these fields triggers libmseed to make changes during packing.
 * This is in part due to the assumption in libmseed that records read
 * are valid and not fundamentally broken.  For instance an option to
 * change the byte-order flag would imply that the original byte-order
 * is not correct or, in the case of libmseed, that the user is
 * requesting a change. A modification engine that does not require
 * the use of libmseed's ms_packheader() would be able to make any
 * arbitrary change, but said engine would additionally need to deal
 * with all the byte order and parsing issues.
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <regex.h>
#include <math.h>

#include <libmseed.h>

#include "dsarchive.h"

#define VERSION "1.2.1"
#define PACKAGE "msmod"

/* A simple bitwise AND test to return 0 or 1 */
#define bittest(x,y) (x&y)?1:0

/* For a linked list of input files */
typedef struct Filelink_s {
  char *filename;
  struct Filelink_s *next;
} Filelink;

/* Archive (output structure) definition containers */
typedef struct Archive_s {
  DataStream  datastream;
  struct Archive_s *next;
} Archive;

typedef struct ClockCorrConfig_s {
  char type[20];
  hptime_t *inst_time;
  hptime_t *ref_time;
  int num_records;
  double *coeff;
  int num_coeffs;
} ClockCorrConfig;

typedef struct {
    size_t n;
    double *x;
    double *a;
    double *b;
    double *c;
    double *d;
} CubicSpline;


static int processmods (MSRecord *msr);
static int processparam (int argcount, char **argvec);
static char *getoptval (int argcount, char **argvec, int argopt);
static void addfile (char *filename);
static int lisnumber (char *number);
static int  addarchive(const char *path, const char *layout);
static int readregexfile (char *regexfile, char **pppattern);
static void freefilelist (void);
static ClockCorrConfig *read_cc_config(char *ccfilename);
static double *parse_doubles(const char *input, int *count_out);
static int  process_cc(ClockCorrConfig *cc_confg, MSRecord *msr);
static int process_cc_is_in_bounds(hptime_t msr_hptime, ClockCorrConfig *cc_confg);
static int process_calc_cc(hptime_t msr_hptime, ClockCorrConfig *cc_config, hptime_t *correction);
static int process_calc_cc_linear(hptime_t msr_hptime, ClockCorrConfig *cc_config,   hptime_t *correction);
static int process_calc_cc_spline(hptime_t msr_hptime, ClockCorrConfig *cc_config,   hptime_t *correction);
static int process_calc_cc_polynomial(hptime_t msr_hptime, ClockCorrConfig *cc_config,   hptime_t *correction);
int  verify_cc_polynomial_config(ClockCorrConfig *cc_config);
static int build_cubic_spline(const double *x, const double *y, size_t n, CubicSpline *spline);
static double evaluate_spline(const CubicSpline *spline, double xval);


static void usage (int level);

static flag     verbose        = 0;
static flag     basicsum       = 0;    /* Controls printing of basic summary */
static int      reclen         = -1;   /* Input data record length, autodetected in most cases */
static hptime_t starttime      = HPTERROR;  /* Limit to records after starttime */
static hptime_t endtime        = HPTERROR;  /* Limit to records before endtime */
static hptime_t starttimecont  = HPTERROR;  /* Limit to records that contain or start after starttime */
static hptime_t endtimecont    = HPTERROR;  /* Limit to records that contain or end before endtime */
static regex_t *match          = 0;    /* Compiled match regex */
static regex_t *reject         = 0;    /* Compiled reject regex */
static char    *outputfile     = 0;    /* Single output file */
static Archive *archiveroot    = 0;    /* Output file structures */
static flag     overwriteinput = 0;    /* Overwrite input records after modifying */
static Filelink *filelist = 0;
static ClockCorrConfig *cc_config = NULL;  /*Structure holding Clock Correction config */

/* Modification specifiers */
static char    *modnet          = 0;
static char    *modsta          = 0;
static char    *modloc          = 0;
static char    *modchan         = 0;
static char     modquality      = 0;
static double   modtimeshift    = 0;
static double   modtimecorr     = 0;
static double   modtimecorrval  = 0;
static char     modtimecorrapp  = 0;
static double   modsamprate     = 0;
static char     mod0actflags    = 0;
static char     mod1actflags    = 0;
static char     mod0ioflags     = 0;
static char     mod1ioflags     = 0;
static char     mod0dqflags     = 0;
static char     mod1dqflags     = 0;
static double   modb100samprate = 0;
static int      modb1000enc     = 0;
static int      modb1001tqual   = 0;


int
main ( int argc, char **argv )
{
  Filelink *flp;
  MSRecord *msr = 0;
  int retcode = MS_NOERROR;
  FILE *ofp = 0;
  flag stopflag = 0;
  off_t filepos = 0;

  long long int totalfiles = 0;
  long long int totalrecs  = 0;

  char matchsrc[50];
  char srcname[50];
  char stime[30];

  int writefd = 0;
  Archive *arch;

  /* Process input parameters */
  if (processparam (argc, argv) < 0)
    return -1;

  /* Open the output file if specified */
  if ( outputfile )
    {
      if ( strcmp (outputfile, "-") == 0 )
        {
          ofp = stdout;
        }
      else if ( (ofp = fopen (outputfile, "wb")) == NULL )
        {
          fprintf (stderr, "Cannot open output file: %s (%s)\n",
                   outputfile, strerror(errno));
          return -1;
        }
    }

  flp = filelist;

  while ( flp != 0 && ! stopflag )
    {
      if ( verbose >= 2 )
	fprintf (stderr, "Processing: %s\n", flp->filename);

      /* Open input file for writing if overwriting and not using stdin */
      if ( overwriteinput && strcmp (flp->filename, "_") )
	{
	  if ( (writefd = open (flp->filename, O_WRONLY, 0)) == -1 )
	    {
	      fprintf (stderr, "Error opening %s for overwriting: %s\n",
		       flp->filename, strerror(errno));
	      flp = flp->next;
	    }
	}

      /* Loop over the input file */
      for (;;)
        {
          if ( (retcode = ms_readmsr (&msr, flp->filename, reclen, &filepos,
                                      NULL, 1, 0, verbose)) != MS_NOERROR )
            break;

          /* Check if record matches start/end time criteria */
          if ( starttime != HPTERROR && (msr->starttime < starttime) )
            {
              if ( verbose >= 3 )
                {
                  msr_srcname (msr, srcname, 1);
                  ms_hptime2seedtimestr (msr->starttime, stime, 1);
                  fprintf (stderr, "Skipping (starttime) %s, %s\n", srcname, stime);
                }
              continue;
            }

          if ( endtime != HPTERROR && (msr_endtime(msr) > endtime) )
            {
              if ( verbose >= 3 )
                {
                  msr_srcname (msr, srcname, 1);
                  ms_hptime2seedtimestr (msr->starttime, stime, 1);
                  fprintf (stderr, "Skipping (endtime) %s, %s\n", srcname, stime);
                }
              continue;
            }

          /* Check if record matches start/end time criteria */
          if ( starttimecont != HPTERROR || endtimecont != HPTERROR )
          {
            hptime_t recendtime = msr_endtime (msr);

            if ( starttimecont != HPTERROR && (msr->starttime < starttimecont && ! (msr->starttime <= starttimecont && recendtime >= starttimecont)) )
            {
              if ( verbose >= 3 )
              {
                msr_srcname (msr, srcname, 1);
                ms_hptime2seedtimestr (msr->starttime, stime, 1);
                ms_log (1, "Skipping (starttime contains) %s, %s\n", srcname, stime);
              }
              continue;
            }

            if ( endtimecont != HPTERROR && (recendtime > endtimecont && ! (msr->starttime <= endtimecont && recendtime >= endtimecont)) )
            {
              if ( verbose >= 3 )
              {
                msr_srcname (msr, srcname, 1);
                ms_hptime2seedtimestr (msr->starttime, stime, 1);
                ms_log (1, "Skipping (endtime contains) %s, %s\n", srcname, stime);
              }
              continue;
            }
          }

          if ( match || reject )
            {
              /* Generate the srcname including the quality code */
              msr_srcname (msr, matchsrc, 1);

              /* Check if record is matched by the match regex */
              if ( match )
                {
                  if ( regexec (match, matchsrc, 0, 0, 0) != 0 )
                    {
                      if ( verbose >= 3 )
                        {
                          ms_hptime2seedtimestr (msr->starttime, stime, 1);
                          fprintf (stderr, "Skipping (match) %s, %s\n", srcname, stime);
                        }
                      continue;
                    }
                }

              /* Check if record is rejected by the reject regex */
              if ( reject )
                {
                  if ( regexec (reject, matchsrc, 0, 0, 0) == 0 )
                    {
                      if ( verbose >= 3 )
                        {
                          ms_hptime2seedtimestr (msr->starttime, stime, 1);
                          fprintf (stderr, "Skipping (reject) %s, %s\n", srcname, stime);
                        }
                      continue;
                    }
                }
            }

          if ( verbose )
	    {
              msr_print (msr, verbose-1);
            }

	  /* Revert time to uncorrected value if correction was applied during unpacking */
	  if ( msr->fsdh->time_correct != 0 && ! (msr->fsdh->act_flags & 0x02) )
	    {
	      msr->starttime = msr_starttime_uc (msr);
	    }

	  /* Perform modifications to record header */
	  retcode = processmods (msr);
	  if ( retcode )
	    {
	      fprintf (stderr, "ERROR modifying:\n  ");
	      msr_print (msr, verbose-1);
	      stopflag = 1;
	      break;
	    }

	  /* Repack header into record */
	  if ( msr_pack_header (msr, 1, verbose-1) < 0 )
	    {
	      fprintf (stderr, "ERROR packing header for:\n  ");
	      msr_print (msr, verbose-1);
	      stopflag = 1;
	      break;
	    }

	  /* Replace input record if specified */
	  if ( overwriteinput && writefd )
	    {
	      if ( pwrite (writefd, msr->record, msr->reclen, filepos) != msr->reclen )
		{
		  fprintf (stderr, "ERROR overwriting record in %s: %s\n",
			   flp->filename, strerror(errno));
		  break;
		}
	    }

	  /* Write to a single output file if specified */
	  if ( ofp )
	    {
	      if ( fwrite (msr->record, msr->reclen, 1, ofp) != 1 )
		{
		  fprintf (stderr, "ERROR writing to '%s'\n", outputfile);
		  stopflag = 1;
		  break;
		}
	    }

	  /* Write to Archive(s) if specified */
	  if ( archiveroot )
	    {
	      arch = archiveroot;
	      while ( arch )
		{
		  if ( ds_streamproc (&arch->datastream, msr, 0, verbose-1) )
		    {
		      fprintf (stderr, "ERROR writing to archive (%s)\n", arch->datastream.path);
		      stopflag = 1;
		      break;
		    }

		  arch = arch->next;
		}
	    }

	  /* Update record count */
          totalrecs++;
	} /* End of reading records from file */

      /* Print error if not EOF and not counting down records */
      if ( retcode != MS_ENDOFFILE )
        fprintf (stderr, "Error processing %s: %s\n",
                 flp->filename, ms_errorstr(retcode));

      /* Close input file for overwriting */
      if ( writefd )
	{
	  close (writefd);
	  writefd = 0;
	}

      /* Make sure everything is cleaned up */
      ms_readmsr (&msr, NULL, 0, NULL, NULL, 0, 0, 0);

      totalfiles++;
      flp = flp->next;
    } /* End of looping over file list */

  if ( outputfile )
    fclose (ofp);

  if ( basicsum )
    printf ("Files: %lld, Records: %lld\n", totalfiles, totalrecs);

  freefilelist();

  return 0;
}  /* End of main() */


/***************************************************************************
 * processmods():
 *
 * Process all specified modifcations on the specified MSRecord.  No
 * field validation is done in this routine, all values are expected
 * to be valid for each given field.
 *
 * Returns 0 on success, and -1 on failure
 ***************************************************************************/
static int
processmods (MSRecord *msr)
{
  if ( ! msr )
    return -1;

  /* Modify network code */
  if ( modnet )
    {
      strncpy (msr->network, modnet, sizeof(msr->network));
    }

  /* Modify station code */
  if ( modsta )
    {
      strncpy (msr->station, modsta, sizeof(msr->station));
    }

  /* Modify location code */
  if ( modloc )
    {
      strncpy (msr->location, modloc, sizeof(msr->location));
    }

  /* Modify channel code */
  if ( modchan )
    {
      int idx = 0;
      while ( modchan[idx] && idx < (sizeof(msr->channel)-1) )
	{
	  if ( modchan[idx] != '.' )
	    msr->channel[idx] = modchan[idx];
	  idx++;
	}
      msr->channel[idx] = '\0';
    }

  /* Modify data header indicator/quality code */
  if ( modquality )
    {
      msr->dataquality = modquality;
    }

  /* Modify time tag */
  if ( modtimeshift && msr->fsdh )
    {
      if ( verbose > 1 )
	fprintf (stderr, "Shifting record start time by %g seconds\n", modtimeshift);

      /* Apply time shift to starttime */
      msr->starttime += (hptime_t) (modtimeshift * HPTMODULUS);
    }

  /* Modify time correction value and apply to the time tag */
  if ( modtimecorr && msr->fsdh )
    {
      if ( verbose > 1 )
	fprintf (stderr, "Applying time correction of %g seconds\n", modtimeshift);

      if ( verbose && msr->fsdh->time_correct && ! (msr->fsdh->act_flags & 0x02) )
	fprintf (stderr, "Warning, setting time correction over an unapplied value\n");

      /* Set the time correction applied flag (bit 1 of the activitiy flags) */
      msr->fsdh->act_flags |= 0x02;

      /* Set the time correction field, value is units of 0.0001 seconds */
      msr->fsdh->time_correct = modtimecorr * 10000;

      /* Apply time shift to starttime */
      msr->starttime += (hptime_t) (modtimecorr * HPTMODULUS);
    }

  /* Modify time correction value without applying to the time tag */
  if ( modtimecorrval && msr->fsdh )
    {
      /* Set the time correction field, value is units of 0.0001 seconds */
      msr->fsdh->time_correct = modtimecorrval * 10000;
    }

  /* Apply time correction value to the time tag */
  if ( modtimecorrapp && msr->fsdh )
    {
      /* Check if time correction field is set and if it's been applied */
      if ( msr->fsdh->time_correct != 0 && ! (msr->fsdh->act_flags & 0x02) )
	{
	  /* Set time to corrected value and set the time correction flag */
	  msr->starttime = msr_starttime(msr);
	  msr->fsdh->act_flags |= 0x02;
	}
    }

  /* Modify sampling rate */
  if ( modsamprate )
    {
      msr->samprate = modsamprate;
    }

  /* Modify activity flags */
  if ( mod0actflags )
    {
      /* Reverse sense of bit set for later XOR */
      mod0actflags ^= 0xFF;

      if ( msr->fsdh )
	/* XOR bit set with the activity flags */
	msr->fsdh->act_flags &= mod0actflags;
      else
	fprintf (stderr, "ERROR, no FSDH for record, that's really bad\n");
    }
  if ( mod1actflags )
    {
      if ( msr->fsdh )
	/* OR bit set with the activity flags */
	msr->fsdh->act_flags |= mod1actflags;
      else
	fprintf (stderr, "ERROR, no FSDH for record, that's really bad\n");
    }

  /* Modify I/O flags */
  if ( mod0ioflags )
    {
      /* Reverse sense of bit set for later XOR */
      mod0ioflags ^= 0xFF;

      if ( msr->fsdh )
	/* XOR bit set with the I/O flags */
	msr->fsdh->io_flags &= mod0ioflags;
      else
	fprintf (stderr, "ERROR, no FSDH for record, that's really bad\n");
    }
  if ( mod1ioflags )
    {
      if ( msr->fsdh )
	/* OR bit set with the I/O flags */
	msr->fsdh->io_flags |= mod1ioflags;
      else
	fprintf (stderr, "ERROR, no FSDH for record, that's really bad\n");
    }

  /* Modify data quality flags */
  if ( mod0dqflags )
    {
      /* Reverse sense of bit set for later XOR */
      mod0dqflags ^= 0xFF;

      if ( msr->fsdh )
	/* XOR bit set with the data quality flags */
	msr->fsdh->dq_flags &= mod0dqflags;
      else
	fprintf (stderr, "ERROR, no FSDH for record, that's really bad\n");
    }
  if ( mod1dqflags )
    {
      if ( msr->fsdh )
	/* OR bit set with the data quality flags */
	msr->fsdh->dq_flags |= mod1dqflags;
      else
	fprintf (stderr, "ERROR, no FSDH for record, that's really bad\n");
    }

  /* Modify Blockette 100 actual sample rate */
  if ( modb100samprate )
    {
      if ( msr->Blkt100 )
	msr->Blkt100->samprate = modb100samprate;
    }

  /* Modify Blockette 1000 encoding format */
  if ( modb1000enc )
    {
      /* This value will be copied into Blockette 1000 during packing */
      msr->encoding = modb1000enc;
    }

  /* Modify Blockette 1001 timing quality value */
  if ( modb1001tqual )
    {
      if ( msr->Blkt1001 )
	msr->Blkt1001->timing_qual = modb1001tqual;
    }
    
  /* Do Clock Correction if requested */
  if ( cc_config && msr->fsdh )
    {
       if ( process_cc(cc_config, msr) )
       {
     	  fprintf (stderr, "ERROR, processing Clock Correction failed\n");
     	  return -1;
       }  
    }    

  return 0;
}  /* End of processmods() */


/***************************************************************************
 * processparam():
 * Process the command line parameters.
 *
 * Returns 0 on success, and -1 on failure
 ***************************************************************************/
static int
processparam (int argcount, char **argvec)
{
  int optind;
  char *matchpattern = 0;
  char *rejectpattern = 0;
  char *ccfilename = NULL;
  char *tptr;
  char *bit,*val;

  /* Process all command line arguments */
  for (optind = 1; optind < argcount; optind++)
    {
      if (strcmp (argvec[optind], "-V") == 0)
	{
	  fprintf (stderr, "%s version: %s\n", PACKAGE, VERSION);
	  exit (0);
	}
      else if (strcmp (argvec[optind], "-h") == 0)
	{
	  usage (0);
	  exit (0);
	}
      else if (strcmp (argvec[optind], "-H") == 0)
	{
	  usage (1);
	  exit (0);
	}
      else if (strncmp (argvec[optind], "-v", 2) == 0)
	{
	  verbose += strspn (&argvec[optind][1], "v");
	}
      else if (strcmp (argvec[optind], "-s") == 0)
	{
	  basicsum = 1;
	}
      else if (strcmp (argvec[optind], "-ts") == 0)
	{
	  starttime = ms_seedtimestr2hptime (getoptval(argcount, argvec, optind++));
	  if ( starttime == HPTERROR )
	    return -1;
	}
      else if (strcmp (argvec[optind], "-te") == 0)
	{
	  endtime = ms_seedtimestr2hptime (getoptval(argcount, argvec, optind++));
	  if ( endtime == HPTERROR )
	    return -1;
	}
      else if (strcmp (argvec[optind], "-tsc") == 0)
	{
	  starttimecont = ms_seedtimestr2hptime (getoptval(argcount, argvec, optind++));
	  if ( starttimecont == HPTERROR )
	    return -1;
	}
      else if (strcmp (argvec[optind], "-tec") == 0)
	{
	  endtimecont = ms_seedtimestr2hptime (getoptval(argcount, argvec, optind++));
	  if ( endtimecont == HPTERROR )
	    return -1;
	}
      else if (strcmp (argvec[optind], "-M") == 0)
	{
	  matchpattern = getoptval(argcount, argvec, optind++);
	}
      else if (strcmp (argvec[optind], "-R") == 0)
	{
	  rejectpattern = getoptval(argcount, argvec, optind++);
	}
      else if (strcmp (argvec[optind], "-i") == 0)
        {
          overwriteinput = 1;
        }
      else if (strcmp (argvec[optind], "-o") == 0)
        {
          outputfile = getoptval(argcount, argvec, optind++);
        }
      else if (strcmp (argvec[optind], "-A") == 0)
        {
          if ( addarchive(getoptval(argcount, argvec, optind++), NULL) == -1 )
            return -1;
        }
      else if (strcmp (argvec[optind], "-CHAN") == 0)
        {
          if ( addarchive(getoptval(argcount, argvec, optind++), CHANLAYOUT) == -1 )
            return -1;
        }
      else if (strcmp (argvec[optind], "-QCHAN") == 0)
        {
          if ( addarchive(getoptval(argcount, argvec, optind++), QCHANLAYOUT) == -1 )
            return -1;
        }
      else if (strcmp (argvec[optind], "-CDAY") == 0)
        {
          if ( addarchive(getoptval(argcount, argvec, optind++), CDAYLAYOUT) == -1 )
            return -1;
        }
      else if (strcmp (argvec[optind], "-BUD") == 0)
        {
          if ( addarchive(getoptval(argcount, argvec, optind++), BUDLAYOUT) == -1 )
            return -1;
        }
      else if (strcmp (argvec[optind], "-CSS") == 0)
        {
          if ( addarchive(getoptval(argcount, argvec, optind++), CSSLAYOUT) == -1 )
            return -1;
        }
      else if (strcmp (argvec[optind], "--net") == 0)
        {
	  modnet = getoptval(argcount, argvec, optind++);
        }
      else if (strcmp (argvec[optind], "--sta") == 0)
        {
	  modsta = getoptval(argcount, argvec, optind++);
        }
      else if (strcmp (argvec[optind], "--loc") == 0)
        {
	  modloc = getoptval(argcount, argvec, optind++);
        }
      else if (strcmp (argvec[optind], "--chan") == 0)
        {
	  modchan = getoptval(argcount, argvec, optind++);
        }
      else if (strcmp (argvec[optind], "--cc") == 0)
	{
	  ccfilename = getoptval(argcount, argvec, optind++);
	}
      else if (strcmp (argvec[optind], "--quality") == 0)
        {
	  tptr = getoptval(argcount, argvec, optind++);
	  modquality = *tptr;

	  if ( ! MS_ISDATAINDICATOR(modquality) )
	    fprintf (stderr, "WARNING: '%c' is not a recognized data quality indicator\n", modquality);
        }
      else if (strcmp (argvec[optind], "--timeshift") == 0)
        {
	  modtimeshift = strtod (getoptval(argcount, argvec, optind++) ,NULL);
        }
      else if (strcmp (argvec[optind], "--timecorr") == 0)
        {
	  modtimecorr = strtod (getoptval(argcount, argvec, optind++) ,NULL);
        }
      else if (strcmp (argvec[optind], "--timecorrval") == 0)
        {
	  modtimecorrval = strtod (getoptval(argcount, argvec, optind++) ,NULL);
        }
      else if (strcmp (argvec[optind], "--applytimecorr") == 0)
        {
	  modtimecorrapp = 1;
        }
      else if (strcmp (argvec[optind], "--samprate") == 0)
        {
	  modsamprate = strtod (getoptval(argcount, argvec, optind++) ,NULL);
        }
      else if (strcmp (argvec[optind], "--actflags") == 0)
        {
	  bit = getoptval(argcount, argvec, optind++);
	  val = bit+2;

	  if ( *(bit+1) != ',' )
	    {
	      fprintf (stderr, "ERROR, 'bit,value' format unrecognized\n");
	      return -1;
	    }

	  if ( *val != '0' && *val != '1' )
	    {
	      fprintf (stderr, "ERROR, 'value' of bit must be 0 or 1\n");
	      return -1;
	    }

	  switch ( *bit ) {
	  case '0': if ( *val == '0' ) mod0actflags |= 0x01; else mod1actflags |= 0x01; break;
	  case '1': if ( *val == '0' ) mod0actflags |= 0x02; else mod1actflags |= 0x02; break;
	  case '2': if ( *val == '0' ) mod0actflags |= 0x04; else mod1actflags |= 0x04; break;
	  case '3': if ( *val == '0' ) mod0actflags |= 0x08; else mod1actflags |= 0x08; break;
	  case '4': if ( *val == '0' ) mod0actflags |= 0x10; else mod1actflags |= 0x10; break;
	  case '5': if ( *val == '0' ) mod0actflags |= 0x20; else mod1actflags |= 0x20; break;
	  case '6': if ( *val == '0' ) mod0actflags |= 0x40; else mod1actflags |= 0x40; break;
	  case '7': if ( *val == '0' ) mod0actflags |= 0x80; else mod1actflags |= 0x80; break;
	  default:  fprintf (stderr, "ERROR, unrecognized activity flag bit: '%c'\n", *bit); return -1;
	  }
        }
      else if (strcmp (argvec[optind], "--ioflags") == 0)
        {
	  bit = getoptval(argcount, argvec, optind++);
	  val = bit+2;

	  if ( *(bit+1) != ',' )
	    {
	      fprintf (stderr, "ERROR, 'bit,value' format unrecognized\n");
	      return -1;
	    }

	  if ( *val != '0' && *val != '1' )
	    {
	      fprintf (stderr, "ERROR, 'value' of bit must be 0 or 1\n");
	      return -1;
	    }

	  switch ( *bit ) {
	  case '0': if ( *val == '0' ) mod0ioflags |= 0x01; else mod1ioflags |= 0x01; break;
	  case '1': if ( *val == '0' ) mod0ioflags |= 0x02; else mod1ioflags |= 0x02; break;
	  case '2': if ( *val == '0' ) mod0ioflags |= 0x04; else mod1ioflags |= 0x04; break;
	  case '3': if ( *val == '0' ) mod0ioflags |= 0x08; else mod1ioflags |= 0x08; break;
	  case '4': if ( *val == '0' ) mod0ioflags |= 0x10; else mod1ioflags |= 0x10; break;
	  case '5': if ( *val == '0' ) mod0ioflags |= 0x20; else mod1ioflags |= 0x20; break;
	  case '6': if ( *val == '0' ) mod0ioflags |= 0x40; else mod1ioflags |= 0x40; break;
	  case '7': if ( *val == '0' ) mod0ioflags |= 0x80; else mod1ioflags |= 0x80; break;
	  default:  fprintf (stderr, "ERROR, unrecognized I/O flag bit: '%c'\n", *bit); return -1;
	  }
        }
      else if (strcmp (argvec[optind], "--dqflags") == 0)
        {
	  bit = getoptval(argcount, argvec, optind++);
	  val = bit+2;

	  if ( *(bit+1) != ',' )
	    {
	      fprintf (stderr, "ERROR, 'bit,value' format unrecognized\n");
	      return -1;
	    }

	  if ( *val != '0' && *val != '1' )
	    {
	      fprintf (stderr, "ERROR, 'value' of bit must be 0 or 1\n");
	      return -1;
	    }

	  switch ( *bit ) {
	  case '0': if ( *val == '0' ) mod0dqflags |= 0x01; else mod1dqflags |= 0x01; break;
	  case '1': if ( *val == '0' ) mod0dqflags |= 0x02; else mod1dqflags |= 0x02; break;
	  case '2': if ( *val == '0' ) mod0dqflags |= 0x04; else mod1dqflags |= 0x04; break;
	  case '3': if ( *val == '0' ) mod0dqflags |= 0x08; else mod1dqflags |= 0x08; break;
	  case '4': if ( *val == '0' ) mod0dqflags |= 0x10; else mod1dqflags |= 0x10; break;
	  case '5': if ( *val == '0' ) mod0dqflags |= 0x20; else mod1dqflags |= 0x20; break;
	  case '6': if ( *val == '0' ) mod0dqflags |= 0x40; else mod1dqflags |= 0x40; break;
	  case '7': if ( *val == '0' ) mod0dqflags |= 0x80; else mod1dqflags |= 0x80; break;
	  default:  fprintf (stderr, "ERROR, unrecognized data quality flag bit: '%c'\n", *bit); return -1;
	  }
        }
      else if (strcmp (argvec[optind], "--b100samprate") == 0)
        {
	  modb100samprate = strtod (getoptval(argcount, argvec, optind++) ,NULL);
        }
      else if (strcmp (argvec[optind], "--b1000encoding") == 0)
        {
	  modb1000enc = strtol (getoptval(argcount, argvec, optind++) ,NULL,10);

	  if ( modb1000enc < 0 || modb1000enc > 31 ) {
	    fprintf (stderr, "ERROR, unrecognized encoding format: '%d'\n", modb1000enc);
	    return -1;
	  }
        }
      else if (strcmp (argvec[optind], "--b1001tqual") == 0)
        {
	  modb1001tqual = strtol (getoptval(argcount, argvec, optind++) ,NULL,10);

	  if ( modb1001tqual < 0 || modb1001tqual > 100 )
	    {
	      fprintf (stderr, "ERROR, timing quality must be 0 to 100\n");
	      return -1;
	    }
        }
      else if (strncmp (argvec[optind], "-", 1) == 0 &&
	       strlen (argvec[optind]) > 1 )
	{
	  fprintf(stderr, "ERROR Unknown option: %s\n", argvec[optind]);
	  exit (1);
	}
      else
	{
	  addfile (argvec[optind]);
	}
    }

  /* Make sure input file(s) were specified */
  if ( filelist == 0 )
    {
      fprintf (stderr, "No input files were specified\n\n");
      fprintf (stderr, "%s version %s\n\n", PACKAGE, VERSION);
      fprintf (stderr, "Try %s -h for usage\n", PACKAGE);
      exit (1);
    }

  /* Overwrite input data records if no output file(s) specified */
  if ( ! outputfile && ! archiveroot && ! overwriteinput )
    {
      fprintf (stderr, "No output options were specified\n\n");
      fprintf (stderr, "%s version %s\n\n", PACKAGE, VERSION);
      fprintf (stderr, "Try %s -h for usage\n", PACKAGE);
      exit (1);
    }

  /* Expand match pattern from a file if prefixed by '@' */
  if ( matchpattern )
    {
      if ( *matchpattern == '@' )
	{
	  tptr = matchpattern + 1; /* Skip the @ sign */
	  matchpattern = 0;

	  if ( readregexfile (tptr, &matchpattern) <= 0 )
	    {
	      fprintf (stderr, "ERROR reading match pattern regex file\n");
	      exit (1);
	    }
	}
    }

  /* Expand reject pattern from a file if prefixed by '@' */
  if ( rejectpattern )
    {
      if ( *rejectpattern == '@' )
	{
	  tptr = rejectpattern + 1; /* Skip the @ sign */
	  rejectpattern = 0;

	  if ( readregexfile (tptr, &rejectpattern) <= 0 )
	    {
	      fprintf (stderr, "ERROR reading reject pattern regex file\n");
	      exit (1);
	    }
	}
    }

  /* Compile match and reject patterns */
  if ( matchpattern )
    {
      match = (regex_t *) malloc (sizeof(regex_t));

      if ( regcomp (match, matchpattern, REG_EXTENDED) != 0)
	{
	  fprintf (stderr, "ERROR compiling match regex: '%s'\n", matchpattern);
	}
    }

  if ( rejectpattern )
    {
      reject = (regex_t *) malloc (sizeof(regex_t));

      if ( regcomp (reject, rejectpattern, REG_EXTENDED) != 0)
	{
	  fprintf (stderr, "ERROR compiling reject regex: '%s'\n", rejectpattern);
	}
    }

  /* Read Clock Correction parameter file */
  if ( ccfilename )
    {
      cc_config = read_cc_config(ccfilename);

      if (!cc_config)
	{
	  fprintf (stderr, "ERROR reading CC configuration: '%s'\n", ccfilename);
           exit (1);
	}
    }


  /* Report the program version */
  if ( verbose )
    fprintf (stderr, "%s version: %s\n", PACKAGE, VERSION);

  return 0;
}  /* End of processparam() */


/***************************************************************************
 * getoptval:
 * Return the value to a command line option; checking that the value is
 * itself not an option (starting with '-') and is not past the end of
 * the argument list.
 *
 * argcount: total arguments in argvec
 * argvec: argument list
 * argopt: index of option to process, value is expected to be at argopt+1
 *
 * Returns value on success and exits with error message on failure
 ***************************************************************************/
static char *
getoptval (int argcount, char **argvec, int argopt)
{
  if ( argvec == NULL || argvec[argopt] == NULL ) {
    fprintf (stderr, "ERROR getoptval(): NULL option requested\n");
    exit (1);
    return 0;
  }

  /* Special case of '-o -' usage */
  if ( (argopt+1) < argcount && strcmp (argvec[argopt], "-o") == 0 )
    if ( strcmp (argvec[argopt+1], "-") == 0 )
      return argvec[argopt+1];

  /* Special case of '--timeshift -X' */
  if ( (argopt+1) < argcount && strcmp (argvec[argopt], "--timeshift") == 0 )
    if ( lisnumber(argvec[argopt+1]) )
      return argvec[argopt+1];

  if ( (argopt+1) < argcount && *argvec[argopt+1] != '-' )
    return argvec[argopt+1];

  fprintf (stderr, "ERROR Option %s requires a value, try -h for usage\n", argvec[argopt]);
  exit (1);
  return 0;
}  /* End of getoptval() */


/***************************************************************************
 * lisnumber:
 *
 * Test if the string is all digits allowing an initial minus sign and
 * any number of dots (.).
 *
 * Return 0 if not a number otherwise 1.
 ***************************************************************************/
static int
lisnumber (char *number)
{
  int idx = 0;

  while ( *(number+idx) )
    {
      if ( idx == 0 && *(number+idx) == '-' )
        {
          idx++;
          continue;
        }

      if ( ! isdigit ((int) *(number+idx)) && *(number+idx) != '.' )
        {
          return 0;
        }

      idx++;
    }

  return 1;
}  /* End of lisnumber() */


/***************************************************************************
 * addfile:
 *
 * Add file to end of the global file list (filelist).
 ***************************************************************************/
static void
addfile (char *filename)
{
  Filelink *lastlp, *newlp;

  if ( filename == NULL )
    {
      fprintf (stderr, "addfile(): No file name specified\n");
      return;
    }

  lastlp = filelist;
  while ( lastlp != 0 )
    {
      if ( lastlp->next == 0 )
        break;

      lastlp = lastlp->next;
    }

  newlp = (Filelink *) malloc (sizeof (Filelink));
  newlp->filename = strdup(filename);
  newlp->next = 0;

  if ( lastlp == 0 )
    filelist = newlp;
  else
    lastlp->next = newlp;

}  /* End of addfile() */


/***************************************************************************
 * addarchive:
 * Add entry to the data stream archive chain.  'layout' if defined
 * will be appended to 'path'.
 *
 * Returns 0 on success, and -1 on failure
 ***************************************************************************/
static int
addarchive ( const char *path, const char *layout )
{
  Archive *newarch;
  int pathlayout;

  if ( ! path )
    {
      fprintf (stderr, "addarchive: cannot add archive with empty path\n");
      return -1;
    }

  newarch = (Archive *) malloc (sizeof (Archive));

  if ( newarch == NULL )
    {
      fprintf (stderr, "addarchive: cannot allocate memory for new archive definition\n");
      return -1;
    }

  /* Setup new entry and add it to the front of the chain */
  pathlayout = strlen (path) + 2;
  if ( layout )
    pathlayout += strlen (layout);

  newarch->datastream.path = (char *) malloc (pathlayout);

  if ( layout )
    snprintf (newarch->datastream.path, pathlayout, "%s/%s", path, layout);
  else
    snprintf (newarch->datastream.path, pathlayout, "%s", path);

  newarch->datastream.grouproot = NULL;

  if ( newarch->datastream.path == NULL )
    {
      fprintf (stderr, "addarchive: cannot allocate memory for new archive path\n");
      if ( newarch )
        free (newarch);
      return -1;
    }

  newarch->next = archiveroot;
  archiveroot = newarch;

  return 0;
}  /* End of addarchive() */


/***************************************************************************
 * readregexfile:
 *
 * Read a list of regular expressions from a file and combine them
 * into a single, compound expression which is returned in *pppattern.
 * The return buffer is reallocated as need to hold the growing
 * pattern.  When called *pppattern should not point to any associated
 * memory.
 *
 * Returns the number of regexes parsed from the file or -1 on error.
 ***************************************************************************/
static int
readregexfile (char *regexfile, char **pppattern)
{
  FILE *fp;
  char  line[1024];
  char  linepattern[1024];
  int   regexcnt = 0;
  int   newpatternsize;

  /* Open the regex list file */
  if ( (fp = fopen (regexfile, "rb")) == NULL )
    {
      fprintf (stderr, "ERROR opening regex list file %s: %s\n",
	       regexfile, strerror (errno));
      return -1;
    }

  if ( verbose )
    fprintf (stderr, "Reading regex list from %s\n", regexfile);

  *pppattern = NULL;

  while ( (fgets (line, sizeof(line), fp)) !=  NULL)
    {
      /* Trim spaces and skip if empty lines */
      if ( sscanf (line, " %s ", linepattern) != 1 )
	continue;

      /* Skip comment lines */
      if ( *linepattern == '#' )
	continue;

      regexcnt++;

      /* Add regex to compound regex */
      if ( *pppattern )
	{
	  newpatternsize = strlen(*pppattern) + strlen(linepattern) + 4;
	  *pppattern = realloc (*pppattern, newpatternsize);
	  snprintf (*pppattern, newpatternsize, "%s|(%s)", *pppattern, linepattern);
	}
      else
	{
	  newpatternsize = strlen(linepattern) + 3;
	  *pppattern = realloc (*pppattern, newpatternsize);
	  snprintf (*pppattern, newpatternsize, "(%s)", linepattern);
	}
    }

  fclose (fp);

  return regexcnt;
}  /* End readregexfile() */


/***************************************************************************
 * freefilelist:
 *
 * Free all memory assocated with global file list.
 ***************************************************************************/
static void
freefilelist (void)
{
  Filelink *flp, *nextflp;

  flp = filelist;

  while ( flp )
    {
      nextflp = flp->next;
      free (flp);
      flp = nextflp;
    }

  filelist = 0;

  return;
}  /* End of freefilelist() */


/***************************************************************************
 * usage():
 * Print the usage message.
 ***************************************************************************/
static void
usage (int level)
{
  fprintf (stderr, "%s - Modify Mini-SEED data: %s\n\n", PACKAGE, VERSION);
  fprintf (stderr, "Usage: %s [options] file1 [file2] [file3] ...\n\n", PACKAGE);
  fprintf (stderr,
	   " ## Options ##\n"
	   " -V           Report program version\n"
	   " -h           Show this usage message\n"
	   " -H           Show usage message with 'format' details (see -A option)\n"
	   " -v           Be more verbose, multiple flags can be used\n"
	   " -s           Print a basic summary after reading all input files\n"
	   "\n"
	   " ## Data selection options ##\n"
	   " -ts time     Limit to records that start after time\n"
	   " -te time     Limit to records that end before time\n"
           " -tsc time    Limit to records that contain or start after time\n"
	   " -tec time    Limit to records that contain or end before time\n"
	   "                time format: 'YYYY[,DDD,HH,MM,SS,FFFFFF]' delimiters: [,:.]\n"
	   " -M match     Limit to records matching the specified regular expression\n"
	   " -R reject    Limit to records not matchint the specfied regular expression\n"
	   "                Regular expressions are applied to: 'NET_STA_LOC_CHAN_QUAL'\n"
	   "\n"
	   " ## Modification options ##\n"
	   " --net code             Change the network code\n"
	   " --sta code             Change the station code\n"
	   " --loc id               Change the location id\n"
	   " --chan codes           Change the channel codes\n"
	   " --quality [DRQM]       Change the data record indicator/quality code\n"
	   " --timeshift secs       Shift the time base by a specified number of seconds\n"
	   " --timecorr secs        Change the time correction and apply to the time stamp\n"
	   " --timecorrval secs     Change the time correction value (not applied)\n"
	   " --applytimecorr        Apply the time correction if not already applied\n"
	   " --samprate sps         Change the sample rate (both nominal and actual)\n"
           " --actflags 'bit,value' Set or unset an activity flag bit\n"
           " --ioflags 'bit,value'  Set or unset an I/O flag bit\n"
           " --dqflags 'bit,value'  Set or unset a data quality flag bit\n"
/*         " --b100samprate rate    Change the Blockette 100 actual sample rate field\n" */
           " --b1000encoding enc    Change the Blockette 1000 data encoding format field\n"
           " --b1001tqual percent   Change the Blockette 1001 timing quality field (0-100)\n"
           " --cc CCFILENAME         Apply clock correction using params from CCFILENAME. '-H' for details\n"

           "\n"
	   " ## Output options ##\n"
	   " -i           Modify the input files in-place\n"
	   " -o file      Specify a single output file\n"
	   " -A format    Write all records is a custom directory/file layout (try -H)\n"
           "\n"
	   " file#        Files(s) of Mini-SEED records for input\n"
	   "\n");

  if  ( level )
    {
      fprintf (stderr,
               "\n"
	       "  # Preset format layouts #\n"
	       " -CHAN dir    Write all records into separate Net.Sta.Loc.Chan files\n"
	       " -QCHAN dir   Write all records into separate Net.Sta.Loc.Chan.Quality files\n"
	       " -CDAY dir    Write all records into separate Net.Sta.Loc.Chan-day files\n"
	       " -BUD BUDdir  Write all records in a BUD file layout\n"
	       " -CSS CSSdir  Write all records in a CSS-like file layout\n"
	       "\n"
               "The archive 'format' argument is expanded for each record using the\n"
               "following flags:\n"
               "\n"
               "  n : network code, white space removed\n"
               "  s : station code, white space removed\n"
               "  l : location code, white space removed\n"
               "  c : channel code, white space removed\n"
               "  Y : year, 4 digits\n"
               "  y : year, 2 digits zero padded\n"
               "  j : day of year, 3 digits zero padded\n"
               "  H : hour, 2 digits zero padded\n"
               "  M : minute, 2 digits zero padded\n"
               "  S : second, 2 digits zero padded\n"
               "  F : fractional seconds, 4 digits zero padded\n"
               "  q : single character record quality indicator (D, R, Q, M)\n"
               "  L : data record length in bytes\n"
               "  r : Sample rate (Hz) as a rounded integer\n"
               "  R : Sample rate (Hz) as a float with 6 digit precision\n"
               "  %% : the percent (%%) character\n"
               "  # : the number (#) character\n"
               "\n"
               "The flags are prefaced with either the %% or # modifier.  The %% modifier\n"
               "indicates a defining flag while the # indicates a non-defining flag.\n"
               "All records with the same set of defining flags will be written to the\n"
               "same file. Non-defining flags will be expanded using the values in the\n"
               "first record for the resulting file name.\n"
               "\n");
      fprintf (stderr,
               "\n"
	       "  # The clock correction (--cc option) file format is: #\n"
	       " type: {keyword} {parameters}\n"
	       " # Instrument Time     Reference Time\n"
	       " {instrument_time_0}   {reference_time_0}\n"
	       " {instrument_time_1}   {reference_time_1}\n"
	       " ....\n"
               "\n");

    }
}  /* End of usage() */



/**
 * @brief Parses a space-separated string of numbers into an array of doubles.
 *
 * This function takes a string containing space-separated numeric values 
 * (e.g., "1.2 3.4 5.6"), parses them, and returns a dynamically allocated array 
 * of doubles. The number of parsed elements is returned through the `count_out` pointer.
 *
 * Memory is allocated internally for the resulting array and must be freed by the caller.
 *
 * @param input      A null-terminated string containing space-separated double values.
 * @param count_out  Pointer to an integer where the number of parsed doubles will be stored.
 *
 * @return A pointer to a newly allocated array of doubles, or NULL on failure.
 *
 * @note The caller is responsible for freeing the returned array.
 * 
 * @author Ilya Dricker, ISTI
 */
double *parse_doubles(const char *input, int *count_out) 
{
    if (!input || !count_out) return NULL;

    char *copy = strdup(input);
    if (!copy) return NULL;

    // First pass: count tokens
    size_t count = 0;
    char *token = strtok(copy, " ");
    while (token) 
    {
        count++;
        token = strtok(NULL, " ");
    }
    free(copy);

    if (count == 0) 
    {
        *count_out = 0;
        return NULL;
    }

    // Allocate array of exact size
    double *array = malloc(count * sizeof(double));
    if (!array) return NULL;

    // Second pass: parse values
    copy = strdup(input);
    if (!copy) 
    {
        free(array);
        return NULL;
    }

    size_t i = 0;
    token = strtok(copy, " ");
    while (token && i < count) 
    {
        array[i++] = strtod(token, NULL);
        token = strtok(NULL, " ");
    }

    free(copy);
    *count_out = count;
    return array;
}

/**
 * @brief Reads and parses a clock correction configuration file.
 *
 * This function reads a clock correction (CC) configuration file specified by `ccfilename`.
 * It supports multiple correction types (`piecewise_linear`, `cubic-spline`, `polynomial`) and
 * parses timestamp records and (if applicable) polynomial coefficients.
 *
 * The function performs several validations:
 * - Validates format of the file lines.
 * - Ensures timestamps are in strictly increasing order.
 * - Verifies polynomial consistency if the polynomial correction type is used.
 *
 * On success, a populated `ClockCorrConfig` structure is returned, containing:
 * - Correction type
 * - Parsed polynomial coefficients (if applicable)
 * - Instrument and reference time arrays
 *
 * Memory is dynamically allocated for the structure and its members; the caller is responsible for freeing it.
 *
 * @param ccfilename  Path to the clock correction configuration file.
 *
 * @return Pointer to a dynamically allocated `ClockCorrConfig` structure, or NULL on error.
 *
 * @note Lines beginning with '#' are treated as comments and ignored.
 * @note Timestamps must be in increasing order; otherwise, an error is reported.
 * @note Polynomial configuration is validated using `verify_cc_polynomial_config()`.
 *
 * @warning This function allocates memory which must be freed by the caller.
 * 
 * @author Ilya Dricker, ISTI
 */

ClockCorrConfig *read_cc_config(char *ccfilename)
{
#define MAX_LINE_LENGTH 256
#define MAX_TYPE_LENGTH 20

   FILE *fd;
   hptime_t hptime_t_inst, hptime_t_ref;
   char line[MAX_LINE_LENGTH];
   char instTime [MAX_LINE_LENGTH];
   char refTime [MAX_LINE_LENGTH];
   int lineNum = 0;
   short int isValid;

   fd = fopen(ccfilename, "r");                          
   if(!fd)
   {
      fprintf (stderr, "ERROR: opening clock correction parameter file %s\n", ccfilename);
      return NULL;
   }
   cc_config = (ClockCorrConfig *) calloc(sizeof(ClockCorrConfig), 1);	
   if (!cc_config)
   {
      fprintf (stderr, "ERROR: allocating memory for ClockCorrConfig structure\n");
      fclose(fd);
      return NULL;
   }
   cc_config->num_records = 0;  // not really needed but ....

   while (fgets(line, sizeof(line), fd) != NULL) 
   {
      // Remove newline if present
      line[strcspn(line, "\r\n")] = 0;
      lineNum++;

      // Check if line starts with "#"
      if (strncmp(line, "#", 1) == 0) 
      {
         continue;
      }
      // Check if line starts with "type:"
      else if (strncmp(line, "type:", 4) == 0) 
      {
         sscanf(line + 5, " %s", cc_config->type);
         
         // Check if type is in the allowed list
         isValid = strncasecmp(cc_config->type,"piecewise_linear", 16) == 0 ||
                    strncasecmp(cc_config->type,"cubic-spline", 12) == 0 ||
                    strncasecmp(cc_config->type,"polynomial", 10) == 0;

         if (!isValid)
         {
            fprintf (stderr, "ERROR: Badly formatted input file: line %d\n", lineNum);
            return NULL;
         }
         
         if (0 == strncasecmp(cc_config->type,"polynomial", 10))
         {
            /* Fill polynomial coeffs */
            cc_config->coeff = parse_doubles(line + 17, &(cc_config->num_coeffs)); 
            if (!cc_config->coeff)
            {
               fprintf (stderr, "ERROR: Badly formatted input file: line %d\n", lineNum);
               return NULL;
            }
         }
      }                                      
      // Check if line starts with a digit: it is interpreted as time
      else if (isdigit((unsigned char)line[0])) 
      {
         sscanf(line, "%s %s", instTime, refTime);
         if (HPTERROR == (hptime_t_inst = ms_timestr2hptime(instTime)))  
         {
            fprintf(stderr, "ERROR: Failed to convert to hptime_t instrument time %s\n", instTime);
            return NULL;
         }
         if (HPTERROR == (hptime_t_ref = ms_timestr2hptime(refTime)))  
         {
            fprintf(stderr, "ERROR: Failed to convert to hptime_t reference time %s\n", instTime);
            return NULL;
         } 
         cc_config->num_records++;
         if (1 == cc_config->num_records)  
         {
            // Init arrays
            cc_config->inst_time = (hptime_t *) calloc(cc_config->num_records, sizeof(hptime_t));
            cc_config->ref_time = (hptime_t *) calloc(cc_config->num_records, sizeof(hptime_t));
         }  
         else 
         {
            cc_config->inst_time = 
               (hptime_t *) realloc(cc_config->inst_time, cc_config->num_records * sizeof(hptime_t));
            cc_config->ref_time = 
               (hptime_t *) realloc(cc_config->ref_time, cc_config->num_records * sizeof(hptime_t));
         }
         if (!cc_config->inst_time || !cc_config->ref_time)
         {
            fprintf(stderr, "ERROR: failed to allocated memory for time array\n");
            return NULL;
         }
         cc_config->inst_time[cc_config->num_records-1] = hptime_t_inst; 
         cc_config->ref_time[cc_config->num_records-1] = hptime_t_ref;                                  
      }
   }
   
   //Verify that each time column is monotonically increasing
   for (size_t i = 0; i < (cc_config->num_records-1); i++) 
   {
      if (cc_config->inst_time[i+1] <= cc_config->inst_time[i]) 
      {
         fprintf(stderr, "ERROR: Non-increasing instrument times: time line {#%d}\n", (int) i+1);
         return NULL;
      }
      if (cc_config->ref_time[i+1] <= cc_config->ref_time[i]) 
      {
         fprintf(stderr, "ERROR: Non-increasing reference times: time line {#%d}\n", (int) i+1);
         return NULL;
      }  
   }
   
   // If polynomial correction, verify that it 
   //   produces the indicated    "reference_times" when applied 
   //   to the corresponding "instrument_times" (note, the equation
   // is the inverse of that written in the help)
   
   if (verify_cc_polynomial_config(cc_config))
   {
      // Errors -if any - are printed inside the function     
      return NULL;
   }
   
   fclose(fd);
   return cc_config;   
}

/**
 * @brief Applies clock correction to a MiniSEED record based on the given correction configuration.
 *
 * This function adjusts the start time of a MiniSEED record (`MSRecord`) using a correction value
 * determined from a specified `ClockCorrConfig` structure. The correction is calculated based on 
 * the correction method (e.g., polynomial, piecewise linear), and the SEED start time and time 
 * correction fields are updated accordingly.
 *
 * Key operations performed:
 * - Verifies the record's start time falls within the instrument time bounds.
 * - Computes the appropriate time correction.
 * - Applies the correction to the SEED start time.
 * - Sets the SEED time correction field (`fsdh->time_correct`).
 * - Marks the record as having had a time correction applied.
 * - Logs correction details (only once with header).
 *
 * @param cc_confg  Pointer to the clock correction configuration (`ClockCorrConfig`) used to compute correction.
 * @param msr       Pointer to the MiniSEED record (`MSRecord`) whose timing will be corrected.
 *
 * @return 0 on success; non-zero error code on failure (e.g., bounds error, time conversion error).
 *
 * @note The time correction is stored in units of 0.0001 seconds as per SEED format specification.
 * @note Logs only the first correction with a header; subsequent calls append to the log.
 * @note Assumes global `cc_config` is consistent with `cc_confg`.
 *
 * @warning `cc_config` is used inside the function but not passed directly—ensure consistency.
 *
 * @author Ilya Dricker, ISTI
 */
int 
   process_cc(ClockCorrConfig *cc_confg, MSRecord *msr)
   {
      int retVal = 0;
      hptime_t  msr_hptime;
      hptime_t  correction;
      static int print_log_header = 1;
      char orig_time_str[28];
      char corr_time_str[28];

      msr_hptime = ms_btime2hptime(&msr->fsdh->start_time);
      
      // Verify that data times are included in the "Instrument bounds"
      retVal = process_cc_is_in_bounds(msr_hptime, cc_confg);
      if (retVal)
      {
         return retVal;
      }
      
      // Calculate the time correction neeeded, by the selected method
      retVal = process_calc_cc(msr_hptime, cc_config, &correction);
      if (retVal)
      {
         return retVal;
      }

      // Apply this to the Record Start Time field
      retVal = ms_hptime2btime (msr_hptime + correction, &(msr->fsdh->start_time));
      if (retVal)
      {
         fprintf(stderr, "Call to ms_hptime2btime() failed.\n");      
         return retVal;
      }
      
      // Put this value in the Time Correction field
      // The units are in 0.0001 seconds (SEED manual p.109)
      msr->fsdh->time_correct = (long int) correction/100;

      // Set Activity Flag "Time Correction Applied" Bit
      msr->fsdh->act_flags |= (1 << 1);
      
      // Log activity
      if (print_log_header)
      {
      
         ms_log (0, "# RecNo  Instrument time            Corrected to    reference     Corrected-Instrument    Instrument-sync_inst[0]\n");
         print_log_header = 0;
      }
      if (NULL == ms_hptime2isotimestr ( msr_hptime, (char *) orig_time_str, 1))
      {
      
         fprintf (stderr, "ms_hptime2isotimestr failed.");
         return -1;
      }

      if (NULL == ms_hptime2isotimestr ( msr_hptime + correction, (char *)corr_time_str, 1))
      {
      
         fprintf (stderr, "ms_hptime2isotimestr failed.");
         return -1;
      }

      ms_log(0, "%7d  %-27s  %-27s  %12.6f  %24.6f\n", 
          msr->sequence_number,
          orig_time_str,
          corr_time_str,
          (double)correction/1000000,
          (double)(msr_hptime  - cc_config->ref_time[0])/1000000);
          
      return 0;
   }

/**
 * @brief Checks if the MiniSEED record time falls within valid instrument time bounds.
 *
 * This function verifies that the provided high-precision time (`msr_hptime`) lies within 
 * an acceptable range of the instrument times specified in the given clock correction 
 * configuration. A 2-second early margin and a 1-second late margin are permitted to 
 * accommodate minor misalignments.
 *
 * Specifically:
 * - If the data starts more than 2 seconds before the first configured instrument time, 
 *   the function reports an error.
 * - If the data starts more than 1 second after the last configured instrument time, 
 *   the function also reports an error.
 *
 * @param msr_hptime  High-precision time (in 100-nanosecond units) of the record to check.
 * @param cc_confg    Pointer to the clock correction configuration containing time bounds.
 *
 * @return 0 if `msr_hptime` is within acceptable bounds; -1 otherwise.
 *
 * @note Uses global `cc_config` instead of the passed-in `cc_confg`. This may cause confusion or errors 
 *       if the two are inconsistent. Refactoring is recommended for clarity.
 *
 * @warning Time values are compared in 100-nanosecond units (1e-7 seconds).
 * @warning Error messages are printed directly to `stderr`.
 * 
 * @author Ilya Dricker, ISTI
 */
int
   process_cc_is_in_bounds(hptime_t msr_hptime, ClockCorrConfig *cc_confg)
   {
      if (msr_hptime + 2000000 < cc_config->inst_time[0]) // Allow 2 second
      {
         fprintf (stderr, 
           "Data starts before first instrument time (by %.2f seconds).\n", 
            (double) (cc_config->inst_time[0] - msr_hptime)/1000000);
            return -1;            
      }

      if (msr_hptime - 1000000 > cc_config->inst_time[cc_config->num_records-1]) // allow 1 sec
      {
         fprintf (stderr, 
           "Data ends after last instrument time (by %.2f seconds).\n", 
            (double)(msr_hptime - cc_config->inst_time[cc_config->num_records-1])/1000000);
            return -1;
      }

      return 0;
   }

/**
 * @brief Calculates the clock correction for a given time using the specified correction method.
 *
 * This function determines the appropriate time correction to apply to a MiniSEED record 
 * based on the correction type defined in the `ClockCorrConfig` structure.
 *
 * Supported correction types:
 * - `"piecewise_linear"`: Uses linear interpolation between time pairs.
 * - `"cubic-spline"`: Uses cubic spline interpolation.
 * - `"polynomial"`: Evaluates a fitted polynomial over instrument time.
 *
 * The computed correction is returned via the `correction` output parameter.
 *
 * @param msr_hptime   The high-precision time (in 100-nanosecond units) of the MiniSEED record.
 * @param cc_config    Pointer to the clock correction configuration structure.
 * @param correction   Output pointer to store the calculated time correction (in 100-ns units).
 *
 * @return 0 on success; -1 if the correction type is invalid or on internal errors.
 *
 * @note The correction method is chosen based on a case-insensitive match of the `type` field.
 *
 * @warning If the `type` is not recognized, an error is printed to `stderr` and -1 is returned.
 * 
 * @author Ilya Dricker, ISTI
 */   
int 
  process_calc_cc(hptime_t msr_hptime, ClockCorrConfig *cc_config,   hptime_t *correction)
  {
     if (!cc_config)
        return -1;
     if (0 == strncasecmp(cc_config->type,"piecewise_linear", 16))
        return (process_calc_cc_linear(msr_hptime, cc_config,  correction));
     else if (0 == strncasecmp(cc_config->type, "cubic-spline", 12))
        return (process_calc_cc_spline(msr_hptime, cc_config,  correction));
     else if (0 == strncasecmp(cc_config->type, "polynomial", 10))
        return (process_calc_cc_polynomial(msr_hptime, cc_config,  correction));
     else
     {
        fprintf(stderr, "ERROR: clock correction <type> %s is not valid\n", cc_config->type);
        return -1;
     }
     return 0;   
   }
      
/**
 * Perform piecewise linear time correction.
 * 
 * @param msr_hptime The time to correct.
 * @param cc_config->inst_time Array of instrument timestamps.
 * @param cc_config->ref_time Array of corresponding reference timestamps.
 * @param cc_config->num_records Number of points in the arrays.
 * @return via pointer Time correction.
 * @retVal 0 in case of success; -1 in case of failure
 * 
 * @author Ilya Dricker, ISTI
 */   
int 
  process_calc_cc_linear(hptime_t msr_hptime, ClockCorrConfig *cc_config,   hptime_t *correction)
{
    if (cc_config->num_records < 2) 
    {
        fprintf(stderr, "Need at least 2 points in linear time correction.\n");
        return -1;
    }

    for (size_t n = 0; n < cc_config->num_records - 1; n++) 
    {
        hptime_t t0 = cc_config->inst_time[n];
        hptime_t t1 = cc_config->inst_time[n+1];

        if (msr_hptime >= t0 && msr_hptime <= t1) 
        {
//            double dT = (double) (msr_hptime - t0);
            double dT = (double) (msr_hptime - cc_config->inst_time[n]);

            double inst_delta =  (t1 - t0);
            double ref_delta =  (cc_config->ref_time[n+1] - cc_config->ref_time[n]);
            double coeff_a=cc_config->ref_time[n]-cc_config->inst_time[n];

            // Avoid division by zero
            if (inst_delta == 0.0) {
                fprintf(stderr, "Warning: cc_config->inst_time[%zu] == cc_config->inst_time[%zu+1]\n", n, n);
                *correction = 0;
                return 0;
            }

             *correction = (hptime_t)(dT * (ref_delta / inst_delta) - dT + coeff_a);

            return 0;
        }
    }

    // If we reach here, msr_hptime is outside the known intervals
    fprintf(stderr, "Time in MSEED header is out of interpolation bounds.\n");
    return -1;
}   


/**
 * @brief Computes clock correction using cubic spline interpolation.
 *
 * This function calculates the time correction for a given instrument timestamp 
 * (`msr_hptime`) using cubic spline interpolation between the configured 
 * instrument and reference time pairs in `ClockCorrConfig`.
 *
 * The difference between instrument and reference times (in seconds) is modeled as a smooth spline curve.
 * The function evaluates the spline at the target time to determine the correction to apply.
 *
 * @param msr_hptime   High-precision instrument time (in 100-nanosecond units) for which correction is computed.
 * @param cc_config    Pointer to the clock correction configuration structure containing input time pairs.
 * @param correction   Output pointer where the calculated time correction will be stored (in 100-ns units).
 *
 * @return 0 on success; -1 on error (e.g., invalid input, memory allocation failure, spline error).
 *
 * @note Time correction is calculated as: `correction = reference_time - instrument_time`.
 * @note Internally, all calculations are performed in seconds with microsecond precision.
 * @note This function dynamically allocates and frees spline coefficient arrays and temporary vectors.
 *
 * @warning Requires at least two time records in `cc_config`. 
 * @warning Logs errors to `stderr` on failure conditions.
 *
 * @author Ilya Dricker, ISTI
 */     
int 
  process_calc_cc_spline(hptime_t msr_hptime, ClockCorrConfig *cc_config,   hptime_t *correction)
{
   if (!cc_config || !cc_config->inst_time || !cc_config->ref_time || cc_config->num_records < 2)
   {
      fprintf(stderr, "Invalid ClockCorrConfig for spline correction.\n");
      return -1;
   }

   size_t n = (size_t)cc_config->num_records;
   double *x = malloc(n * sizeof(double));
   double *y = malloc(n * sizeof(double));
   if (!x || !y)
   {
      fprintf(stderr, "Memory allocation failed.\n");
      free(x); free(y);
      return -1;
   }

   hptime_t t0 = cc_config->inst_time[0];

   for (size_t i = 0; i < n; i++)
   {
      x[i] = (double)(cc_config->inst_time[i] - t0) / 1e6;  // seconds
      y[i] = (double)(cc_config->ref_time[i] - cc_config->inst_time[i]) / 1e6;  // ref - inst
   }

   double dT = (double)(msr_hptime - t0) / 1e6;

   CubicSpline spline;
   if (build_cubic_spline(x, y, n, &spline) != 0)
   {
      fprintf(stderr, "Spline construction failed.\n");
      free(x); free(y);
      return -1;
   }

   double delta_sec = evaluate_spline(&spline, dT);
   *correction = (hptime_t)(delta_sec * 1e6);  // convert back to microseconds

   // Cleanup
    free(x); free(y);
    free(spline.x); free(spline.a); free(spline.b); free(spline.c); free(spline.d);
    
    return 0;
} 

/**
 * @brief Perform polynomial time correction.
 *
 * This function applies a polynomial clock correction model to a given
 * reference time to estimate the instrument time. The correction model is:
 *
 *     instrument_time = reference_time + a0 + a1*dT + a2*dT^2 + ...
 *
 * where:
 *   - dT = reference_time - reference_time[0]
 *   - a0, a1, ..., an are the polynomial coefficients provided in the configuration
 *
 * @param ref_hptime   The reference time to correct (in hptime_t, microseconds since epoch).
 * @param cc_config    Pointer to ClockCorrConfig containing coefficients and reference_time[0].
 * @param correction   Pointer to output variable receiving the computed correction (in hptime_t).
 *
 * @return 0 on success, -1 on failure (e.g., invalid config or missing reference time).
 */
int 
  process_calc_cc_polynomial(hptime_t msr_hptime, ClockCorrConfig *cc_config,   hptime_t *correction)
{
   if (!cc_config || !cc_config->coeff || cc_config->num_coeffs < 1)   
   {
      fprintf(stderr, "Invalid polynomial configuration.\n");
      return -1;
   }

   if (!cc_config->ref_time) 
   {
      fprintf(stderr, "ref_time[0] required to calculate dT.\n");
      return -1;
   }

   double dT = (double)(msr_hptime - cc_config->inst_time[0]) / 1e6;

   double poly_correction = 0.0;

   for (size_t i = 0; i < cc_config->num_coeffs; i++) 
   {
      poly_correction += cc_config->coeff[i] * pow(dT, i);
   }

   *correction = (hptime_t)(-1 * poly_correction *1e6);  // result in microseconds
   return 0;   
}

/**
 * @brief Verifies that the polynomial clock correction configuration accurately reproduces reference times.
 *
 * This function checks the validity of a polynomial clock correction by applying the polynomial
 * correction to each instrument time in the configuration and comparing the result with the
 * corresponding reference time. If the difference between the corrected instrument time and the
 * reference time exceeds a small threshold (1000 microseconds), an error is reported.
 *
 * If the configuration is not of type `"polynomial"`, the function immediately returns success (0).
 *
 * Detailed error messages include original instrument time, reference time, corrected time, and
 * their differences, formatted as ISO timestamps.
 *
 * @param cc_config  Pointer to the `ClockCorrConfig` structure containing polynomial coefficients and times.
 *
 * @return 0 if all corrected times match reference times within the threshold; -1 otherwise.
 *
 * @note Uses a threshold of 1000 microseconds (1 millisecond) for allowable deviation.
 * @note Conversion of times to ISO strings is used for detailed error reporting.
 * @note On error, detailed diagnostic information is printed to `stderr`.
 * 
 * @warning Relies on `process_calc_cc_polynomial()` for correction calculations.
 * @warning Errors and diagnostics are output to standard error.
 * 
 * @author Ilya Dricker, ISTI
 */
int 
 verify_cc_polynomial_config(ClockCorrConfig *cc_config)
{

   hptime_t correction;
   int retVal;
   int i;
   int SMALL = 1000; // In microseconds
   char orig_time_str[28];
   char corr_time_str[28];
   char ref_time_str[28];
  
   
   // If it is not polynomial stop testing, return Success
   if (0 != strncasecmp(cc_config->type, "polynomial", 10))
      return 0;
   
   for (i=0; i< cc_config->num_records; i++)
   {
      retVal = process_calc_cc_polynomial(cc_config->inst_time[i], cc_config,  &correction);      
      if (retVal)
      {
         fprintf(stderr, "ERROR: Polynomial does not generate reference corrected times\n");
         fprintf(stderr, "ERROR: process_calc_cc_polynomial() failed\n");
         return -1;
      }
      // Check results
      if (abs(cc_config->ref_time[i] - correction - cc_config->inst_time[i]) > SMALL)
      {
         if (NULL == ms_hptime2isotimestr ( cc_config->inst_time[i], (char *) orig_time_str, 1))
         {
      
            fprintf (stderr, "ms_hptime2isotimestr failed.");
            return -1;
         }

         if (NULL == ms_hptime2isotimestr ( cc_config->inst_time[i] + correction, (char *)corr_time_str, 1))
         {
      
            fprintf (stderr, "ms_hptime2isotimestr failed.");
            return -1;
         }

         if (NULL == ms_hptime2isotimestr ( cc_config->ref_time[i], (char *)ref_time_str, 1))
         {
      
            fprintf (stderr, "ms_hptime2isotimestr failed.");
            return -1;
         }


         fprintf(stderr, "ERROR: Polynomial does not generate reference corrected times\n");
         fprintf(stderr, "INSTRUMENT_TIME             |   REFERENCE_TIME            |    CORRECTED_TIME           | CORRECTED-REFERENCE (s)\n");
         fprintf(stderr, "--------------------------- | --------------------------- | --------------------------- | -----------------------\n");
         fprintf(stderr, "%-27s | %-27s | %-27s | %10.6f\n", 
            orig_time_str,
            ref_time_str,
            corr_time_str,
           (double)correction/1000000);
         
         return -1;
      }
      
   }
   return 0;            
}

/**
 * @brief Constructs a cubic spline interpolation from given data points.
 *
 * This function computes the coefficients of a cubic spline that smoothly interpolates 
 * the given data points `(x[i], y[i])` for `i = 0 ... n-1`. The resulting spline is stored
 * in the provided `CubicSpline` structure.
 *
 * The spline coefficients a, b, c, and d correspond to the polynomial segments between each 
 * pair of points, enabling evaluation of the spline at any point within the domain.
 *
 * The algorithm implemented is the standard cubic spline construction using the 
 * tridiagonal system solver for second derivatives.
 *
 * @param x       Array of x-coordinates (must be strictly increasing), length n.
 * @param y       Array of y-coordinates, length n.
 * @param n       Number of data points.
 * @param spline  Pointer to a `CubicSpline` struct where the spline data will be stored.
 *
 * @return 0 on success; -1 if memory allocation fails.
 *
 * @note The caller is responsible for freeing the allocated arrays in the `CubicSpline` struct.
 * @note The function assumes `x` values are sorted in ascending order.
 */
static int build_cubic_spline(const double *x, const double *y, size_t n, CubicSpline *spline)
{
    spline->n = n;
    spline->x = malloc(n * sizeof(double));
    spline->a = malloc(n * sizeof(double));
    spline->b = malloc((n - 1) * sizeof(double));
    spline->c = malloc(n * sizeof(double));
    spline->d = malloc((n - 1) * sizeof(double));
    if (!spline->x || !spline->a || !spline->b || !spline->c || !spline->d)
        return -1;

    for (size_t i = 0; i < n; i++) 
    {
        spline->x[i] = x[i];
        spline->a[i] = y[i];
    }

    double *h = malloc((n - 1) * sizeof(double));
    double *alpha = malloc((n - 1) * sizeof(double));
    double *l = malloc(n * sizeof(double));
    double *mu = malloc(n * sizeof(double));
    double *z = malloc(n * sizeof(double));
    if (!h || !alpha || !l || !mu || !z)
        return -1;

    for (size_t i = 0; i < n - 1; i++)
        h[i] = x[i+1] - x[i];

    for (size_t i = 1; i < n - 1; i++)
        alpha[i] = (3.0 / h[i]) * (y[i+1] - y[i]) - (3.0 / h[i-1]) * (y[i] - y[i-1]);

    l[0] = 1.0; mu[0] = 0.0; z[0] = 0.0;
    for (size_t i = 1; i < n - 1; i++) 
    {
        l[i] = 2.0 * (x[i+1] - x[i-1]) - h[i-1] * mu[i-1];
        mu[i] = h[i] / l[i];
        z[i] = (alpha[i] - h[i-1] * z[i-1]) / l[i];
    }

    l[n-1] = 1.0; z[n-1] = 0.0; spline->c[n-1] = 0.0;
    for (ssize_t j = n - 2; j >= 0; j--) 
    {
        spline->c[j] = z[j] - mu[j] * spline->c[j+1];
        spline->b[j] = (spline->a[j+1] - spline->a[j]) / h[j]
                       - h[j] * (spline->c[j+1] + 2.0 * spline->c[j]) / 3.0;
        spline->d[j] = (spline->c[j+1] - spline->c[j]) / (3.0 * h[j]);
    }

    free(h); free(alpha); free(l); free(mu); free(z);
    return 0;
}
/**
 * @brief Evaluates the cubic spline at a given x-value.
 *
 * This function finds the appropriate interval within the spline's domain that contains `xval`,
 * and evaluates the cubic polynomial defined by the spline coefficients at that point.
 *
 * @param spline  Pointer to the `CubicSpline` structure containing the spline coefficients and nodes.
 * @param xval    The x-value at which to evaluate the spline.
 *
 * @return The interpolated y-value corresponding to `xval`.
 *
 * @note If `xval` is outside the spline's range, the last interval is used for extrapolation.
 * @note The spline must have at least two points (`spline->n >= 2`).
 */
static double evaluate_spline(const CubicSpline *spline, double xval)
{
    size_t i = spline->n - 2;
    for (size_t j = 0; j < spline->n - 1; j++) 
    {
        if (xval >= spline->x[j] && xval <= spline->x[j+1]) 
        {
            i = j;
            break;
        }
    }
    double dx = xval - spline->x[i];
    return spline->a[i] + spline->b[i]*dx + spline->c[i]*dx*dx + spline->d[i]*dx*dx*dx;
}

