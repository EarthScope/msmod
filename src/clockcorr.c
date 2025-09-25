/***************************************************************************
 * clockcorr.c - Functions and structures related to clock correction option ms msmod.
 *
 * Clock correction (-cc) option is described in msmod man page, -h, -H options of msmod program
 * Specs of the option is provided here:
 * https://github.com/FDSN/OBS-standards/blob/main/other/msmod_drift_addition.md
 *
 * Written by Ilya Dricker (2025), ISTI.
 ***************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <math.h>

#include <libmseed.h>

#include "clockcorr.h"


typedef struct {
    size_t n;
    double *x;
    double *a;
    double *b;
    double *c;
    double *d;
} CubicSpline;

 int  process_cc(ClockCorrConfig *cc_confg, MSRecord *msr);
 ClockCorrConfig *read_cc_config(char *ccfilename);
static double *parse_doubles(const char *input, int *count_out);
static int process_cc_is_in_bounds(hptime_t msr_hptime, ClockCorrConfig *cc_confg);
static int process_calc_cc(hptime_t msr_hptime, ClockCorrConfig *cc_config, hptime_t *correction);
static int process_calc_cc_linear(hptime_t msr_hptime, ClockCorrConfig *cc_config,   hptime_t *correction);
static int process_calc_cc_spline(hptime_t msr_hptime, ClockCorrConfig *cc_config,   hptime_t *correction);
static int process_calc_cc_polynomial(hptime_t msr_hptime, ClockCorrConfig *cc_config,   hptime_t *correction);
static int  verify_cc_polynomial_config(ClockCorrConfig *cc_config);
static int build_cubic_spline(const double *x, const double *y, size_t n, CubicSpline *spline);
static double evaluate_spline(const CubicSpline *spline, double xval);

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
 * It supports multiple correction types (`piecewise_linear`, `cubic_spline`, `polynomial`) and
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
                    strncasecmp(cc_config->type,"cubic_spline", 12) == 0 ||
                    strncasecmp(cc_config->type,"polynomial", 10) == 0;

         if (!isValid)
         {
            fprintf (stderr, "ERROR: unknown type %s in config: line %d\n", cc_config->type, lineNum);
            return NULL;
         }
         
         if (0 == strncasecmp(cc_config->type,"polynomial", 10))
         {
            /* Fill polynomial coeffs */
            cc_config->coeff = parse_doubles(line + 17, &(cc_config->num_coeffs)); 
            if (!cc_config->coeff)
            {
               fprintf (stderr, "ERROR: improperly formatted polynomial coefficients: line %d\n", lineNum);
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
            fprintf(stderr, "ERROR: failed to convert to hptime_t instrument time %s\n", instTime);
            return NULL;
         }
         if (HPTERROR == (hptime_t_ref = ms_timestr2hptime(refTime)))  
         {
            fprintf(stderr, "ERROR: failed to convert to hptime_t reference time %s\n", instTime);
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
         fprintf(stderr, "ERROR: non-increasing instrument times: time line {#%d}\n", (int) i+1);
         return NULL;
      }
      if (cc_config->ref_time[i+1] <= cc_config->ref_time[i]) 
      {
         fprintf(stderr, "ERROR: non-increasing reference times: time line {#%d}\n", (int) i+1);
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
 * - `"cubic_spline"`: Uses cubic spline interpolation.
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
     else if (0 == strncasecmp(cc_config->type, "cubic_spline", 12))
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
         fprintf(stderr, "ERROR: polynomial does not generate reference corrected times\n");
         fprintf(stderr, "ERROR: process_calc_cc_polynomial() failed\n");
         return -1;
      }
      // Check results
      if (llabs(cc_config->ref_time[i] - correction - cc_config->inst_time[i]) > SMALL)
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


         fprintf(stderr, "ERROR: polynomial does not generate reference corrected times\n");
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
