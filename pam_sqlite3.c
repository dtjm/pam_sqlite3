/*
 * PAM authentication module for SQLite
 *
 * SQLite3 port: Corey Henderson (cormander) <admin@ravencore.com>
 * SQLite port: Edin Kadribasic <edink@php.net>
 * Extended SQL configuration support by Wez Furlong <wez@thebrainroom.com>
 *
 * Based in part on pam_pgsql.c by David D.W. Downey ("pgpkeys") <david-downey@codecastle.com>
 *
 * Based in part on pam_unix.c of FreeBSD.
 *
                          pam_sqlite3

             This work is Copyright (C) Corey Henderson
                           GNU GPL v3
 This work is a dirivative of pam_sqlite, Copyright (C) Edin Kadriba
   pam_sqlite is a dirivative of libpam-pgsql, see the CREDITS file

Different portions of this program are Copyright to the respective
authors of those peices of code; but are all under the terms
of of the GNU General Pulblic License.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

 */

/* $Id: pam_sqlite.c,v 1.11 2003/07/17 13:47:07 wez Exp $ */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <syslog.h>
#include <ctype.h>
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include <time.h>
#include <sqlite3.h>
#if HAVE_CRYPT_H
#include <crypt.h>
#endif

#define PAM_SM_AUTH
#define PAM_SM_ACCOUNT
#define PAM_SM_PASSWORD
#include <security/pam_modules.h>
#include <security/pam_appl.h>
#include "pam_mod_misc.h"

#define PASSWORD_PROMPT			"Password: "
#define PASSWORD_PROMPT_NEW		"New password: "
#define PASSWORD_PROMPT_CONFIRM "Confirm new password: "
#define CONF					"/etc/pam_sqlite3.conf"

#define DBGLOG(x...)  if(options->debug) {							\
						  openlog("PAM_sqlite3", LOG_PID, LOG_AUTH); \
						  syslog(LOG_DEBUG, ##x);					\
						  closelog();								\
					  }
#define SYSLOG(x...)  do {											\
						  openlog("PAM_sqlite3", LOG_PID, LOG_AUTH); \
						  syslog(LOG_INFO, ##x);					\
						  closelog();								\
					  } while(0)
#define SYSLOGERR(x...) SYSLOG("Error: " x)

typedef enum {
	PW_CLEAR = 1,
#if HAVE_MD5_CRYPT
	PW_MD5,
#endif
#if HAVE_SHA256_CRYPT
	PW_SHA256,
#endif
#if HAVE_SHA512_CRYPT
	PW_SHA512,
#endif
	PW_CRYPT,
} pw_scheme;

struct module_options {
	char *database;
	char *table;
	char *user_column;
	char *pwd_column;
	char *expired_column;
	char *newtok_column;
	pw_scheme pw_type;
	int debug;
	char *sql_verify;
	char *sql_check_expired;
	char *sql_check_newtok;
	char *sql_set_passwd;
};

#define FAIL(MSG) 		\
	{ 					\
		SYSLOGERR(MSG);	\
		free(buf); 		\
		return NULL; 	\
	}

#define GROW(x)		if (x > buflen - dest - 1) {       		\
	char *grow;                                        		\
	buflen += 256 + x;                                 		\
	grow = realloc(buf, buflen + 256 + x);             		\
	if (grow == NULL) FAIL("Out of memory building query"); \
	buf = grow;                                        		\
}

#define APPEND(str, len)	GROW(len); memcpy(buf + dest, str, len); dest += len
#define APPENDS(str)	len = strlen(str); APPEND(str, len)

#define MAX_ZSQL -1

/*
 * Being very defensive here. The current logic in the rest of the code should prevent this from
 * happening. But lets protect against future code changes which could cause a NULL ptr to creep
 * in.
 */
#define CHECK_STRING(str) 													 	\
	if (!str) 															    	\
		FAIL("Internal error in format_query: string ptr " #str " was NULL");

static char *format_query(const char *template, struct module_options *options,
	const char *user, const char *passwd)
{
	char *buf = malloc(256);
	if (!buf)
		return NULL;

	int buflen = 256;
	int dest = 0, len;
	const char *src = template;
	char *pct;
	char *tmp;

	while (*src) {
		pct = strchr(src, '%');

		if (pct) {
			/* copy from current position to % char into buffer */
			if (pct != src) {
				len = pct - src;
				APPEND(src, len);
			}

			/* decode the escape */
			switch(pct[1]) {
				case 'U':	/* username */
					if (user) {
						tmp = sqlite3_mprintf("%q", user);
						if (!tmp)
							FAIL("sqlite3_mprintf out of memory");
						len = strlen(tmp);
						APPEND(tmp, len);
						sqlite3_free(tmp);
					}
					break;
				case 'P':	/* password */
					if (passwd) {
						tmp = sqlite3_mprintf("%q", passwd);
						if (!tmp)
							FAIL("sqlite3_mprintf out of memory");
						len = strlen(tmp);
						APPEND(tmp, len);
						sqlite3_free(tmp);
					}
					break;

				case 'O':	/* option value */
					pct++;
					switch (pct[1]) {
						case 'p':	/* passwd */
							CHECK_STRING(options->pwd_column);
							APPENDS(options->pwd_column);
							break;
						case 'u':	/* username */
							CHECK_STRING(options->user_column);
							APPENDS(options->user_column);
							break;
						case 't':	/* table */
							CHECK_STRING(options->table);
							APPENDS(options->table);
							break;
						case 'x':	/* expired */
							CHECK_STRING(options->expired_column);
							APPENDS(options->expired_column);
							break;
						case 'n':	/* newtok */
							CHECK_STRING(options->newtok_column);
							APPENDS(options->newtok_column);
							break;
					}
					break;

				case '%':	/* quoted % sign */
					APPEND(pct, 1);
					break;

				default:	/* unknown */
					APPEND(pct, 2);
					break;
			}
			src = pct + 2;
		} else {
			/* copy rest of string into buffer and we're done */
			len = strlen(src);
			APPEND(src, len);
			break;
		}
	}

	buf[dest] = '\0';
	return buf;
}

static void
get_module_options_from_file(const char *filename, struct module_options *opts, int warn);

/*
 * safe_assign protects against duplicate config options causing a memory leak.
 */
static void inline
safe_assign(char **asignee, const char *val)
{
	if(*asignee)
		free(*asignee);
	*asignee = strdup(val);
}

/* private: parse and set the specified string option */
static void
set_module_option(const char *option, struct module_options *options)
{
	char *buf, *eq;
	char *val, *end;

	if(!option || !*option)
		return;

	buf = strdup(option);
	if(!buf)
		return;

	if((eq = strchr(buf, '='))) {
		end = eq - 1;
		val = eq + 1;
		if(end <= buf || !*val)
		{
			free(buf);
			return;
		}
		while(end > buf && isspace(*end))
			end--;
		end++;
		*end = '\0';
		while(*val && isspace(*val))
			val++;
	} else {
		val = NULL;
	}

	DBGLOG("setting option: %s=>%s\n", buf, val);

	if(!strcmp(buf, "database")) {
		safe_assign(&options->database, val);
	} else if(!strcmp(buf, "table")) {
		safe_assign(&options->table, val);
	} else if(!strcmp(buf, "user_column")) {
		safe_assign(&options->user_column, val);
	} else if(!strcmp(buf, "pwd_column")) {
		safe_assign(&options->pwd_column, val);
	} else if(!strcmp(buf, "expired_column")) {
		safe_assign(&options->expired_column, val);
	} else if(!strcmp(buf, "newtok_column")) {
		safe_assign(&options->newtok_column, val);
	} else if(!strcmp(buf, "pw_type")) {
		options->pw_type = PW_CLEAR;
		if(!strcmp(val, "crypt")) {
			options->pw_type = PW_CRYPT;
		}
#if HAVE_MD5_CRYPT
		else if(!strcmp(val, "md5")) {
			options->pw_type = PW_MD5;
		}
#endif
#if HAVE_SHA256_CRYPT
		else if(!strcmp(val, "sha-256")) {
			options->pw_type = PW_SHA256;
		}
#endif
#if HAVE_SHA512_CRYPT
		else if(!strcmp(val, "sha-512")) {
			options->pw_type = PW_SHA512;
		}
#endif
	} else if(!strcmp(buf, "debug")) {
		options->debug = 1;
	} else if (!strcmp(buf, "config_file")) {
		get_module_options_from_file(val, options, 1);
	} else if (!strcmp(buf, "sql_verify")) {
		safe_assign(&options->sql_verify, val);
	} else if (!strcmp(buf, "sql_check_expired")) {
		safe_assign(&options->sql_check_expired, val);
	} else if (!strcmp(buf, "sql_check_newtok")) {
		safe_assign(&options->sql_check_newtok, val);
	} else if (!strcmp(buf, "sql_set_passwd")) {
		safe_assign(&options->sql_set_passwd, val);
	} else {
		DBGLOG("ignored option: %s\n", buf);
	}

	free(buf);
}

/* private: read module options from a config file */
static void
get_module_options_from_file(const char *filename, struct module_options *opts, int warn)
{
	FILE *fp;

	if ((fp = fopen(filename, "r"))) {
		char line[1024];
		char *str, *end;

		while(fgets(line, sizeof(line), fp)) {
			str = line;
			end = line + strlen(line) - 1;
			while(*str && isspace(*str))
				str++;
			while(end > str && isspace(*end))
				end--;
			end++;
			*end = '\0';
			set_module_option(str, opts);
		}
		fclose(fp);
	} else if (warn) {
		SYSLOG("unable to read config file %s", filename);
	}
}

/* private: read module options from file or commandline */
static int
get_module_options(int argc, const char **argv, struct module_options **options)
{
	int i, rc;
	struct module_options *opts;

	rc = 0;
	if (!(opts = (struct module_options *)malloc(sizeof *opts))){
		*options = NULL;
		return rc;
	}

	bzero(opts, sizeof(*opts));
	opts->pw_type = PW_CLEAR;

	get_module_options_from_file(CONF, opts, 0);

	for(i = 0; i < argc; i++) {
		if(pam_std_option(&rc, argv[i]) == 0)
			continue;
		set_module_option(argv[i], opts);
	}
	*options = opts;

	return rc;
}

/* private: free module options returned by get_module_options() */
static void
free_module_options(struct module_options *options)
{
	if (!options)
		return;

	if(options->database)
		free(options->database);
	if(options->table)
		free(options->table);
	if(options->user_column)
		free(options->user_column);
	if(options->pwd_column)
		free(options->pwd_column);
	if(options->expired_column)
		free(options->expired_column);
	if(options->newtok_column)
		free(options->newtok_column);
	if(options->sql_verify)
		free(options->sql_verify);
	if(options->sql_check_expired)
		free(options->sql_check_expired);
	if(options->sql_check_newtok)
		free(options->sql_check_newtok);
	if(options->sql_set_passwd)
		free(options->sql_set_passwd);

	bzero(options, sizeof(*options));
	free(options);
}

/* private: make sure required options are present (in cmdline or conf file) */
static int
options_valid(struct module_options *options)
{
	if(!options)
	{
		SYSLOGERR("failed to read options.");
		return -1;
	}

	if(options->database == 0 || options->table == 0 || options->user_column == 0)
	{
		SYSLOGERR("the database, table and user_column options are required.");
		return -1;
	}
	return 0;
}

/* private: open SQLite database */
static sqlite3 *pam_sqlite3_connect(struct module_options *options)
{
  const char *errtext = NULL;
  sqlite3 *sdb = NULL;

  if (sqlite3_open(options->database, &sdb) != SQLITE_OK) {
      errtext = sqlite3_errmsg(sdb);
	  SYSLOG("Error opening SQLite database (%s)", errtext);
	  /*
	   * N.B. sdb is usually non-NULL when errors occur, so we explicitly
	   * release the resource and return NULL to indicate failure to the caller.
	   */

	  sqlite3_close(sdb);
	  return NULL;
  }

  return sdb;
}

/* private: generate random salt character */
static char *
crypt_make_salt(struct module_options *options)
{
	int i;
	time_t now;
	static unsigned long x;
	static char result[22];
	static char salt_chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./";
	static const int NUM_SALT_CHARS = sizeof(salt_chars) / sizeof(salt_chars[0]);

	time(&now);
	x += now + getpid() + clock();
	srandom(x);

	for (i=0; i<19; i++) {
		result[i] = salt_chars[random() % NUM_SALT_CHARS];
	}
	result[i+1] = '$';
	result[i+2]='\0';

	switch(options->pw_type) {
	case PW_CRYPT:
		result[2] = '\0';
		break;
#if HAVE_MD5_CRYPT
	case PW_MD5:
		result[0]='$';
		result[1]='1';
		result[2]='$';
		break;
#endif
#if HAVE_SHA256_CRYPT
	case PW_SHA256:
		result[0]='$';
		result[1]='5';
		result[2]='$';
		break;
#endif
#if HAVE_SHA512_CRYPT
	case PW_SHA512:
		result[0]='$';
		result[1]='6';
		result[2]='$';
		break;
#endif
	default:
		result[0] = '\0';
	}

	return result;
}

/* private: encrypt password using the preferred encryption scheme */
static char *
encrypt_password(struct module_options *options, const char *pass)
{
	char *s = NULL;

	switch(options->pw_type) {
#if HAVE_MD5_CRYPT
		case PW_MD5:
#endif
#if HAVE_SHA256_CRYPT
		case PW_SHA256:
#endif
#if HAVE_SHA512_CRYPT
		case PW_SHA512:
#endif
		case PW_CRYPT:
			s = strdup(crypt(pass, crypt_make_salt(options)));
			break;
		case PW_CLEAR:
		default:
			s = strdup(pass);
	}
	return s;
}

/* private: authenticate user and passwd against database */
static int
auth_verify_password(const char *user, const char *passwd,
					 struct module_options *options)
{
	int res;
	sqlite3 *conn = NULL;
	sqlite3_stmt *vm = NULL;
	int rc = PAM_AUTH_ERR;
	const char *tail  = NULL;
	const char *errtext = NULL;
	const char *encrypted_pw = NULL;
	char *query  = NULL;

	if(!(conn = pam_sqlite3_connect(options))) {
		rc = PAM_AUTH_ERR;
		goto done;
	}

	if(!(query = format_query(options->sql_verify ? options->sql_verify :
			"SELECT %Op FROM %Ot WHERE %Ou='%U'",
			options, user, passwd))) {
		SYSLOGERR("failed to construct sql query");
		rc = PAM_AUTH_ERR;
		goto done;
	}

	DBGLOG("query: %s", query);

	res = sqlite3_prepare(conn, query, MAX_ZSQL, &vm, &tail);

	free(query);

	if (res != SQLITE_OK) {
        errtext = sqlite3_errmsg(conn);
		DBGLOG("Error executing SQLite query (%s)", errtext);
		rc = PAM_AUTH_ERR;
		goto done;
	}

	if (SQLITE_ROW != sqlite3_step(vm)) {
		rc = PAM_USER_UNKNOWN;
		DBGLOG("no rows to retrieve");
	} else {
		const char *stored_pw = (const char *) sqlite3_column_text(vm, 0);

		if (!stored_pw) {
			SYSLOG("sqlite3 failed to return row data");
			rc = PAM_AUTH_ERR;
			goto done;
		}

		switch(options->pw_type) {
		case PW_CLEAR:
			if(strcmp(passwd, stored_pw) == 0)
				rc = PAM_SUCCESS;
			break;
#if HAVE_MD5_CRYPT
		case PW_MD5:
#endif
#if HAVE_SHA256_CRYPT
		case PW_SHA256:
#endif
#if HAVE_SHA512_CRYPT
		case PW_SHA512:
#endif
		case PW_CRYPT:
			encrypted_pw = crypt(passwd, stored_pw);
			if (!encrypted_pw) {
				SYSLOG("crypt failed when encrypting password");
				rc = PAM_AUTH_ERR;
				goto done;
			}

			if(strcmp(encrypted_pw, stored_pw) == 0)
				rc = PAM_SUCCESS;
			break;
		}
	}

done:
	sqlite3_finalize(vm);
	sqlite3_close(conn);
	return rc;
}

/* public: authenticate user */
PAM_EXTERN int
pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
	struct module_options *options = NULL;
	const char *user = NULL, *password = NULL, *service = NULL;
	int rc, std_flags;

	std_flags = get_module_options(argc, argv, &options);
	if(options_valid(options) != 0) {
		rc = PAM_AUTH_ERR;
		goto done;
	}

	if((rc = pam_get_user(pamh, &user, NULL)) != PAM_SUCCESS) {
		SYSLOG("failed to get username from pam");
		goto done;
	}

	DBGLOG("attempting to authenticate: %s", user);

	if((rc = pam_get_pass(pamh, &password, PASSWORD_PROMPT, std_flags)
		!= PAM_SUCCESS)) {
		goto done;
	}

	if((rc = auth_verify_password(user, password, options)) != PAM_SUCCESS)
		SYSLOG("(%s) user %s not authenticated.", pam_get_service(pamh, &service), user);
	else
		SYSLOG("(%s) user %s authenticated.", pam_get_service(pamh, &service), user);

done:
	free_module_options(options);
	return rc;
}

/* public: check if account has expired, or needs new password */
PAM_EXTERN int
pam_sm_acct_mgmt(pam_handle_t *pamh, int flags, int argc,
							const char **argv)
{
	struct module_options *options = NULL;
	const char *user = NULL;
	int rc = PAM_AUTH_ERR;
	sqlite3 *conn = NULL;
	sqlite3_stmt *vm = NULL;
	char *query = NULL;
	const char *tail = NULL;
	const char *errtext = NULL;
	int res;

	get_module_options(argc, argv, &options);
	if(options_valid(options) != 0) {
		rc = PAM_AUTH_ERR;
		goto done;
	}

	/* both not specified, just succeed. */
	if(options->expired_column == NULL && options->newtok_column == NULL) {
		rc = PAM_SUCCESS;
		goto done;
	}

	if((rc = pam_get_user(pamh, &user, NULL)) != PAM_SUCCESS) {
		SYSLOGERR("could not retrieve user");
		goto done;
	}

	if(!(conn = pam_sqlite3_connect(options))) {
		SYSLOGERR("could not connect to database");
		rc = PAM_AUTH_ERR;
		goto done;
	}

	/* if account has expired then expired_column = '1' or 'y' */
	if(options->expired_column || options->sql_check_expired) {

		if(!(query = format_query(options->sql_check_expired ? options->sql_check_expired :
				"SELECT 1 from %Ot WHERE %Ou='%U' AND (%Ox='y' OR %Ox='1')",
				options, user, NULL))) {
			SYSLOGERR("failed to construct sql query");
			rc = PAM_AUTH_ERR;
			goto done;
		}

		DBGLOG("query: %s", query);

		res = sqlite3_prepare(conn, query, MAX_ZSQL, &vm, &tail);

		free(query);

		if (res != SQLITE_OK) {
            errtext = sqlite3_errmsg(conn);
			SYSLOGERR("Error executing SQLite query (%s)", errtext);
			rc = PAM_AUTH_ERR;
			goto done;
		}

		res = sqlite3_step(vm);

		DBGLOG("query result: %d", res);

		if(SQLITE_ROW == res) {
			rc = PAM_ACCT_EXPIRED;
			goto done;
		}
		sqlite3_finalize(vm);
		vm = NULL;
	}

	/* if new password is required then newtok_column = 'y' or '1' */
	if(options->newtok_column || options->sql_check_newtok) {
		if(!(query = format_query(options->sql_check_newtok ? options->sql_check_newtok :
				"SELECT 1 FROM %Ot WHERE %Ou='%U' AND (%On='y' OR %On='1')",
				options, user, NULL))) {
			SYSLOGERR("failed to construct sql query");
			rc = PAM_AUTH_ERR;
			goto done;
		}

		DBGLOG("query: %s", query);

		res = sqlite3_prepare(conn, query, MAX_ZSQL, &vm, &tail);
		free(query);

		if (res != SQLITE_OK) {
            errtext = sqlite3_errmsg(conn);
			SYSLOGERR("query failed: %s", errtext);
			rc = PAM_AUTH_ERR;
			goto done;
		}

		res = sqlite3_step(vm);

		if(SQLITE_ROW == res) {
			rc = PAM_NEW_AUTHTOK_REQD;
			goto done;
		}
		sqlite3_finalize(vm);
		vm = NULL;
	}

	rc = PAM_SUCCESS;

done:
	/* Do all cleanup in one place. */
	sqlite3_finalize(vm);
	sqlite3_close(conn);
	free_module_options(options);
	return rc;
}

/* public: change password */
PAM_EXTERN int
pam_sm_chauthtok(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
	struct module_options *options = NULL;
	int rc = PAM_AUTH_ERR;
	int std_flags;
	const char *user = NULL, *pass = NULL, *newpass = NULL, *service = NULL;
	char *newpass_crypt = NULL;
	sqlite3 *conn = NULL;
	char *errtext = NULL;
	char *query = NULL;
	int res;

	std_flags = get_module_options(argc, argv, &options);
	if(options_valid(options) != 0) {
		rc = PAM_AUTH_ERR;
		goto done;
	}

	if((rc = pam_get_user(pamh, &user, NULL)) != PAM_SUCCESS) {
		SYSLOGERR("could not retrieve user");
		goto done;
	}

	if(flags & PAM_PRELIM_CHECK) {
		/* at this point, this is the first time we get called */
		if((rc = pam_get_pass(pamh, &pass, PASSWORD_PROMPT, std_flags)) == PAM_SUCCESS) {
			if((rc = auth_verify_password(user, pass, options)) == PAM_SUCCESS) {
				rc = pam_set_item(pamh, PAM_OLDAUTHTOK, (const void *)pass);
				if(rc != PAM_SUCCESS) {
					SYSLOGERR("failed to set PAM_OLDAUTHTOK!");
				}
				goto done;
			} else {
				SYSLOG("password verification failed for '%s'", user);
				goto done;
			}
		} else {
			SYSLOGERR("could not retrieve password from '%s'", user);
			goto done;
		}
	} else if(flags & PAM_UPDATE_AUTHTOK) {
		rc = pam_get_item(pamh, PAM_OLDAUTHTOK, (const void **) &pass);
		if(rc != PAM_SUCCESS) {
			SYSLOGERR("could not retrieve old token");
			goto done;
		}
		rc = auth_verify_password(user, pass, options);
		if(rc != PAM_SUCCESS) {
			SYSLOG("(%s) user '%s' not authenticated.", pam_get_service(pamh, &service), user);
			goto done;
		}

		/* get and confirm the new passwords */
		rc = pam_get_confirm_pass(pamh, &newpass, PASSWORD_PROMPT_NEW, PASSWORD_PROMPT_CONFIRM, std_flags);
		if(rc != PAM_SUCCESS) {
			SYSLOGERR("could not retrieve new authentication tokens");
			goto done;
		}

		/* save the new password for subsequently stacked modules */
		rc = pam_set_item(pamh, PAM_AUTHTOK, (const void *)newpass);
		if(rc != PAM_SUCCESS) {
			SYSLOGERR("failed to set PAM_AUTHTOK!");
			goto done;
		}

		/* update the database */
		if(!(newpass_crypt = encrypt_password(options, newpass))) {
			SYSLOGERR("passwd encrypt failed");
			rc = PAM_BUF_ERR;
			goto done;
		}
		if(!(conn = pam_sqlite3_connect(options))) {
			SYSLOGERR("could not connect to database");
			rc = PAM_AUTHINFO_UNAVAIL;
			goto done;
		}

		DBGLOG("creating query");

		if(!(query = format_query(options->sql_set_passwd ? options->sql_set_passwd :
				"UPDATE %Ot SET %Op='%P' WHERE %Ou='%U'",
				options, user, newpass_crypt))) {
			SYSLOGERR("failed to construct sql query");
			rc = PAM_AUTH_ERR;
			goto done;
		}

		DBGLOG("query: %s", query);

		res = sqlite3_exec(conn, query, NULL, NULL, &errtext);
		free(query);

		if (SQLITE_OK != res) {
			SYSLOGERR("query failed[%d]: %s", res, errtext);
            sqlite3_free(errtext);  // error strings rom sqlite3_exec must be freed
			rc = PAM_AUTH_ERR;
			goto done;
		}

		/* if we get here, we must have succeeded */
	}

	SYSLOG("(%s) password for '%s' was changed.", pam_get_service(pamh, &service), user);
	rc = PAM_SUCCESS;

done:
	/* Do all cleanup in one place. */
	sqlite3_close(conn);
	if (newpass_crypt != NULL)
		memzero_explicit(newpass_crypt, strlen(newpass_crypt));
	free(newpass_crypt);
	free_module_options(options);
	return rc;
}

/* public: just succeed. */
PAM_EXTERN int
pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
	return PAM_SUCCESS;
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: sw=4 ts=4 fdm=marker
 * vim<600: sw=4 ts=4
 */
