#ifndef FIPS_TEST_SETTLE_FAULTS_H
#define FIPS_TEST_SETTLE_FAULTS_H
#include <fips/proto/settle/faults.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
/* Host-only CLI for isolated mutation children. No environment reads in core. */
static inline fips_settle_faults *fips_test_settle_fault_args(int argc,char **argv,const char **fixture)
{
    *fixture=NULL;
    if(argc==2&&!strcmp(argv[1],"--list-faults")){for(unsigned f=0;f<FIPS_SETTLE_FAULT_COUNT;++f)if(fips_settle_fault_supported((fips_settle_fault)f))puts(fips_settle_fault_name((fips_settle_fault)f));exit(0);}
    if(argc==1)return NULL;
    if(argc==2 && argv[1][0]!='-'){*fixture=argv[1];return NULL;}
    if((argc!=3&&argc!=4)||strcmp(argv[1],"--fault")){fprintf(stderr,"usage: %s [fixture] | --fault NAME [fixture] | --list-faults\n",argv[0]);exit(2);}
    fips_settle_fault f; if(!fips_settle_fault_named(argv[2],&f)||!fips_settle_fault_supported(f)){fprintf(stderr,"unsupported settlement fault: %s\n",argv[2]);exit(2);}
#ifdef __linux__
    /* Ubuntu's piped Apport handler ignores RLIMIT_CORE. Intentional assertion
     * children must not launch crash-report collection for every killed mutant.
     * This changes dump generation only, not sanitizer checks or diagnostics. */
    if(prctl(PR_SET_DUMPABLE,0L,0L,0L,0L)!=0){perror("disable mutation core dump");exit(2);}
#endif
    fips_settle_faults *scope=calloc(1,sizeof(*scope));if(!scope || fips_settle_fault_select(scope,f,true)!=FIPS_OK || fips_settle_fault_enter(scope,NULL)!=FIPS_OK)abort();
    if(argc==4)*fixture=argv[3];
    return scope;
}
static inline void fips_test_settle_fault_done(fips_settle_faults *scope)
{ if(fips_settle_fault_enter(NULL,NULL)!=FIPS_OK)abort();free(scope); }
#endif
