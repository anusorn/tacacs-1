/*
 * Copyright (C) 2014, 2015 Cumulus Networks, Inc
 * Author: Dave Olson <olson@cumulusnetworks.com>
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program - see the file COPYING.
 */

/*
 * This plugin implements getpwnam_r for NSS over TACACS+
 * and implements getpwuid_r for UIDs if and only if a mapped
 * TACACS+ user is currently logged in (libtacplus_map)
 * This means that if you do, e.g.: ls -ld ~tacacs15, you will
 * sometimes get a mapped username, and other times get tacacs15,
 * depending on whether a mapped user is logged in or not.
 */


#include <string.h>
#include <syslog.h>
#include <stdlib.h>
#include <pwd.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <nss.h>
#include <libaudit.h>

#include <tacplus/libtac.h>
#include <tacplus/map_tacplus_user.h>

#include "nss_tacplus.h"

static const char *nssname = "nss_tacplus"; /* for syslogs */
static const char *config_file = "/etc/tacplus_nss.conf";

/*
 * pwbuf is used to reduce number of arguments passed around; the strings in
 * the passwd struct need to point into this buffer.
 */
struct pwbuf {
    char *name;
    char *buf;
    struct passwd *pw;
    int *errnop;
    size_t buflen;
};

typedef struct {
    struct addrinfo *addr;
    const char *key;
} tacplus_server_t;

/* set from configuration file parsing */
static tacplus_server_t tac_srv[TAC_PLUS_MAXSERVERS];
static int tac_srv_no;
static char tac_service[] = "shell";
static char tac_protocol[] = "ssh";
static int debug;
static int conf_parsed = 0;

static int nss_tacplus_config(int *errnop, const char *cfile, int top)
{
    FILE *conf;
    int tac_key_no = 0;
    char lbuf[256];

    if(conf_parsed > 1) /* 1: we've tried and thrown errors, 2, OK */
        return 0;

    conf = fopen(cfile, "r");
    if(conf == NULL) {
        *errnop = errno;
        if(!conf_parsed)
            syslog(LOG_WARNING, "%s: can't open config file %s: %m",
                nssname, cfile); 
        goto err;
    }

    while (fgets(lbuf, sizeof lbuf, conf)) {
        if(*lbuf == '#' || isspace(*lbuf))
            continue; /* skip comments, white space lines, etc. */
        strtok(lbuf, " \t\n\r\f"); /* terminate buffer at first whitespace */
        if(!strncmp (lbuf, "include=", 8)) {
            /*
             * allow include files, useful for centralizing tacacs
             * server IP address and secret.
             */
            if(lbuf[8]) /* else treat as empty config, ignoring errors */
                (void)nss_tacplus_config(errnop, &lbuf[8], top+1);
        }
        else if(!strncmp (lbuf, "debug=", 6)) 
            debug = strtoul(lbuf+6, NULL, 0);
        /*
         * This next group is here to prevent a warning in the
         * final "else" case.  We don't need them, but if there
         * is a common included file, we might see them.
         */
        else if(!strncmp (lbuf, "service=", 8) ||
            !strncmp (lbuf, "protocol=", 9) ||
            !strncmp (lbuf, "login=", 6))
            ;
        else if(!strncmp (lbuf, "secret=", 7)) {
            /* no need to complain if too many on this one */
            if(tac_key_no < TAC_PLUS_MAXSERVERS) {
                if((tac_srv[tac_key_no].key = strdup(lbuf+7)))
                    tac_key_no++;
                else
                    syslog(LOG_ERR, "%s: unabled to copy server secret %s",
                        nssname, lbuf+7);
            }
        }
        else if(!strncmp (lbuf, "server=", 7)) {
            if(tac_srv_no < TAC_PLUS_MAXSERVERS) {
                struct addrinfo hints, *servers, *server;
                int rv;
                char *port, server_buf[sizeof lbuf];

                memset(&hints, 0, sizeof hints);
                hints.ai_family = AF_UNSPEC;  /* use IPv4 or IPv6, whichever */
                hints.ai_socktype = SOCK_STREAM;

                strcpy(server_buf, lbuf + 7);

                port = strchr(server_buf, ':');
                if(port != NULL) {
                    *port = '\0';
					port++;
                }
                if((rv = getaddrinfo(server_buf, (port == NULL) ?
                            "49" : port, &hints, &servers)) == 0) {
                    for(server = servers; server != NULL &&
                        tac_srv_no < TAC_PLUS_MAXSERVERS;
                        server = server->ai_next) {
                        tac_srv[tac_srv_no].addr = server;
                        if(tac_key_no && tac_srv_no != (tac_key_no-1))
                            /* use current key if set, and not the same index */
                            tac_srv[tac_srv_no].key = tac_srv[tac_key_no-1].key;
                        tac_srv_no++;
                    }
                }
                else {
                    syslog(LOG_ERR,
                        "%s: skip invalid server: %s (getaddrinfo: %s)",
                        nssname, server_buf, gai_strerror(rv));
                }
            }
            else {
                syslog(LOG_WARNING, "%s: maximum number of servers (%d) "
                    "exceeded, skipping", nssname, TAC_PLUS_MAXSERVERS);
            }
        }
        else if(debug) /* ignore unrecognized lines, unless debug on */
            syslog(LOG_WARNING, "%s: unrecognized parameter: %s",
                nssname, lbuf);
    }
    fclose(conf);

    if(top == 1) {
        int n;
        if(tac_srv_no == 0)
            syslog(LOG_WARNING, "%s:%s: no TACACS %s in config, giving up",
                nssname, __FUNCTION__, tac_srv_no ? "service" :
                (*tac_service ? "server" : "service and no server"));

        for(n = 0; debug && n < tac_srv_no; n++)
            syslog(LOG_DEBUG, "%s: server[%d] { addr=%s, key='%s' }",
                nssname, n, tac_ntop(tac_srv[n].addr->ai_addr), tac_srv[n].key);
    }

    return 0;

err:
    if(conf)
            fclose(conf);
    return 1;
}


/*
 * copy a passwd structure and it's strings, using the provided buffer
 * for the strings.
 * if usename is non-NULL, use that, rather than pw_name in srcpw, so we can
 * preserve the original requested name (this is part of the tacacs remapping).
 * For strings, if pointer is null, use an empty string.
 * Returns 0 if everything fit, otherwise 1.
 */
static int
pwcopy(char *buf, size_t len, struct passwd *srcpw, struct passwd *destpw,
       char *usename)
{
    int needlen, cnt;

    if (!usename)
        usename = srcpw->pw_name;

    needlen = usename ? strlen(usename) + 1 : 1 + 
        srcpw->pw_dir ? strlen(srcpw->pw_dir) + 1 : 1 +
        srcpw->pw_gecos ? strlen(srcpw->pw_gecos) + 1 : 1 +
        srcpw->pw_shell ? strlen(srcpw->pw_shell) + 1 : 1 +
        srcpw->pw_passwd ? strlen(srcpw->pw_passwd) + 1 : 1;
    if (needlen > len) {
        if (debug)
            syslog(LOG_DEBUG, "%s provided password buffer too small (%ld<%d)",
                nssname, len, needlen);
        return 1;
    }

    destpw->pw_uid = srcpw->pw_uid;
    destpw->pw_gid = srcpw->pw_gid;

    cnt = snprintf(buf, len, "%s", usename ? usename : "");
    destpw->pw_name = buf;
    cnt++; /* allow for null byte also */
    buf += cnt;
    len -= cnt;
    cnt = snprintf(buf, len, "%s", srcpw->pw_passwd ? srcpw->pw_passwd : "");
    destpw->pw_passwd = buf;
    cnt++;
    buf += cnt;
    len -= cnt;
    cnt = snprintf(buf, len, "%s", srcpw->pw_shell ? srcpw->pw_shell : "");
    destpw->pw_shell = buf;
    cnt++;
    buf += cnt;
    len -= cnt;
    cnt = snprintf(buf, len, "%s", srcpw->pw_gecos ? srcpw->pw_gecos : "");
    destpw->pw_gecos = buf;
    cnt++;
    buf += cnt;
    len -= cnt;
    cnt = snprintf(buf, len, "%s", srcpw->pw_dir ? srcpw->pw_dir : "");
    destpw->pw_dir = buf;
    cnt++;
    buf += cnt;
    len -= cnt;

    return 0;
}

/*
 * Find the username or the matching tacacs privilege user in /etc/passwd
 * We use fgetpwent() so we can check the local file, always.
 * This could cause problems if somebody is using local users, ldap, and tacacs,
 * but we just require that the mapped user always be a local user.  Since the
 * local user password isn't supposed to be used, that should be OK.
 *
 * We shouldn't normally find the username, because tacacs lookup should be
 * configured to follow local in nsswitch.conf, but somebody may configure the
 * other way, so we look for both the given user, and our "matching" user name
 * based on the tacacs authorization level.
 *
 * If not found, then try to map to a localuser tacacsN where N <= to the
 * TACACS+ privilege level, using the APIs in libtacplus_map.so
 * algorithm in update_mapuser()
 * Returns 0 on success, else 1
 */
static int
find_pw_userpriv(unsigned priv, struct pwbuf *pb)
{
    FILE *pwfile;
    struct passwd upw, tpw, *ent;
    int matches, ret, retu, rett;
    unsigned origpriv = priv;
    char ubuf[pb->buflen], tbuf[pb->buflen];
    char tacuser[9]; /* "tacacs" followed by 1-2 digits */

    tacuser[0] = '\0';

    pwfile = fopen("/etc/passwd", "r");
    if (!pwfile) {
        syslog(LOG_WARNING, "%s: failed to open /etc/passwd: %m",
            nssname);
        return 1;
    }

recheck:
    snprintf(tacuser, sizeof tacuser, "tacacs%u", priv);
    tpw.pw_name = upw.pw_name = NULL;
    retu = 0, rett = 0;
    for (matches=0; matches < 2 && (ent = fgetpwent(pwfile)); ) {
        if(!ent->pw_name)
            continue; /* shouldn't happen */
        if(!strcmp(ent->pw_name, pb->name)) {
            retu = pwcopy(ubuf, sizeof(ubuf), ent, &upw, NULL);
            matches++;
        }
        else if(!strcmp(ent->pw_name, tacuser)) {
            rett = pwcopy(tbuf, sizeof(tbuf), ent, &tpw, NULL);
            matches++;
        }
    }
    if(!matches && priv > 0) {
        priv--;
        rewind(pwfile);
        goto recheck;
    }
    ret = 1;
    fclose(pwfile);
    if(matches)  {
        if(priv != origpriv && debug)
            syslog(LOG_DEBUG, "%s: local user not found at privilege=%u,"
                " using %s", nssname, origpriv, tacuser);
        if(upw.pw_name && !retu)
            ret = pwcopy(pb->buf, pb->buflen, &upw, pb->pw, pb->name);
        else if(tpw.pw_name && !rett)
            ret = pwcopy(pb->buf, pb->buflen, &tpw, pb->pw, pb->name);
    }
    if (ret)
       *pb->errnop = ERANGE;

    return ret;
}

/*
 * This is similar to find_pw_userpriv(), but passes in a fixed
 * name for UID lookups, where we have the mapped name from the
 * map file, so trying multiple tacacsN users would be wrong.
 * Some commonality, but ugly to factor
 * Only applies to mapped users
 * returns 0 on success
 */
static int
find_pw_user(char *logname, char *tacuser, struct pwbuf *pb)
{
    FILE *pwfile;
    struct passwd *ent;
    int ret = 1;

    if (!tacuser) {
        syslog(LOG_DEBUG, "%s: passed null username, failing",
            nssname);
        return 1;
    }

    pwfile = fopen("/etc/passwd", "r");
    if (!pwfile) {
        syslog(LOG_WARNING, "%s: failed to open /etc/passwd: %m",
            nssname);
        return 1;
    }

    pb->pw->pw_name = NULL; /* be paranoid */
    for (ret = 1; ret && (ent = fgetpwent(pwfile)); ) {
        if(!ent->pw_name)
            continue; /* shouldn't happen */
        if(!strcmp(ent->pw_name, tacuser)) {
            ret = pwcopy(pb->buf, pb->buflen, ent, pb->pw, logname);
            break;
        }
    }
    fclose(pwfile);
    if (ret)
       *pb->errnop = ERANGE;

    return ret;
}

/*
 * we got the user back.  Go through the attributes, find their privilege
 * level, map to the local user, fill in the data, etc.
 * Returns 0 on success, 1 on errors.
 */
static int
got_tacacs_user(struct tac_attrib *attr, struct pwbuf *pb)
{
    unsigned long priv_level = 0;

    while (attr != NULL)  {
        /* we are looking for the privilege attribute, can be in several forms,
         * typically priv-lvl= or priv_lvl= */
        if(strncasecmp(attr->attr, "priv", 4) == 0) {
            char *ok, *val;

            for(val=attr->attr; *val && *val != '*' && *val != '='; val++)
                ;
            if (!*val)
                continue;
            val++;

            priv_level = strtoul(val, &ok, 0);

            /* if this fails, we leave priv_level at 0, which is
             * least privileged, so that's OK, but at least report it
             */
            if (ok == val)
                syslog(LOG_WARNING, "%s: non-numeric privilege for %s, (%s)",
                    nssname, pb->name, attr->attr);
        }
        attr = attr->next;
    }

    return find_pw_userpriv(priv_level, pb);
}

/*
 * find the first responding tacacs server, and return the fd.
 * Since we may be looking up multiple users, we leave the connection open,
 * once found.
 * Returns fd for connection, or -1 on failure
 */
static int
connect_tacacs(struct tac_attrib **attr)
{
    int srvr, fd;

    if(!*tac_service) /* reported at config file processing */
        return -1;
    for(srvr = 0; srvr < tac_srv_no; srvr++) {
        fd = tac_connect_single(tac_srv[srvr].addr, tac_srv[srvr].key, NULL);
        if(fd >= 0) {
            *attr = NULL; /* so tac_add_attr() allocates memory */
            tac_add_attrib(attr, "service", tac_service);
            if(tac_protocol[0])
                tac_add_attrib(attr, "protocol", tac_protocol);
            /* empty cmd is required, at least for linux tac_plus */
            tac_add_attrib(attr, "cmd", "");
            return fd;
        }
    }
    return -1;
}


/*
 * lookup the user on a TACACS server.  Returns 0 on successful lookup, else 1
 *
 * We have to make a new connection each time, because libtac is single threaded
 * (doesn't support multiple connects at the same time due to use of globals)),
 * and doesn't have support for persistent connections.   That's fixable, but
 * not worth the effort at this point.
 */
static int
lookup_tacacs_user(struct pwbuf *pb)
{
    struct areply arep;
    int ret;
    struct tac_attrib *attr;
    int tac_fd;

    if ((tac_fd = connect_tacacs(&attr)) == -1)
        return 1;

    ret = tac_author_send(tac_fd, pb->name, "", "", attr);
    if(ret < 0) {
        if(debug)
            syslog (LOG_WARNING, "%s: TACACS+ send failed (%d) for [%s]: %m",
                nssname, ret, pb->name);
    }
    else 
        tac_author_read(tac_fd, &arep);

    tac_free_attrib(&attr); 
    close(tac_fd);
    if (ret < 0)
        return 1;

    if(arep.status == AUTHOR_STATUS_PASS_ADD ||
       arep.status == AUTHOR_STATUS_PASS_REPL)
        ret = got_tacacs_user(arep.attr, pb);
    else
        ret = 1;
    if(arep.msg)
        free (arep.msg);
    if(arep.attr) /* free returned attributes */
        tac_free_attrib(&arep.attr);
    
    return ret;
}

static int
lookup_mapped_uid(struct pwbuf *pb, uid_t uid, uid_t auid, int session)
{
    char *loginname, mappedname[256];

    loginname = lookup_mapuid(uid, auid, session,
                            mappedname, sizeof mappedname);
    if (loginname)
        return find_pw_user(loginname, mappedname, pb);
    return 1;
}

/*
 * This is an NSS entry point. 
 * We implement getpwnam(), because we remap from the tacacs login
 * to the local tacacs0 ... tacacs15 users for all other info, and so
 * the normal order of "passwd tacplus" (possibly with ldap or anything
 * else prior to tacplus) will mean we only get used when there isn't
 * a local user to be found.
 */
enum nss_status _nss_tacplus_getpwnam_r (const char *name, struct passwd *pw,
    char *buffer, size_t buflen, int *errnop)
{
    enum nss_status status = NSS_STATUS_NOTFOUND;
    int result;
    struct pwbuf pb;

    result = nss_tacplus_config(errnop, config_file, 1);
    conf_parsed = result == 0 ? 2 : 1;

    if(result) { /* no config file, no servers, etc. */
        syslog(LOG_WARNING, "%s: bad config or server line for nss_tacplus",
            nssname);
    }
    else {
        /* marshal the args for the lower level functions */
        pb.name = (char *)name;
        pb.pw = pw;
        pb.buf = buffer;
        pb.buflen = buflen;
        pb.errnop = errnop;

       if(!lookup_tacacs_user(&pb))
           status =  NSS_STATUS_SUCCESS;
    }
   return status;
}

/*
 * This is an NSS entry point. 
 * We implement getpwuid(), for anything that wants to get the original
 * login name from the uid.
 * If it matches an entry in the map, we use that data to replace
 * the data from the local passwd file (not via NSS).
 * locally from the map.
 *
 * This can be made to work 2 different ways, and we need to choose
 * one, or make it configurable.
 *
 * 1) Given a valid auid and a session id, and a mapped user logged in,
 * we'll match only that user.   That is, we can only do the lookup
 * successfully for child processes of the mapped tacacs login, and
 * only while still logged in (map entry is valid).
 *
 * 2) Use auid/session wildcards, and and always match on the first valid
 * tacacs map file entry.  This means if two tacacs users are logged in
 * at the same privilege level at the same time, uid lookups for ps, ls,
 * etc. will return the first (in the map file, not necessarily first
 * logged in) mapped name.
 *
 * For now, if auid and session are set, I try them, and if that lookup
 * fails, try the wildcard.
 *
 * Only works while the UID is in use for a mapped user, and only
 * for processes invoked from that session.  Other callers will
 * just get the files, ldap, or nis entry for the UID
 * Only works while the UID is in use for a mapped user, and returns
 * the first match from the mapped users.
 */
enum nss_status _nss_tacplus_getpwuid_r (uid_t uid, struct passwd *pw,
    char *buffer, size_t buflen, int *errnop)
{
    struct pwbuf pb;
    enum nss_status status = NSS_STATUS_NOTFOUND;
    int session, ret;
    uid_t auid;

    /* we only need debug for this */
    ret = nss_tacplus_config(errnop, config_file, 1);
    conf_parsed = ret == 0 ? 2 : 1;

    auid = audit_getloginuid(); /* audit_setloginuid not called */
    session = map_get_sessionid();

    /* marshal the args for the lower level functions */
    pb.pw = pw;
    pb.buf = buffer;
    pb.buflen = buflen;
    pb.errnop = errnop;
    pb.name = NULL;

    /*
     * the else case will only be called if we don't have an auid or valid
     * sessionid, since otherwise the first call will be using wildcards,
     * since the getloginuid and get_sessionid calls will "fail".
     */
    if(!lookup_mapped_uid(&pb, uid, auid, session))
        status = NSS_STATUS_SUCCESS;
    else if((auid != (uid_t)-1 || session != ~0U) &&
        !lookup_mapped_uid(&pb, uid, (uid_t)-1, ~0))
        status = NSS_STATUS_SUCCESS;
    return status;
}
