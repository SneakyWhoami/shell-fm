/*
	Copyright (C) 2006 by Jonas Kramer
	Published under the terms of the GNU General Public License (GPL).
*/

#define _GNU_SOURCE


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <signal.h>
#include <stdarg.h>

#include "hash.h"
#include "http.h"
#include "play.h"
#include "settings.h"
#include "md5.h"
#include "history.h"
#include "service.h"
#include "playlist.h"
#include "ropen.h"
#include "strary.h"
#include "sckif.h"
#include "util.h"
#include "rest.h"
#include "json.h"
#include "split.h"

#include "globals.h"

struct hash data; /* Warning! MUST be bzero'd ASAP or we're all gonna die! */

pid_t playfork = 0; /* PID of the decoding & playing process, if running */
int playpipe = 0;

struct playlist playlist;
char * current_station = NULL;
unsigned resolve_artist(const char * name);
void expand_station_url(char **);

#define HTTP_STATION_PREFIX "http://www.last.fm/listen/"


int station(const char * station_url) {
	char * response, * complete_url;
	unsigned retval = !0;
	json_value * json;
	struct hash p = { 0, NULL };

	delayquit = 0;

	if(playfork && haskey(& rc, "delay-change")) {
		if(nextstation) {
			/*
			  Cancel station change if url is empty or equal to the current
			  station.
			*/
			free(nextstation);
			nextstation = NULL;

			if(!strlen(station_url) || !strcmp(station_url, current_station)) {
				puts("Station change cancelled.");
				return 0;
			}
		}

		/* Do nothing if the station is already played. */
		else if(current_station && !strcmp(current_station, station_url)) {
			return 0;
		}

		/* Do nothing if the station URL is empty. */
		else if(!strlen(station_url)) {
			return 0;
		}

		puts("\rDelayed.");
		nextstation = strdup(station_url);

		return 0;
	}

	/* Do nothing if the station is already played. */
	else if(current_station && !strcmp(current_station, station_url)) {
		return 0;
	}

	freelist(& playlist);

	if(!haskey(& rc, "session")) {
		fputs("Not authenticated, yet.\n", stderr);
		return 0;
	}

	if(!strncmp(HTTP_STATION_PREFIX, station_url, strlen(HTTP_STATION_PREFIX))) {
		station_url += strlen(HTTP_STATION_PREFIX);
	}

	if(!station_url || !strlen(station_url))
		return 0;

	if(!strncasecmp(station_url, "lastfm://", 9)) {
		complete_url = strdup(station_url);
		station_url += 9;
	}
	else {
		int size = strlen(station_url) + 10;
		complete_url = malloc(size);
		snprintf(complete_url, size, "lastfm://%s", station_url);
	}

	expand_station_url(& complete_url);

	set(& p, "station", complete_url);

	debug("expanded station = <%s>\n", value(& p, "station"));

	free(complete_url);

	response = rest("radio.tune", & p);

	empty(& p);

	if(response == NULL)
		return 0;

	json = json_parse(response);
	json_hash(json, & p, NULL);

	dump_hash(& p);

	if(haskey(& p, "error")) {
		retval = 0;

		fprintf(stderr, "Sorry, couldn't set station to %s. %s.\n", station_url, value(& p, "message"));
	}
	else {
		if(haskey(& p, "station.name")) {
			if(playlist.title != NULL)
				free(playlist.title);

			playlist.title = strdup(value(& p, "station.name"));
		}
	}

	json_value_free(json);
	empty(& p);
	free(response);
	response = NULL;

	if(retval) {
		expand(& playlist);

		enable(CHANGED);
		histapp(station_url);

		if(current_station)
			free(current_station);

		current_station = strdup(station_url);
		assert(current_station != NULL);

		if(retval && playfork) {
			enable(INTERRUPTED);
			kill(playfork, SIGUSR1);
		}
	}

	return retval;
}


/*
	Takes pointer to a playlist, forks off a playback process and tries to play
	the next track in the playlist. If there's already a playback process, it's
	killed first (which means the currently played track is skipped).
*/
int play(struct playlist * list) {
	unsigned i;
	int pipefd[2];
	char * keys [] = {
		"creator", "title", "album", "duration", "station",
		"trackauth", "trackpage", "artistpage", "albumpage",
		"image", "freeTrackURL",
	};

	assert(list != NULL);

	if(!list->left)
		return 0;

	if(playfork) {
		kill(playfork, SIGUSR1);
		return !0;
	}

	enable(QUIET);

	empty(& track);

	for(i = 0; i < (sizeof(keys) / sizeof(char *)); ++i)
		set(& track, keys[i], value(& list->track->track, keys[i]));

	if(pipe(pipefd) != 0)
		return !0;

	playfork = fork();

	if(!playfork) {
		FILE * fd = NULL;
		const char * location = value(& list->track->track, "location");

		close(pipefd[1]);
		rmsckif();

		subfork = 0;

		if(location != NULL) {
			fetch(location, & fd, NULL, NULL);

			if(fd != NULL) {
				/*
					If there was an error, tell the main process about it by
					sending SIGUSR2.
				*/
				if(!playback(fd, pipefd[0]))
					kill(getppid(), SIGUSR2);

				close(pipefd[0]);
				fshutdown(& fd);
			}
		}

		exit(EXIT_SUCCESS);
	}

	close(pipefd[0]);

	if(playpipe != 0)
		close(playpipe);

	playpipe = pipefd[1];

	return !0;
}


unsigned resolve_artist(const char * name) {
	unsigned artist_id = 0;
	char * response = fetch(
		makeurl("http://www.last.fm/ajax/getResource?type=artist&name=%s", name),
		NULL, NULL, NULL
	);

	debug("response=<\n%s\n>\n", response);

	if(response != NULL) {
		struct hash h = { 0, NULL };
		json_value * json;

		assert((json = json_parse(response)) != NULL);

		json_hash(json, & h, NULL);

		if(haskey(& h, "resource.id"))
			artist_id = atol(value(& h, "resource.id"));

		json_value_free(json);
		empty(& h);
	}

	free(response);

	return artist_id;
}


void expand_station_url(char ** station_url) {
	debug("checking url: %s\n", * station_url);

	/* Allow URLs like artists/name1*name2*name3 to be entered and replace the
	 * names with artist ids. */
	if(strncmp(* station_url, "lastfm://artists/", 17) == 0) {
		debug("expanding multiple artists radio\n");

		unsigned name_count = 0, n;
		char ** names = split((* station_url) + 17, "*", & name_count);
		size_t url_length = 17;

		* station_url = realloc(* station_url, 18);
		sprintf(* station_url, "lastfm://artists/");

		for(n = 0; n < name_count; ++n) {
			unsigned artist_id = resolve_artist(names[n]);

			debug("artist <%s> id <%d>\n", names[n], artist_id);

			if(artist_id) {
				char id_string[32] = { 0, };

				snprintf(id_string, sizeof(id_string), "%d", artist_id);

				url_length += strlen(id_string) + n;

				* station_url = realloc(* station_url, url_length + 1);

				if(n > 0)
					strcat(* station_url, ",");

				strcat(* station_url, id_string);
			}
		}

		purge(names);

		debug("new station: %s\n", * station_url);
	}
	else {
		debug("nothing to expand\n");
	}
}
