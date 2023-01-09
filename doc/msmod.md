# <p >Mini-SEED Modifier - perform stream-based Mini-SEED modifications</p>

1. [Name](#)
1. [Synopsis](#synopsis)
1. [Description](#description)
1. [Options](#options)
1. [Archive Format](#archive-format)
1. [Archive Format Examples](#archive-format-examples)
1. [Author](#author)

## <a id='synopsis'>Synopsis</a>

<pre >
msmod [options] file1 [file2 file3 ...]
</pre>

## <a id='description'>Description</a>

<p ><b>msmod</b> is a general purpose Mini-SEED modification processor. Mini-SEED records are read from each input file and modified as specified and written back out in a serial-manner.  Each modification is applied to all processed records.</p>

<p >If '-' is specified standard input will be read.  Multiple input files will be processed in the order specified.</p>

<p >The header of each record processed is repacked regardless of selected modifications, this repacking includes normalization of the sampling rate fields (e.g. a factor of 32760 and multiplier of -819 will be converted to a factor of 40 and multiplier of 1).  The repacking might also change the offsets to any included blockettes if they were not packed in tight sequence, the order of the blockettes will be preserved.</p>

<p >When a input file is full SEED including both SEED headers and data records all of the headers will be skipped and completely unprocessed.</p>

## <a id='options'>Options</a>

<b>-V</b>

<p style="padding-left: 30px;">Print program version and exit.</p>

<b>-h/-H</b>

<p style="padding-left: 30px;">Print program usage and exit, if -H is specified additional details regarding output to user-defined archive structures (see -A).</p>

<b>-v</b>

<p style="padding-left: 30px;">Be more verbose.  This flag can be used multiple times ("-v -v" or "-vv") for more verbosity.</p>

<b>-s</b>

<p style="padding-left: 30px;">Print a basic summary including the number of records and the number of samples they included after processing all input records.</p>

<b>-ts </b><i>time</i>

<p style="padding-left: 30px;">Limit processing to Mini-SEED records that start after <i>time</i>. The format of the <i>time</i> argument is: 'YYYY[,DDD,HH,MM,SS,FFFFFF]' where valid delimiters are either commas (,), colons (:) or periods (.).</p>

<b>-te </b><i>time</i>

<p style="padding-left: 30px;">Limit processing to Mini-SEED records that end before <i>time</i>. The format of the <i>time</i> argument is: 'YYYY[,DDD,HH,MM,SS,FFFFFF]' where valid delimiters are either commas (,), colons (:) or periods (.).</p>

<b>-tsc </b><i>time</i>

<p style="padding-left: 30px;">Limit processing to Mini-SEED records that contain or start after <i>time</i>.  The format of the <i>time</i> argument is: 'YYYY[,DDD,HH,MM,SS,FFFFFF]' where valid delimiters are either commas (,), colons (:) or periods (.).</p>

<b>-tec </b><i>time</i>

<p style="padding-left: 30px;">Limit processing to Mini-SEED records that contain or end before <i>time</i>.  The format of the <i>time</i> argument is: 'YYYY[,DDD,HH,MM,SS,FFFFFF]' where valid delimiters are either commas (,), colons (:) or periods (.).</p>

<b>-M </b><i>match</i>

<p style="padding-left: 30px;">Limit processing to Mini-SEED records that match the <i>match</i> regular expression.  For each input record a source name string composed of 'NET_STA_LOC_CHAN_QUAL' is created and compared to the regular expression.</p>

<b>-R </b><i>reject</i>

<p style="padding-left: 30px;">Limit processing to Mini-SEED records that do not match the <i>reject</i> regular expression.  For each input record a source name string composed of 'NET_STA_LOC_CHAN_QUAL' is created and compared to the regular expression.</p>

<b>-i</b>

<p style="padding-left: 30px;">Modify input records in-place by writing modified records back to the input files.</p>

<b>-o </b><i>outfile</i>

<p style="padding-left: 30px;">Write all processed Mini-SEED records to <i>outfile</i>.</p>

<b>-A </b><i>format</i>

<p style="padding-left: 30px;">All output records will be written to a directory/file layout defined by <i>format</i>.  All directories implied in the <i>format</i> string will be created if necessary.  The option may be used multiple times to write input records to multiple archives.  See the <b>Archive Format</b> section below.</p>

<b>--net code</b>

<p style="padding-left: 30px;">Specify a new network code.</p>

<b>--sta code</b>

<p style="padding-left: 30px;">Specify a new station code.</p>

<b>--loc id</b>

<p style="padding-left: 30px;">Specify a new location id.</p>

<b>--chan codes</b>

<p style="padding-left: 30px;">Specify new channel codes.  A dot (.) will be interpreted as the same character as the input channel name, for example, "L.." can be specified to only replace the first code with 'L' and leave the other two codes as they are.</p>

<b>--quality [DRMQ]</b>

<p style="padding-left: 30px;">Specify a new header/quality identifier, can be either 'D', 'R', 'M', or 'Q'.</p>

<b>--timeshift secs</b>

<p style="padding-left: 30px;">Specify a time shift in seconds to apply to record start times.  This value can be positive or negative and is not stored in the record as a time correction.</p>

<b>--timecorr secs</b>

<p style="padding-left: 30px;">Change the value of the time correction field in the headers and apply the correction to the record start times.  This is similar to <i>--timecorr</i> except that the correction is stored in the header and bit 1 of the activity flags (field 12) will be set to indicate the correction has been applied.</p>

<b>--timecorrval secs</b>

<p style="padding-left: 30px;">Change the value of the time correction field in the headers without applying the change to the record start times.  Bit 1 of the activity flags (field 12) will not be changed.</p>

<b>--applytimecorr</b>

<p style="padding-left: 30px;">Apply the time correction value to the record start time if it has not previously been applied and set bit 1 of the activity flags (field 12) to indicate the correction has been applied.</p>

<b>--samprate sps</b>

<p style="padding-left: 30px;">Specify new sampling rate in samples per second.  This will be applied to both the nominal and actual (if present) sampling rate fields.</p>

<b>--actflags bit,value</b>

<p style="padding-left: 30px;">Specify an activity flag by indicating the bit to change and it's value.  Valid bit ranges are 0 to 6 and valid bit values are 0 and 1. Activity flags are defined in the SEED 2.4 manual as follows:</p>

<pre style="padding-left: 30px;">
  <b>[Bit 0]</b> : Calibration signals preset
  <b>[Bit 1]</b> : Time correction applied
  <b>[Bit 2]</b> : Beginning of an event, station trigger
  <b>[Bit 3]</b> : End of the event, station detriggers
  <b>[Bit 4]</b> : A positive leap second happended during this record
  <b>[Bit 5]</b> : A negative leap second happended during this record
  <b>[Bit 6]</b> : Event in progress
</pre>

<b>--ioflags bit,value</b>

<p style="padding-left: 30px;">Specify an I/O flag by indicating the bit to change and it's value. Valid bit ranges are 0 to 5 and valid bit values are 0 and 1.  I/O flags are defined in the SEED 2.4 manual as follows:</p>

<pre style="padding-left: 30px;">
  <b>[Bit 0]</b> : Station volume parity error possibly present
  <b>[Bit 1]</b> : Long record read (possibly no problem)
  <b>[Bit 2]</b> : Short record read (record padded)
  <b>[Bit 3]</b> : Start of time series
  <b>[Bit 4]</b> : End of time series
  <b>[Bit 5]</b> : Clock locked
</pre>

<b>--dqflags bit,value</b>

<p style="padding-left: 30px;">Specify a data quality flag by indicating the bit to change and it's value.  Valid bit ranges are 0 to 7 and valid bit values are 0 and 1. Data quality flags are defined in the SEED 2.4 manual as follows:</p>

<pre style="padding-left: 30px;">
  <b>[Bit 0]</b> : Amplifier saturation detected (station dependent)
  <b>[Bit 1]</b> : Digitizer clipping detected
  <b>[Bit 2]</b> : Spikes detected
  <b>[Bit 3]</b> : Glitches detected
  <b>[Bit 4]</b> : Missing/padded data preset
  <b>[Bit 5]</b> : Telementry synchronization error
  <b>[Bit 6]</b> : A digital filter may be charging
  <b>[Bit 7]</b> : Time tag is questionable
</pre>

<b>--b1000encoding encoding</b>

<p style="padding-left: 30px;">Chanage the Blockette 1000 data encoding format field.  Valid values are included in the SEED manual documentation for Blockette 1000.</p>

<b>--b1001tqual percent</b>

<p style="padding-left: 30px;">Chanage the Blockette 1001 timing quality field, valid values are 0 to 100 percent.  Further description is included in the SEED manual documentation for Blockette 1001.</p>

## <a id='archive-format'>Archive Format</a>

<p >An archive format is expanded for each record using the following substitution flags:</p>

<pre >
  <b>n</b> : network code, white space removed
  <b>s</b> : station code, white space removed
  <b>l</b> : location code, white space removed
  <b>c</b> : channel code, white space removed
  <b>Y</b> : year, 4 digits
  <b>y</b> : year, 2 digits zero padded
  <b>j</b> : day of year, 3 digits zero padded
  <b>H</b> : hour, 2 digits zero padded
  <b>M</b> : minute, 2 digits zero padded
  <b>S</b> : second, 2 digits zero padded
  <b>F</b> : fractional seconds, 4 digits zero padded
  <b>q</b> : single character record quality indicator (D, R, Q)
  <b>L</b> : data record length in bytes
  <b>r</b> : sample rate (Hz) as a rounded integer
  <b>R</b> : sample rate (Hz) as a float with 6 digit precision
  <b>%</b> : the percent (%) character
  <b>#</b> : the number (#) character
</pre>

<p >The flags are prefaced with either the '%' or '#' modifier.  The '%' modifier indicates a defining flag while the '#' indicates a non-defining flag.  All records with the same set of defining flags will be written to the same file.  Non-defining flags will be expanded using the values in the first record for the resulting file name.</p>

<p >Time flags are based on the start time of the given record.</p>

## <a id='archive-format-examples'>Archive Format Examples</a>

<p >The format string for the predefined <i>BUD</i> layout:</p>

<p ><b>/archive/%n/%s/%s.%n.%l.%c.%Y.%j</b></p>

<p >would expand to day length files named something like:</p>

<p ><b>/archive/NL/HGN/HGN.NL..BHE.2003.055</b></p>

<p >As an example of using non-defining flags the format string for the predefined <i>CSS</i> layout:</p>

<p ><b>/data/%Y/%j/%s.%c.%Y:%j:#H:#M:#S</b></p>

<p >would expand to:</p>

<p ><b>/data/2003/055/HGN.BHE.2003:055:14:17:54</b></p>

<p >resulting in day length files because the hour, minute and second are specified with the non-defining modifier.  The hour, minute and second fields are from the first record in the file.</p>

## <a id='author'>Author</a>

<pre >
Chad Trabant
IRIS Data Management Center
</pre>


(man page 2018/06/27)
