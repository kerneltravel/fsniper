#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <fnmatch.h>
#include <pcre.h>
#include <magic.h>
#include "keyvalcfg.h"
#include "watchnode.h"
#include "util.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef USE_EFENCE
#include <efence.h>
#endif

extern struct keyval_section *config;
extern struct watchnode *node;
extern int verbose;

void handle_event(struct inotify_event* event, int writefd)
{
	char abort;
	char isglob;
	char foundslash;
	char *filename;
	char *handlersubstr;
	char *handlerexec;
	const char *mimetype;
	magic_t magic;
	int i, j, sysret, attempts;
	struct keyval_section *child;
	struct keyval_pair *handler;
	pcre *regex;
	char *regex_str;
	const char *pcre_err;
	int pcre_erroffset;
	int pcre_match;

/* find the node that corresponds to the event's descriptor */
	for (; node; node = node->next)
	{
		if (node->wd == event->wd)
			break;
	}

	if (!node)
		return;

/* combine the name inotify gives with the full path to the file */
	filename = malloc(strlen(node->path) + strlen("/") + strlen(event->name) + 1);
	strcpy(filename, node->path);
	strcat(filename, "/");
	strcat(filename, event->name);
	
/* does perror work here? should we also call exit()? */
	if ( (magic = magic_open(MAGIC_MIME)) == NULL)
		perror("magic_open");

	if (magic_load(magic, NULL) < 0)
		perror("magic_load");

	mimetype = magic_file(magic, filename);
	if (mimetype == NULL)
		perror("magic_file");

/* match the config's expression against a glob, regex, or mimetype */
	abort = 0;

	for (child = node->section->children; child; child = child->next)
	{
		abort = 0;
		foundslash = 0;
		isglob = 1;
		for (i=0; child->name[i]; i++)
		{
			if (child->name[i] == '/')
			{
				isglob = 0;
				break;
			}
		}

		if (isglob == 1)
		{
			if (fnmatch(child->name, filename, 0) != 0)
				abort = 1;

			if (abort == 0)
				break;
		}

		/* regexs are in the format /regex/ */
		if (isglob == 0 && child->name[0] == '/' && child->name[strlen(child->name)-1] == '/')
		{
			/* child->name is "/regex/", we want "regex" */
			regex_str = strndup(child->name+1, strlen(child->name)-2);
			regex = pcre_compile(regex_str, 0, &pcre_err, &pcre_erroffset, NULL);
			pcre_match = pcre_exec(regex, NULL, filename, strlen(filename), 0, 0, NULL, 0);
			free(regex_str);

			if (pcre_match < 0)
				abort = 1;

			if (abort == 0)
				break;
		}
		
		/* parse the mimetype to see if it matches the one in the config */
		for (i=0, j=0; isglob == 0 && mimetype[i] && child->name[j] && abort == 0; i++, j++)
		{
			if (!mimetype[i])
			{
				abort = 1;
				break;
			}
			else if (!child->name[j])
			{
				abort = 1;
				break;
			}

			if (foundslash == 0 && child->name[j] == '/')
				foundslash = 1;

			if (mimetype[i] != child->name[j] && child->name[j] != '*')
			{
				abort = 1;
				break;
			}
			else if (child->name[j] == '*' && foundslash == 0)
			{
				while (mimetype[i] != '/')
					i++;
				while (child->name[j] != '/')
					j++;
				foundslash = 1;
			}
			else if (child->name[j] == '*' && foundslash == 1)
				break;
		}
		if (abort == 0)
			break;
	}
	
	if (abort == 1)
	{
		free(filename);
		magic_close(magic);
		exit(-1);
	}

	/* dup the fd */
	dup2(writefd, fileno(stdout));

/* find the handlers which correspond to the mimetype, and continue executing them
   until we've run them all or one returns 0 */	
	handler = child->keyvals;

	/* delay attempts are limited! */
	attempts = 0;

	/* TODO: configify or constify attempts */
	while (handler && attempts < 5)
	{
		if (strcmp(handler->key, "handler") != 0)
			break;

		handlersubstr = strstr(handler->value, "%%");
		/* is no %% an error or should we pass the filename at the end if there's no %%? */
		if (handlersubstr == NULL)
		{
			free(filename);
			magic_close(magic);
			close(writefd);
			exit(1);
		}

		handlerexec = malloc(strlen(handler->value) - strlen("%%") + strlen(filename)
												 + strlen(" \"") + strlen("\"") + 1);
		strncpy(handlerexec, handler->value, handlersubstr - handler->value);
		strcat(handlerexec, " \"");
		strcat(handlerexec, filename);
		strcat(handlerexec, "\"");
		strcat(handlerexec, handler->value + (handlersubstr - handler->value) + strlen("%%"));
		
		write(writefd, "Executing: ", 11);
		write(writefd, handlerexec, strlen(handlerexec));
		write(writefd, "\n", 1);
		sysret = WEXITSTATUS(system(handlerexec));		

		free(handlerexec);


		/* do somethng based on return code! */

		if (sysret == 0)
		{
			/* the handler handled it!  break out of the handler loop and exit this thread */
			break;
		}
		else if (sysret == DELAY_RET_CODE)
		{
			/* go to sleep for a while */
			/* TODO: get config value for this delay time */
			attempts++;
			write(writefd, "Handler indicated delay, sleeping...", 37); 
			sleep(5 * 60);
			write(writefd, "Handler process resuming.", 26);
		} 
		else
		{
			/* some other return code, means try the next handler */
			handler = handler->next;
		}
	}

	/* TODO: constify/configify this again */
	if (attempts == 5)
		write(writefd, "Handler gave up on retries.", 28); 

	free(filename);
	magic_close(magic);

	/* close down our pipe */
	close(writefd);
	exit(0);
}
