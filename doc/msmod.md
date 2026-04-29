# Mini-SEED Modifier - perform stream-based Mini-SEED modifications

1. [Synopsis](#synopsis)
1. [Description](#description)
1. [Options](#options)
1. [Clock Correction Input File Format](#clock-correction-input-file-format)
1. [Clock Correction Input File Examples](#clock-correction-input-file-examples)
1. [Archive Format](#archive-format)
1. [Archive Format Examples](#archive-format-examples)
1. [Author](#author)

## <a id="synopsis">Synopsis</a>

```
msmod [options] file1 [file2 file3 ...]
```

## <a id="description">Description</a>

<b>msmod</b> is a general purpose Mini-SEED modification processor. Mini-SEED records are read from each input file and modified as specified and written back out in a serial-manner.  Each modification is applied to all processed records.

If '-' is specified standard input will be read.  Multiple input files will be processed in the order specified.

The header of each record processed is repacked regardless of selected modifications, this repacking includes normalization of the sampling rate fields (e.g. a factor of 32760 and multiplier of -819 will be converted to a factor of 40 and multiplier of 1).  The repacking might also change the offsets to any included blockettes if they were not packed in tight sequence, the order of the blockettes will be preserved.

When a input file is full SEED including both SEED headers and data records all of the headers will be skipped and completely unprocessed.

## <a id="options">Options</a>

- <b>-V</b>
  Print program version and exit.

- <b>-h/-H</b>
  Print program usage and exit, if -H is specified additional details regarding output to user-defined archive structures (see -A).

- <b>-v</b>
  Be more verbose.  This flag can be used multiple times ("-v -v" or "-vv") for more verbosity.

- <b>-s</b>
  Print a basic summary including the number of records and the number of samples they included after processing all input records.

- -ts <i>time</i>
  Limit processing to Mini-SEED records that start after <i>time</i>. The format of the <i>time</i> argument is: 'YYYY[,DDD,HH,MM,SS,FFFFFF]' where valid delimiters are either commas (,), colons (:) or periods (.).

- -te <i>time</i>
  Limit processing to Mini-SEED records that end before <i>time</i>. The format of the <i>time</i> argument is: 'YYYY[,DDD,HH,MM,SS,FFFFFF]' where valid delimiters are either commas (,), colons (:) or periods (.).

- -tsc <i>time</i>
  Limit processing to Mini-SEED records that contain or start after <i>time</i>.  The format of the <i>time</i> argument is: 'YYYY[,DDD,HH,MM,SS,FFFFFF]' where valid delimiters are either commas (,), colons (:) or periods (.).

- -tec <i>time</i>
  Limit processing to Mini-SEED records that contain or end before <i>time</i>.  The format of the <i>time</i> argument is: 'YYYY[,DDD,HH,MM,SS,FFFFFF]' where valid delimiters are either commas (,), colons (:) or periods (.).

- -M <i>match</i>
  Limit processing to Mini-SEED records that match the <i>match</i> regular expression.  For each input record a source name string composed of 'NET_STA_LOC_CHAN_QUAL' is created and compared to the regular expression.

- -R <i>reject</i>
  Limit processing to Mini-SEED records that do not match the <i>reject</i> regular expression.  For each input record a source name string composed of 'NET_STA_LOC_CHAN_QUAL' is created and compared to the regular expression.

- <b>-i</b>
  Modify input records in-place by writing modified records back to the input files.

- -o <i>outfile</i>
  Write all processed Mini-SEED records to <i>outfile</i>.

- -A <i>format</i>
  All output records will be written to a directory/file layout defined by <i>format</i>.  All directories implied in the <i>format</i> string will be created if necessary.  The option may be used multiple times to write input records to multiple archives.  See the \fBArchive Format\fP section below.

- <b>--net code</b>
  Specify a new network code.

- <b>--sta code</b>
  Specify a new station code.

- <b>--loc id</b>
  Specify a new location id.

- <b>--chan codes</b>
  Specify new channel codes.  A dot (.) will be interpreted as the same character as the input channel name, for example, "L.." can be specified to only replace the first code with 'L' and leave the other two codes as they are.

- <b>--quality [DRMQ]</b>
  Specify a new header/quality identifier, can be either 'D', 'R', 'M', or 'Q'.

- <b>--timeshift secs</b>
  Specify a time shift in seconds to apply to record start times.  This value can be positive or negative and is not stored in the record as a time correction.

- <b>--timecorr secs</b>
  Change the value of the time correction field in the headers and apply the correction to the record start times.  This is similar to <i>--timecorr</i> except that the correction is stored in the header and bit 1 of the activity flags (field 12) will be set to indicate the correction has been applied.

- <b>--timecorrval secs</b>
  Change the value of the time correction field in the headers without applying the change to the record start times.  Bit 1 of the activity flags (field 12) will not be changed.

- <b>--applytimecorr</b>
  Apply the time correction value to the record start time if it has not previously been applied and set bit 1 of the activity flags (field 12) to indicate the correction has been applied.

- <b>--samprate sps</b>
  Specify new sampling rate in samples per second.  This will be applied to both the nominal and actual (if present) sampling rate fields.

- <b>--actflags bit,value</b>
  Specify an activity flag by indicating the bit to change and it's value.  Valid bit ranges are 0 to 6 and valid bit values are 0 and 1. Activity flags are defined in the SEED 2.4 manual as follows:

  ```
    \fB[Bit 0]\fP : Calibration signals preset
    \fB[Bit 1]\fP : Time correction applied
    \fB[Bit 2]\fP : Beginning of an event, station trigger
    \fB[Bit 3]\fP : End of the event, station detriggers
    \fB[Bit 4]\fP : A positive leap second happended during this record
    \fB[Bit 5]\fP : A negative leap second happended during this record
    \fB[Bit 6]\fP : Event in progress
  ```

- <b>--ioflags bit,value</b>
  Specify an I/O flag by indicating the bit to change and it's value. Valid bit ranges are 0 to 5 and valid bit values are 0 and 1.  I/O flags are defined in the SEED 2.4 manual as follows:

  ```
    \fB[Bit 0]\fP : Station volume parity error possibly present
    \fB[Bit 1]\fP : Long record read (possibly no problem)
    \fB[Bit 2]\fP : Short record read (record padded)
    \fB[Bit 3]\fP : Start of time series
    \fB[Bit 4]\fP : End of time series
    \fB[Bit 5]\fP : Clock locked
  ```

- <b>--dqflags bit,value</b>
  Specify a data quality flag by indicating the bit to change and it's value.  Valid bit ranges are 0 to 7 and valid bit values are 0 and 1. Data quality flags are defined in the SEED 2.4 manual as follows:

  ```
    \fB[Bit 0]\fP : Amplifier saturation detected (station dependent)
    \fB[Bit 1]\fP : Digitizer clipping detected
    \fB[Bit 2]\fP : Spikes detected
    \fB[Bit 3]\fP : Glitches detected
    \fB[Bit 4]\fP : Missing/padded data preset
    \fB[Bit 5]\fP : Telementry synchronization error
    \fB[Bit 6]\fP : A digital filter may be charging
    \fB[Bit 7]\fP : Time tag is questionable
  ```

- <b>--b1000encoding encoding</b>
  Chanage the Blockette 1000 data encoding format field.  Valid values are included in the SEED manual documentation for Blockette 1000.

- <b>--b1001tqual percent</b>
  Chanage the Blockette 1001 timing quality field, valid values are 0 to 100 percent.  Further description is included in the SEED manual documentation for Blockette 1001.

- <b>--cc CCFILENAME</b>
  Apply clock correction using parameters from CCFILENAME. The option sets timecorrection and modifies starttime in every record according to a clock drift specified in the clock correction input file. The clock correction input file format is described in a section below.

  Clock correction option logging is implemented via ms_log() function. For each Modified mini-SEED record, the following information is logged in the columns:

  ```
  	       RecNo: the record number
  	       Instrument time: original instrument time (ISO8601)
  	       Corrected to reference: corrected time (ISO8601)
  	       Corrected-Instrument: corrected time minus instrument time (s)
  	       Instrument-sync_inst[0]: instrument time - instrument_time_0 (s)
  ```

## <a id="clock-correction-input-file-format">Clock Correction Input File Format</a>

Clock correction input file format is:

```
\fB
	       type: {type_value}
	       {instrument_time_0}   {reference_time_0}
	       {instrument_time_1}   {reference_time_1}
	       ....
\fP
```

The times in each column must monotonically increase and the {instrument_time}s must cover the time range of the miniSEED file(s). {*_time_*} format is yyyy-mm-ddTHH:MM:SS(.FFFFF)Z. Comment lines start with '#' and have no effect on processing. Possible {type value}s  are:

```
  type: \fBpiecewise_linear\fP
	       shifts instrument_time to reference_time for each provided value,
	       linearly interpolates in between
  type: \fBcubic_spline\fP
	       shifts instrument_time to reference_time for each provided value,
	       cubic spline interpolation in between
  type: \fBpolynomial\fP a0 a1 a2 a3...
	       sets corrected_time = instrument_time_0 + a0 + a1*delta + a2*delta**2 + ...,
	       where delta = instrument_time - instrument_time_0.
               {instrument_time_n} and {reference_time_n} are used to validate results
```

## <a id="clock-correction-input-file-examples">Clock Correction Input File Examples</a>

```
type: cubic_spline
# Instrument time        Reference time
2022-01-01T00:00:00Z     2022-01-01T00:00:00Z
2022-06-01T00:00:00Z     2022-06-01T00:00:00.1Z
2023-01-01T00:00:00Z     2023-01-01T00:00:01.5Z

type: polynomial 0.001 3.38e-9 1.4e-15
# Instrument time        Reference time
2022-01-01T00:00:00Z     2022-01-01T00:00:00.001Z
2022-07-01T00:00:00Z     2022-07-01T00:00:00.396Z
2023-01-01T00:00:00Z     2023-01-01T00:00:01.500Z
```

## <a id="archive-format">Archive Format</a>

An archive format is expanded for each record using the following substitution flags:

```
  \fBn\fP : network code, white space removed
  \fBs\fP : station code, white space removed
  \fBl\fP : location code, white space removed
  \fBc\fP : channel code, white space removed
  \fBY\fP : year, 4 digits
  \fBy\fP : year, 2 digits zero padded
  \fBj\fP : day of year, 3 digits zero padded
  \fBH\fP : hour, 2 digits zero padded
  \fBM\fP : minute, 2 digits zero padded
  \fBS\fP : second, 2 digits zero padded
  \fBF\fP : fractional seconds, 4 digits zero padded
  \fBq\fP : single character record quality indicator (D, R, Q)
  \fBL\fP : data record length in bytes
  \fBr\fP : sample rate (Hz) as a rounded integer
  \fBR\fP : sample rate (Hz) as a float with 6 digit precision
  \fB%\fP : the percent (%) character
  \fB#\fP : the number (#) character
```

The flags are prefaced with either the '%' or '#' modifier.  The '%' modifier indicates a defining flag while the '#' indicates a non-defining flag.  All records with the same set of defining flags will be written to the same file.  Non-defining flags will be expanded using the values in the first record for the resulting file name.

Time flags are based on the start time of the given record.

## <a id="archive-format-examples">Archive Format Examples</a>

The format string for the predefined <i>BUD</i> layout:

<b>/archive/%n/%s/%s.%n.%l.%c.%Y.%j</b>

would expand to day length files named something like:

<b>/archive/NL/HGN/HGN.NL..BHE.2003.055</b>

As an example of using non-defining flags the format string for the predefined <i>CSS</i> layout:

<b>/data/%Y/%j/%s.%c.%Y:%j:#H:#M:#S</b>

would expand to:

<b>/data/2003/055/HGN.BHE.2003:055:14:17:54</b>

resulting in day length files because the hour, minute and second are specified with the non-defining modifier.  The hour, minute and second fields are from the first record in the file.

## <a id="author">Author</a>

```
Chad Trabant
IRIS Data Management Center
```

---

*Generated from man page dated 2018/06/27.*
