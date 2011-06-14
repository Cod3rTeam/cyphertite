/* $cyphertite$ */
/*
 * Copyright (c) 2011 Conformal Systems LLC <info@conformal.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef NEED_LIBCLENS
#include <clens.h>
#endif

#include <stdio.h>
#include <pwd.h>
#include <readpassphrase.h>

#include <sys/stat.h>

#ifndef NO_UTIL_H
#include <util.h>
#endif

#include <netinet/in.h>
#include <arpa/inet.h>

#include <clog.h>

#include "ctutil.h"

__attribute__((__unused__)) static const char *cvstag = "$cyphertite$";


int			ct_settings_add(struct ct_settings *, char *, char *);
uint8_t			ct_getbyteval(char);

int
ct_get_password(char *password, size_t passwordlen, char *prompt, int verify)
{
	int			rv = 1;
	char			pw[_PASSWORD_LEN + 1];

	if (readpassphrase(prompt ? prompt : "New password: ", password,
	    passwordlen, RPP_REQUIRE_TTY) == NULL) {
		CWARNX("invalid password");
		goto done;
	}
	if (verify) {
		if (readpassphrase("Retype password: ", pw, sizeof pw,
		    RPP_REQUIRE_TTY) == NULL) {
			CWARNX("invalid password");
			goto done;
		}
		if (strcmp(password, pw)) {
			CWARNX("passwords do not match");
			goto done;
		}
	}
	if (strlen(password) == 0) {
		CWARNX("password must not be empty");
		goto done;
	}

	rv = 0;
done:
	bzero(pw, sizeof pw);
	return (rv);
}
uint8_t
ct_getbyteval(char c)
{
	if (c == '\0')
		return (0xff);
	else if (c >= '0' && c <= '9')
		return (c - '0');
	else if (toupper(c) >= 'A' && toupper(c) <= 'F')
		return (toupper(c) - 'A' + 10);
	else
		return (0xff);
}

int
ct_text2sha(char *shat, uint8_t *sha)
{
	int			i, x;
	uint8_t			v1, v2;

	for (i = 0, x = 0; i < SHA_DIGEST_LENGTH * 2; i += 2, x++) {
		v1 = ct_getbyteval(shat[i]);
		if (v1 == 0xff)
			return (1);
		v2 = ct_getbyteval(shat[i + 1]);
		if (v2 == 0xff)
			return (1);
		sha[x] = (v1 << 4) | v2;
	}
	if (shat[i] != '\0')
		return (1);

	return (0);
}
void
ct_wire_header(struct ct_header *h)
{
	if (h == NULL)
		CFATALX("invalid pointer");

	h->c_tag = htonl(h->c_tag);
	h->c_flags = htons(h->c_flags);
	h->c_size = htonl(h->c_size);
}

void
ct_unwire_header(struct ct_header *h)
{
	if (h == NULL)
		CFATALX("invalid pointer");

	h->c_tag = ntohl(h->c_tag);
	h->c_flags = ntohs(h->c_flags);
	h->c_size = ntohl(h->c_size);
}

void
ct_unwire_nop(struct ct_nop *n)
{
	if (n == NULL)
		CFATALX("invalid pointer");

	n->cn_id = ntohl(n->cn_id);
}

void
ct_wire_nop_reply(struct ct_nop_reply *n)
{
	if (n == NULL)
		CFATALX("invalid pointer");

	n->cnr_id = htonl(n->cnr_id);
}

void
ct_expand_tilde(struct ct_settings *cs, char **s, char *val)
{
	char			*uid_s;
	struct			passwd *pwd;
	int			i;
	uid_t			uid;

	if (cs == NULL || s == NULL)
		CFATALX("invalid parameter");

	if (val[0] == '~' && strlen(val) > 1) {
		if ((uid = getuid()) == 0) {
			/* see if we are using sudo and get caller uid */
			uid_s = getenv("SUDO_UID");
			if (uid_s)
				uid = atoi(uid_s);
		}
		pwd = getpwuid(uid);
		if (pwd == NULL)
			CFATALX("invalid user %d", uid);

		i = 1;
		while (val[i] == '/' && val[i] != '\0')
			i++;

		if (asprintf(s, "%s/%s", pwd->pw_dir, &val[i]) == -1)
			CFATALX("no memory for %s", cs->cs_name);
	} else
		*s = strdup(val);
}

int
ct_settings_add(struct ct_settings *settings, char *var, char *val)
{
	int			rv = 1, *p;
	double			*f;
	char			**s;
	struct ct_settings	*cs;

	if (settings == NULL)
		CFATALX("invalid parameters");

	for (cs = settings; cs->cs_name != NULL; cs++) {
		if (strcmp(var, cs->cs_name))
			continue;

		if (cs->cs_s) {
			if (cs->cs_s->csp_set(cs, val))
				CFATALX("invalid value for %s: %s", var, val);
			rv = 0;
			break;
		} else
			switch (cs->cs_type) {
			case CT_S_INT:
				p = cs->cs_ival;
				*p = atoi(val);
				rv = 0;
				break;
			case CT_S_DIR:
			case CT_S_STR:
				s = cs->cs_sval;
				if (s == NULL)
					CFATALX("invalid sval for %s",
					    cs->cs_name);
				if (*s)
					free(*s);
				if (cs->cs_type == CT_S_DIR)
					ct_expand_tilde(cs, s, val);
				else
					*s = strdup(val);

				if (s == NULL)
					CFATAL("no memory for %s", cs->cs_name);
				rv = 0;
				break;
			case CT_S_FLOAT:
				f = cs->cs_fval;
				*f = atof(val);
				rv = 0;
				break;
			case CT_S_INVALID:
			default:
				CFATALX("invalid type for %s", var);
			}
		break;
	}

	return (rv);
}

#define	WS	"\n= \t"
int
ct_config_parse(struct ct_settings *settings, const char *filename)
{
	FILE			*config;
	char			*line, *cp, *var, *val;
	size_t			len, lineno = 0;
	char			*v1, *v2;

	CDBG("filename %s\n", filename);

	if (filename == NULL)
		CFATALX("invalid config file name");

	if ((config = fopen(filename, "r")) == NULL)
		return 1;

	for (;;) {
		if ((line = fparseln(config, &len, &lineno, NULL, 0)) == NULL)
			if (feof(config) || ferror(config))
				break;

		cp = line;
		cp += (long)strspn(cp, WS);
		if (cp[0] == '\0') {
			/* empty line */
			free(line);
			continue;
		}

		if ((var = strsep(&cp, WS)) == NULL || cp == NULL)
			CFATALX("invalid config file entry: %s", line);

		cp += (long)strspn(cp, WS);

		if ((val = strsep(&cp, "\0")) == NULL)
			break;

		for (v1 = v2 = val; *v1 != '\0'; v1++) {
			if (!isspace(*v1))
				*v2++ = *v1;
		}
		*v2 = '\0';

		CDBG("config_parse: %s=%s\n",var ,val);
		if (ct_settings_add(settings, var, val))
			CFATALX("invalid conf file entry: %s=%s", var, val);

		free(line);
	}
	CDBG("loaded config from %s", filename);

	fclose(config);

	return 0;
}

void
ct_dump_block(uint8_t *p, size_t sz)
{
	char			*fp, *buf;
	int			bsz, i, j;

	bsz = 16 * 4 + 3 + 2;
	fp = buf = malloc(bsz);
	char *sep = "";
	CDBG("dumping %p %zu", p, sz);
	for (i = 0; i < sz; i += 16) {
		sep = "";
		for (j = 0; j < 16; j++) {
			if (i + j < sz) {
				fp += snprintf(fp, bsz - (fp-buf), "%s%02x",
				    sep, p[i+j]);
			} else {
				fp += snprintf(fp, bsz - (fp-buf), "%s  ", sep);
			}
			sep = " ";
		}
		*fp++ = ' ';
		*fp++ = ' ';
		for (j = 0; j < 16; j++) {
			if (i + j < sz)
				fp += snprintf(fp, bsz - (fp-buf), "%c",
				    isgraph(p[i+j]) ? p[i+j] : ' ');
		}
		CDBG("%s", buf);
		fp = buf;
	}
	free(buf);
}

void
ct_polltype_setup(const char *type)
{
	if (type == NULL)
		return;

	setenv("EVENT_NOKQUEUE", "1", 1);
	setenv("EVENT_NOPOLL", "1", 1);
	setenv("EVENT_NOSELECT", "1", 1);

	if (strcmp(type, "kqueue") == 0)
		unsetenv("EVENT_NOKQUEUE");
	else if (strcmp(type, "poll") == 0)
		unsetenv("EVENT_NOPOLL");
	else if (strcmp(type, "select") == 0)
		unsetenv("EVENT_NOSELECT");
	else
		CFATALX("unknown poll type %s", type);

	CDBG("polltype: %s\n", type);
}

/*
 * make all directories in the full path provided in ``path'' if they don't
 * exist.
 * returns 0 on success.
 */
int
ct_make_full_path(char *path, mode_t mode)
{
	char		*nxt = path;
	struct stat	 st;

	/* deal with full paths */
	if (*nxt == '/')
		nxt++;
	for (;;) {
		if ((nxt = strchr(nxt, '/')) == NULL)
			break;
		*nxt = '\0';

		if (lstat(path, &st) == 0) {
			*(nxt++) = '/';
			continue;
		}

		if (mkdir(path, mode) == -1)
			return (1);
		*(nxt++) = '/';
		/* XXX stupid umask? */
	}

	return (0);
}
