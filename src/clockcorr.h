#ifndef CLOCKCORR_H
#define CLOCKCORR_H


typedef struct ClockCorrConfig_s {
  char type[20];
  hptime_t *inst_time;
  hptime_t *ref_time;
  int num_records;
  double *coeff;
  int num_coeffs;
} ClockCorrConfig;



extern int  process_cc(ClockCorrConfig *cc_confg, MSRecord *msr);
extern ClockCorrConfig *read_cc_config(char *ccfilename);

extern ClockCorrConfig *cc_config;

#endif /* CLOCKCORR_H */
