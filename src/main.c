#include <time.h>

#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <locale.h>
#include <errno.h>
#include <libxml/xmlmemory.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>

#include "solar.h"
#include "systemtime.h"
#include "config-ini.h"
#include "location-geoclue2.h"
#include "location-manual.h"

/* poll.h is not available on Windows but there is no Windows location provider
   using polling. On Windows, we just define some stubs to make things compile.
   */
#ifndef _WIN32
# include <poll.h>
#else
#define POLLIN 0
struct pollfd {
  int fd;
  short events;
  short revents;
};
int poll(struct pollfd *fds, int nfds, int timeout) { abort(); return -1; }
#endif

#if defined(HAVE_SIGNAL_H) && !defined(__WIN32__)
# include <signal.h>
#endif

#ifdef ENABLE_NLS
# include <libintl.h>
# define _(s) gettext(s)
# define N_(s) (s)
#else
# define _(s) s
# define N_(s) s
# define gettext(s) s
#endif

/* Bounds for parameters. */
#define MIN_LAT  -90.0
#define MAX_LAT   90.0
#define MIN_LON -180.0
#define MAX_LON  180.0

typedef enum {
  DAY_NIGHT = 0,
  DAY_SUNSET_NIGHT
} transition_mode_t;

static int
provider_try_start(const location_provider_t *provider,
       location_state_t **state, config_ini_state_t *config,
       char *args)
{
  int r;

  r = provider->init(state);
  if (r < 0) {
    fprintf(stderr, _("Initialization of %s failed.\n"), provider->name);
    return -1;
  }

  /* Set provider options from config file. */
  config_ini_section_t *section =
    config_ini_get_section(config, provider->name);
  if (section != NULL) {
    config_ini_setting_t *setting = section->settings;
    while (setting != NULL) {
      r = provider->set_option(*state, setting->name, setting->value);
      if (r < 0) {
        provider->free(*state);
        fprintf(stderr, _("Failed to set %s option.\n"),
          provider->name);
        /* TRANSLATORS: `help' must not be
           translated. */
        fprintf(stderr, _("Try `-l %s:help' for more information.\n"),
          provider->name);
        return -1;
      }
      setting = setting->next;
    }
  }

  /* Set provider options from command line. */
  const char *manual_keys[] = { "lat", "lon" };
  int i = 0;
  while (args != NULL) {
    char *next_arg = strchr(args, ':');
    if (next_arg != NULL) *(next_arg++) = '\0';

    const char *key = args;
    char *value = strchr(args, '=');
    if (value == NULL) {
      /* The options for the "manual" method can be set
         without keys on the command line for convencience and for backwards
         compatability. We add the proper keys here before calling
         set_option(). */
      if (strcmp(provider->name, "manual") == 0 &&
          i < sizeof(manual_keys)/sizeof(manual_keys[0])) {
        key = manual_keys[i];
        value = args;
      } else {
        fprintf(stderr, _("Failed to parse option `%s'.\n"), args);
        return -1;
      }
    } else {
      *(value++) = '\0';
    }

    r = provider->set_option(*state, key, value);
    if (r < 0) {
      provider->free(*state);
      fprintf(stderr, _("Failed to set %s option.\n"), provider->name);
      /* TRANSLATORS: `help' must not be translated. */
      fprintf(stderr, _("Try `-l %s:help' for more information.\n"),
          provider->name);
      return -1;
    }

    args = next_arg;
    i += 1;
  }

  /* Start provider. */
  r = provider->start(*state);
  if (r < 0) {
    provider->free(*state);
    fprintf(stderr, _("Failed to start provider %s.\n"), provider->name);
    return -1;
  }

  return 0;
}


/* Wait for location to become available from provider.
   Waits until timeout (milliseconds) has elapsed or forever if timeout is -1.
   Writes location to loc. Returns -1 on error, 0 if timeout was reached, 1 if
   location became available. */
static int
provider_get_location(
  const location_provider_t *provider, location_state_t *state,
  int timeout, location_t *loc)
{
  int available = 0;
  struct pollfd pollfds[1];
  while (!available) {
    int loc_fd = provider->get_fd(state);
    if (loc_fd >= 0) {
      /* Provider is dynamic. */
      /* TODO: This should use a monotonic time source. */
      double now;
      int r = systemtime_get_time(&now);
      if (r < 0) {
        fputs(_("Unable to read system time.\n"), stderr);
        return -1;
      }

      /* Poll on file descriptor until ready. */
      pollfds[0].fd = loc_fd;
      pollfds[0].events = POLLIN;
      r = poll(pollfds, 1, timeout);
      if (r < 0) {
        perror("poll");
        return -1;
      } else if (r == 0) {
        return 0;
      }

      double later;
      r = systemtime_get_time(&later);
      if (r < 0) {
        fputs(_("Unable to read system time.\n"), stderr);
        return -1;
      }

      /* Adjust timeout by elapsed time */
      if (timeout >= 0) {
        timeout -= (later - now) * 1000;
        timeout = timeout < 0 ? 0 : timeout;
      }
    }

    int r = provider->handle(state, loc, &available);
    if (r < 0) return -1;
  }

  return 1;
}


/* Check whether location is valid.
   Prints error message on stderr and returns 0 if invalid, otherwise returns
   1. */
static int
location_is_valid(const location_t *location)
{
  /* Latitude */
  if (location->lat < MIN_LAT || location->lat > MAX_LAT) {
    /* TRANSLATORS: Append degree symbols if possible. */
    fprintf(stderr, _("Latitude must be between %.1f and %.1f.\n"), MIN_LAT,
      MAX_LAT);
    return 0;
  }

  /* Longitude */
  if (location->lon < MIN_LON || location->lon > MAX_LON) {
    /* TRANSLATORS: Append degree symbols if possible. */
    fprintf(stderr, _("Longitude must be between %.1f and %.1f.\n"), MIN_LON,
      MAX_LON);
    return 0;
  }

  return 1;
}


/*
 * MAIN
 */
int main(int argc, char *argv[]) {

  /* Get config information */
  int r = 0;
  location_state_t *location_state;
  location_t loc = {NAN, NAN};
  const location_provider_t *p = NULL;
  config_ini_state_t config_state;
  r = config_ini_init(&config_state, NULL);

  /* Decide between manual or geoclue */
  config_ini_section_t *section = config_ini_get_section(&config_state, "backgrounds");
  if (section != NULL) {
    config_ini_setting_t *setting = section->settings;
    while(setting != NULL) {
      if (strcmp(setting->name, "location-provider") == 0) {
        if (strcmp(setting->value, "manual") == 0) {
          p = &manual_location_provider;
        } else if (strcmp(setting->value, "geoclue") == 0) {
          p = &geoclue2_location_provider;
        }
      }
      /* printf("Location:\t%s\n", setting->value); */
      setting = setting->next;
    }
  } else {
    p = &geoclue2_location_provider;
  }

  r = provider_try_start(p, &location_state, &config_state, NULL);
  r = provider_get_location(p, location_state, -1, &loc);
  if (r < 0) {
    fputs(_("Unable to get location from provider.\n"), stderr);
    return -1;
  }
  if (!location_is_valid(&loc)) {
    fputs(_("Invalid location returned from provider.\n"), stderr);
    return -1;
  }

  time_t t = time(NULL);
  double table[SOLAR_TIME_MAX];
  solar_table_fill(t, loc.lat, loc.lon, table);

  /* time_t noon = table[SOLAR_TIME_NOON]; */
  /* time_t midnight = table[SOLAR_TIME_MIDNIGHT]; */
  /* time_t astro_dawn = table[SOLAR_TIME_ASTRO_DAWN]; */
  time_t naut_dawn = table[SOLAR_TIME_NAUT_DAWN];
  /* time_t civil_dawn = table[SOLAR_TIME_CIVIL_DAWN]; */
  time_t sunrise = table[SOLAR_TIME_SUNRISE];
  time_t sunset = table[SOLAR_TIME_SUNSET];
  /* time_t civil_dusk = table[SOLAR_TIME_CIVIL_DUSK]; */
  time_t naut_dusk = table[SOLAR_TIME_NAUT_DUSK];
  /* time_t astro_dusk = table[SOLAR_TIME_ASTRO_DUSK]; */

  char *infile = argv[1];
  int len = strlen(infile);
  char outfile[len];
  strncpy(outfile, infile, len-3); // strip the .in from the xml.in filename
  outfile[len-3] = 0;

  xmlDocPtr doc;
  xmlNodePtr root;
  transition_mode_t mode = -1;
  int hour;

  doc = xmlParseFile(infile);
  root = xmlDocGetRootElement(doc);

  if (strcmp((char*)xmlNodeGetContent(root->xmlChildrenNode->next), "day-night") == 0) {
    mode = DAY_NIGHT;
  } else if (strcmp((char*)xmlNodeGetContent(root->xmlChildrenNode->next), "day-sunset-night") == 0) {
    mode = DAY_SUNSET_NIGHT;
  }
  hour = (int)atoi((char*)xmlNodeGetContent(root->xmlChildrenNode->next->next->next->xmlChildrenNode->next->next->next->next->next->next->next));

  switch(mode) {
    case DAY_NIGHT:
      {
        int start_offset = hour * 3600;
        int sunrise_half = sunrise - naut_dawn;
        int sunset_half = naut_dusk - sunset;

        int sunrise_start = naut_dawn - 1553749200 - start_offset; /*FIXME*/
        if (sunrise_start < 0) {
          start_offset = start_offset + sunrise_start - 3600;
          hour = start_offset / 3600;
        }

        int sunrise_end = sunrise - 1553749200 - start_offset + sunrise_half; /*FIXME*/
        int sunset_start = sunset - 1553749200 - start_offset - sunset_half; /*FIXME*/
        int sunset_end = naut_dusk - 1553749200 - start_offset; /*FIXME*/

        int sunrise_length = sunrise_end - sunrise_start;
        int day_length = sunset_start - sunrise_end;
        int sunset_length = sunset_end - sunset_start;
        int night_to_start = 86400 - sunrise_start - sunrise_length - day_length - sunset_length;

        /* Update hour */
        /* struct tm *now = localtime(&t);
         * if (now->tm_isdst == 1) {
         *   hour = hour + 1;
         * } */
        char tmp[100];
        sprintf(tmp, "%d", hour);
        xmlNodeSetContent(root->xmlChildrenNode->next->next->next->xmlChildrenNode->next->next->next->next->next->next->next,  (unsigned char*)tmp);

        /* Update @starttosunrise@ */
        sprintf(tmp, "%d", sunrise_start);
        xmlNodeSetContent(root->xmlChildrenNode->next->next->next->next->next->xmlChildrenNode->next,  (unsigned char*)tmp);

        /* Update @sunrise@ */
        sprintf(tmp, "%d", sunrise_length);
        xmlNodeSetContent(root->xmlChildrenNode->next->next->next->next->next->next->next->xmlChildrenNode->next,  (unsigned char*)tmp);

        /* Update @day@ */
        sprintf(tmp, "%d", day_length);
        xmlNodeSetContent(root->xmlChildrenNode->next->next->next->next->next->next->next->next->next->xmlChildrenNode->next,  (unsigned char*)tmp);

        /* Update @sunset@ */
        sprintf(tmp, "%d", sunset_length);
        xmlNodeSetContent(root->xmlChildrenNode->next->next->next->next->next->next->next->next->next->next->next->xmlChildrenNode->next,  (unsigned char*)tmp);

        /* Update @nighttostart@ */
        sprintf(tmp, "%d", night_to_start);
        xmlNodeSetContent(root->xmlChildrenNode->next->next->next->next->next->next->next->next->next->next->next->next->next->xmlChildrenNode->next,  (unsigned char*)tmp);
      }
      break;
    case DAY_SUNSET_NIGHT:
      {
        unsigned int start_offset = hour * 3600;
        unsigned int sunrise_half = sunrise - naut_dawn;
        unsigned int sunset_half = naut_dusk - sunset;

        unsigned int sunrise_start = naut_dawn - 1553749200 - start_offset; /*FIXME*/
        if (sunrise_start < 0) {
          start_offset = start_offset + sunrise_start - 3600;
          hour = start_offset / 3600;
        }

        unsigned int sunrise_end = sunrise - 1553749200 - start_offset + sunrise_half; /*FIXME*/
        unsigned int sunset_start = sunset - 1553749200 - start_offset - 3*sunset_half; /*FIXME*/
        unsigned int sunset_end = sunset - 1553749200 - start_offset - sunset_half; /*FIXME*/
        unsigned int nightfall_end = naut_dusk - 1553749200 - start_offset; /*FIXME*/

        unsigned int sunrise_length = sunrise_end - sunrise_start;
        unsigned int day_length = sunset_start - sunrise_end;
        unsigned int sunset_length = sunset_end - sunset_start;
        unsigned int nightfall_length = nightfall_end - sunset_end;
        unsigned int night_to_start = 86400 - sunrise_start - sunrise_length - day_length - sunset_length - nightfall_length;

        /* Update hour */
        /* struct tm *now = localtime(&t);
         * if (now->tm_isdst == 1) {
         *   hour = hour + 1;
         * } */
        char tmp[100];
        sprintf(tmp, "%d", hour);
        xmlNodeSetContent(root->xmlChildrenNode->next->next->next->xmlChildrenNode->next->next->next->next->next->next->next, (unsigned char*)tmp);

        /* Update @starttosunrise@ */
        sprintf(tmp, "%d", sunrise_start);
        xmlNodeSetContent(root->xmlChildrenNode->next->next->next->next->next->xmlChildrenNode->next,  (unsigned char*)tmp);

        /* Update @sunrise@ */
        sprintf(tmp, "%d", sunrise_length);
        xmlNodeSetContent(root->xmlChildrenNode->next->next->next->next->next->next->next->xmlChildrenNode->next,  (unsigned char*)tmp);

        /* Update @day@ */
        sprintf(tmp, "%d", day_length);
        xmlNodeSetContent(root->xmlChildrenNode->next->next->next->next->next->next->next->next->next->xmlChildrenNode->next,  (unsigned char*)tmp);

        /* Update @sunset@ */
        sprintf(tmp, "%d", sunset_length);
        xmlNodeSetContent(root->xmlChildrenNode->next->next->next->next->next->next->next->next->next->next->next->xmlChildrenNode->next,  (unsigned char*)tmp);

        /* Update @nighttostart@ */
        sprintf(tmp, "%d", nightfall_length);
        xmlNodeSetContent(root->xmlChildrenNode->next->next->next->next->next->next->next->next->next->next->next->next->next->xmlChildrenNode->next,  (unsigned char*)tmp);

        /* Update @nighttostart@ */
        sprintf(tmp, "%d", night_to_start);
        xmlNodeSetContent(root->xmlChildrenNode->next->next->next->next->next->next->next->next->next->next->next->next->next->next->next->xmlChildrenNode->next,  (unsigned char*)tmp);
      }
      break;
    default:
      fprintf(stderr, "Mode %d parameter not found.\n", mode);
      break;
  }

  xmlSaveFile(outfile, doc);
  xmlFreeDoc(doc);
  xmlCleanupParser();

  return(0);
}