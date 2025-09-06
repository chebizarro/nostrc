#include "relayd_config.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

static void trim(char *s){
  char *p=s; while(*p && isspace((unsigned char)*p)) p++; if(p!=s) memmove(s,p,strlen(p)+1);
  size_t n=strlen(s); while(n>0 && isspace((unsigned char)s[n-1])) s[--n]=0;
}

static int parse_supported_nips(const char *val, int *out, int *outn){
  *outn = 0; if(!val) return -1;
  const char *p = val; while(*p && *p!='[') p++; if(*p!='[') return -1; p++;
  while(*p && *p!=']'){
    while(isspace((unsigned char)*p) || *p==',') p++;
    if(*p==']') break;
    char *end=NULL; long v = strtol(p,&end,10);
    if(p==end) return -1;
    if(*outn < RELAYD_MAX_SUPPORTED_NIPS) out[(*outn)++] = (int)v;
    p = end;
  }
  return 0;
}

static void apply_defaults(RelaydConfig *cfg){
  memset(cfg, 0, sizeof(*cfg));
  snprintf(cfg->listen, sizeof(cfg->listen), "%s", "127.0.0.1:4848");
  snprintf(cfg->storage_driver, sizeof(cfg->storage_driver), "%s", "nostrdb");
  cfg->supported_nips[0]=1; cfg->supported_nips[1]=11; cfg->supported_nips[2]=42; cfg->supported_nips[3]=45; cfg->supported_nips_count=4;
  cfg->max_filters = 10;
  cfg->max_limit = 500;
  cfg->max_subs = 1;
  cfg->rate_ops_per_sec = 20;
  cfg->rate_burst = 40;
  cfg->negentropy_enabled = 0;
  snprintf(cfg->name, sizeof(cfg->name), "%s", "nostrc-relayd");
  snprintf(cfg->software, sizeof(cfg->software), "%s", "nostrc");
  snprintf(cfg->version, sizeof(cfg->version), "%s", "0.1");
  cfg->description[0] = '\0';
  cfg->contact[0] = '\0';
  snprintf(cfg->auth, sizeof(cfg->auth), "%s", "off");
}

int relayd_config_load(const char *path, RelaydConfig *out){
  if(!out) return -1;
  apply_defaults(out);
  if(!path) return 0;
  FILE *f = fopen(path, "r");
  if(!f) return 0; /* use defaults if missing */
  char line[512];
  while(fgets(line, sizeof(line), f)){
    trim(line);
    if(line[0]=='#' || line[0]==';' || line[0]==0) continue;
    char *eq = strchr(line, '=');
    if(!eq) continue;
    *eq = 0; char *key=line; char *val=eq+1; trim(key); trim(val);
    if(*val=='"'){
      /* string */
      size_t L = strlen(val);
      if(L>=2 && val[L-1]=='"'){ val[L-1]=0; val++; }
    }
    if(strcmp(key, "listen")==0){ snprintf(out->listen, sizeof(out->listen), "%s", val); }
    else if(strcmp(key, "storage_driver")==0){ snprintf(out->storage_driver, sizeof(out->storage_driver), "%s", val); }
    else if(strcmp(key, "supported_nips")==0){ int n=0; (void)parse_supported_nips(val, out->supported_nips, &n); out->supported_nips_count=n; }
    else if(strcmp(key, "max_filters")==0){ out->max_filters = atoi(val); }
    else if(strcmp(key, "max_limit")==0){ out->max_limit = atoi(val); }
    else if(strcmp(key, "max_subs")==0){ out->max_subs = atoi(val); }
    else if(strcmp(key, "name")==0){ snprintf(out->name, sizeof(out->name), "%s", val); }
    else if(strcmp(key, "software")==0){ snprintf(out->software, sizeof(out->software), "%s", val); }
    else if(strcmp(key, "version")==0){ snprintf(out->version, sizeof(out->version), "%s", val); }
    else if(strcmp(key, "description")==0){ snprintf(out->description, sizeof(out->description), "%s", val); }
    else if(strcmp(key, "contact")==0){ snprintf(out->contact, sizeof(out->contact), "%s", val); }
    else if(strcmp(key, "auth")==0){ snprintf(out->auth, sizeof(out->auth), "%s", val); }
    else if(strcmp(key, "rate_ops_per_sec")==0){ out->rate_ops_per_sec = atoi(val); }
    else if(strcmp(key, "rate_burst")==0){ out->rate_burst = atoi(val); }
    else if(strcmp(key, "negentropy_enabled")==0){ out->negentropy_enabled = atoi(val); }
  }
  fclose(f);
  return 0;
}
