/***************************************************************************
 * msmod.c - Mini-SEED modifier.
 *
 * Opens user specified file(s), parses records, makes specified
 * modifications and writes the data back out.
 *
 * Written by Chad Trabant, IRIS Data Management Center.
 *
 * modified 2006.279
 ***************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <regex.h>

#include <libmseed.h>

#include "dsarchive.h"

#define VERSION "0.1"
#define PACKAGE "msmod"

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

static int processmods (MSRecord *msr);
static int processparam (int argcount, char **argvec);
static char *getoptval (int argcount, char **argvec, int argopt);
static void addfile (char *filename);
static int lisnumber (char *number);
static int  addarchive(const char *path, const char *layout);
static int readregexfile (char *regexfile, char **pppattern);
static void freefilelist (void);
static void usage (int level);

static flag     verbose       = 0;
static flag     basicsum      = 0;    /* Controls printing of basic summary */
static int      reclen        = -1;   /* Input data record length, autodetected in most cases */
static hptime_t starttime     = HPTERROR;  /* Limit to records after starttime */
static hptime_t endtime       = HPTERROR;  /* Limit to records before endtime */
static regex_t *match         = 0;    /* Compiled match regex */
static regex_t *reject        = 0;    /* Compiled reject regex */
static char    *outputfile    = 0;    /* Single output file */
static Archive *archiveroot   = 0;    /* Output file structures */
static Filelink *filelist = 0;

/* Modification specifiers */
static double   modtimedelta  = 0;
static char    *modnet        = 0;
static char    *modsta        = 0;
static char    *modloc        = 0;
static char    *modchan       = 0;

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

  char basesrc[50];
  char srcname[50];
  char stime[30];

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
                  msr_srcname (msr, srcname);
                  ms_hptime2seedtimestr (msr->starttime, stime);
                  fprintf (stderr, "Skipping (starttime) %s, %s\n", srcname, stime);
                }
              continue;
            }
          
          if ( endtime != HPTERROR && (msr_endtime(msr) > endtime) )
            {
              if ( verbose >= 3 )
                {
                  msr_srcname (msr, srcname);
                  ms_hptime2seedtimestr (msr->starttime, stime);
                  fprintf (stderr, "Skipping (starttime) %s, %s\n", srcname, stime);
                }
              continue;
            }
          
          if ( match || reject )
            {
              /* Generate the srcname and add the quality code */
              msr_srcname (msr, basesrc);
              snprintf (srcname, sizeof(srcname), "%s_%c", basesrc, msr->dataquality);
              
              /* Check if record is matched by the match regex */
              if ( match )
                {
                  if ( regexec ( match, srcname, 0, 0, 0) != 0 )
                    {
                      if ( verbose >= 3 )
                        {
                          ms_hptime2seedtimestr (msr->starttime, stime);
                          fprintf (stderr, "Skipping (match) %s, %s\n", srcname, stime);
                        }
                      continue;
                    }
                }
              
              /* Check if record is rejected by the reject regex */
              if ( reject )
                {
                  if ( regexec ( reject, srcname, 0, 0, 0) == 0 )
                    {
                      if ( verbose >= 3 )
                        {
                          ms_hptime2seedtimestr (msr->starttime, stime);
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
	  
	  /* Perform modifications to record header */
	  if ( processmods (msr) ) 
	    {
	      fprintf (stderr, "ERROR modifying:\n  ");
	      msr_print (msr, verbose-1);
	      stopflag = 1;
	      break;
	    }

	  /* Repack header into record */
	  if ( msr_pack_header (msr, verbose-1) < 0 )
	    {
	      fprintf (stderr, "ERROR packing header for:\n  ");
	      msr_print (msr, verbose-1);
	      stopflag = 1;
	      break;
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
        fprintf (stderr, "Error reading %s: %s\n",
                 flp->filename, get_errorstr(retcode));
      
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
 * Process all specified modifcations on the specified MSRecord.
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
      strncpy (msr->channel, modchan, sizeof(msr->channel));
    }
  
  /* Modify start time */
  if ( modtimedelta )
    {
      if ( verbose > 1 )
	fprintf (stderr, "Shifting record starttime by %g seconds\n", modtimedelta);
      
      if ( verbose && msr->fsdh->time_correct && ! (msr->fsdh->act_flags & 0x02) )
	fprintf (stderr, "Warning, applying time correction over another time correction\n");
      
      /* Set the time correction applied flag (bit 1 of the activitiy flags) */
      msr->fsdh->act_flags |= 0x02;
      
      /* Set the time correction field, value is units of 0.0001 seconds */
      msr->fsdh->time_correct = modtimedelta * 10000;
      
      /* Apply time shift to starttime */
      msr->starttime += (hptime_t) (modtimedelta * HPTMODULUS);
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
  char *tptr;
  
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
      else if (strcmp (argvec[optind], "-sum") == 0)
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
      else if (strcmp (argvec[optind], "-M") == 0)
	{
	  matchpattern = getoptval(argcount, argvec, optind++);
	}
      else if (strcmp (argvec[optind], "-R") == 0)
	{
	  rejectpattern = getoptval(argcount, argvec, optind++);
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
      else if (strcmp (argvec[optind], "--timedelta") == 0)
        {
	  modtimedelta = strtod (getoptval(argcount, argvec, optind++) ,NULL);
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
  
  /* Special case of '--timedelta -X' */
  if ( (argopt+1) < argcount && strcmp (argvec[argopt], "--timedelta") == 0 )
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
 * Test if the string is all digits allowing an initial minus sign.
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

      if ( ! isdigit ((int) *(number+idx)) )
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
	   " -sum         Print a basic summary after reading all input files\n"
	   "\n"
	   " ## Data selection options ##\n"
	   " -ts time     Limit to records that start after time\n"
	   " -te time     Limit to records that end before time\n"
	   "                time format: 'YYYY[,DDD,HH,MM,SS,FFFFFF]' delimiters: [,:.]\n"
	   " -M match     Limit to records matching the specified regular expression\n"
	   " -R reject    Limit to records not matchint the specfied regular expression\n"
	   "                Regular expressions are applied to: 'NET_STA_LOC_CHAN_QUAL'\n"
	   "\n"
	   " ## Modification options ##\n"
	   " --timedelta secs  Shift the time base by a specified number of seconds\n"
	   " --net code        Change the network code\n"
	   " --sta code        Change the station code\n"
	   " --loc id          Change the location id\n"
	   " --chan codes      Change the channel codes\n"
           "\n"
	   " ## Output options ##\n"
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
               "  q : single character record quality indicator (D, R, Q)\n"
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
    }
}  /* End of usage() */
