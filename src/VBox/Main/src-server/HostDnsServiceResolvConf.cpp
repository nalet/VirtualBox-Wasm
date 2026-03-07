/* $Id: HostDnsServiceResolvConf.cpp 112403 2026-01-11 19:29:08Z knut.osmundsen@oracle.com $ */
/** @file
 * Base class for Host DNS & Co services.
 */

/*
 * Copyright (C) 2014-2026 Oracle and/or its affiliates.
 *
 * This file is part of VirtualBox base platform packages, as
 * available from https://www.virtualbox.org.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, in version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses>.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* -*- indent-tabs-mode: nil; -*- */
#include <VBox/com/string.h>
#include <VBox/com/ptr.h>

#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


#include <iprt/assert.h>
#include <iprt/errcore.h>
#include <iprt/critsect.h>
#include <iprt/ctype.h>
#include <iprt/file.h>
#include <iprt/net.h>
#include <iprt/stream.h>

#include <VBox/log.h>

#include "HostDnsService.h"


#define RCPS_MAX_NAMESERVERS 3
#define RCPS_MAX_SEARCHLIST 10
#define RCPS_BUFFER_SIZE 256
#define RCPS_IPVX_SIZE 47


struct HostDnsServiceResolvConf::Data
{
    Data(const char *fileName)
        : resolvConfFilename(fileName)
    {
    };

    com::Utf8Str resolvConfFilename;
};

HostDnsServiceResolvConf::~HostDnsServiceResolvConf()
{
    if (m)
    {
        delete m;
        m = NULL;
    }
}

HRESULT HostDnsServiceResolvConf::init(HostDnsMonitorProxy *pProxy, const char *aResolvConfFileName)
{
    HRESULT hrc = HostDnsServiceBase::init(pProxy);
    AssertComRCReturn(hrc, hrc);

    m = new Data(aResolvConfFileName);
    AssertPtrReturn(m, E_OUTOFMEMORY);

    return readResolvConf();
}

void HostDnsServiceResolvConf::uninit(void)
{
    if (m)
    {
        delete m;
        m = NULL;
    }

    HostDnsServiceBase::uninit();
}

const com::Utf8Str &HostDnsServiceResolvConf::getResolvConf(void) const
{
    return m->resolvConfFilename;
}

HRESULT HostDnsServiceResolvConf::readResolvConf(void)
{
    HostDnsInformation dnsInfo;
    int vrc = i_rcpParse(m->resolvConfFilename.c_str(), dnsInfo);

    /** @todo r=jack: Why are we returning S_OK after a general failure? */
    if (vrc == -1)
        return S_OK;

    setInfo(dnsInfo);
    return S_OK;
}

/*static*/ int HostDnsServiceResolvConf::i_rcpParse(const char *filename, HostDnsInformation &dnsInfo) RT_NOEXCEPT
{
    /*
     * This just opens the file and parses the content.
     */
    int vrc = VERR_INVALID_PARAMETER;
    if (filename != NULL /*impossible as c_str() never returns NULL*/ && *filename != '\0')
    {
        PRTSTREAM pStream = NULL;
        vrc = RTStrmOpen(filename, "r", &pStream);
        if (RT_SUCCESS(vrc))
        {
            try
            {
                vrc = i_rcpParseInner(pStream, dnsInfo);
            }
            catch (std::bad_alloc &)
            {
                vrc = VERR_NO_MEMORY;
            }
            catch (...)
            {
                vrc = VERR_UNEXPECTED_EXCEPTION;
            }

            RTStrmClose(pStream);
        }
    }
    return vrc;
}

/**
 * Internal helper for isolating the next word (token) from the given string.
 */
static char *getToken(char *psz, char **ppszSavePtr)
{
    AssertPtrReturn(ppszSavePtr, NULL);

    if (psz == NULL)
    {
        psz = *ppszSavePtr;
        if (psz == NULL)
            return NULL;
    }

    /* skip leading blanks. */
    while (RT_C_IS_BLANK(*psz))
        ++psz;

    if (*psz == '\0')
    {
        *ppszSavePtr = NULL;
        return NULL;
    }

    /* Found the start of the token we will be returning. */
    char * const pszToken = psz;

    /* Find the end so we can terminate it. */
    char ch;
    while ((ch = *psz) != '\0' && !RT_C_IS_BLANK(ch))
        ++psz;

    if (ch == '\0')
        psz = NULL;
    else
        *psz++ = '\0';

    *ppszSavePtr = psz;
    return pszToken;
}

/*static*/ int HostDnsServiceResolvConf::i_rcpParseInner(PRTSTREAM a_pStream, HostDnsInformation &dnsInfo)
{
    for (unsigned iLine = 1;; iLine++)
    {
        char buf[RCPS_BUFFER_SIZE];
        int vrc = RTStrmGetLine(a_pStream, buf, sizeof(buf));
        if (RT_FAILURE(vrc))
            return vrc == VERR_EOF ? VINF_SUCCESS : vrc;

        /*
         * Strip comment if present.
         *
         * This is not how ad-hoc parser in bind's res_init.c does it,
         * btw, so this code will accept more input as valid compared
         * to res_init.  (e.g. "nameserver 1.1.1.1; comment" is
         * misparsed by res_init).
         *
         * Update: glibc 2.42.9000 accepts ';' as a trailing comment for
         * sortlist, but not any other directives.
         */
        char *s = strchr(buf, '#');
        if (s)
            *s = '\0';
        s = strchr(buf, ';');
        if (s)
            *s = '\0';

        RTStrPurgeEncoding(buf); /* Just purge it here so we don't get any encoding non-sense in the release log. */

        char *tok = getToken(buf, &s);
        if (tok == NULL)
            continue;

        /*
         * NAMESERVER
         */
        if (RTStrCmp(tok, "nameserver") == 0)
        {
            if (dnsInfo.servers.size() < RCPS_MAX_NAMESERVERS)
            {

                /*
                 * parse next token as an IP address
                 */
                tok = getToken(NULL, &s);
                char * const pszAddr = tok;
                if (tok == NULL)
                    LogRel(("HostDnsServiceResolvConf: line %u: nameserver line without value\n", iLine));
                else
                {
                    /* Check if entry is IPv4 nameserver, save if true */
                    char *pszNext = NULL;
                    RTNETADDRIPV4 IPv4Addr = { 0 };
                    vrc = RTNetStrToIPv4AddrEx(tok, &IPv4Addr, &pszNext);
                    if (RT_SUCCESS(vrc))
                    {
                        if (*pszNext == '\0')
                        {
                            LogRel(("HostDnsServiceResolvConf: line %u: IPv4 nameserver %RTnaipv4\n", iLine, IPv4Addr));
                            dnsInfo.servers.push_back(pszAddr);

                            if ((tok = getToken(NULL, &s)) != NULL)
                                LogRel(("HostDnsServiceResolvConf: line %u: ignoring unexpected trailer on the IPv4 nameserver line (%s)\n", iLine, tok));
                        }
                        else
                            LogRel(("HostDnsServiceResolvConf: line %u: garbage at the end of IPv4 address %s\n", iLine, tok));
                    }
                    else
                    {
                        /* Check if entry is IPv6 nameserver, save if true */
                        RTNETADDRIPV6 IPv6Addr = { { 0, 0 } };
                        vrc = RTNetStrToIPv6AddrEx(tok, &IPv6Addr, &pszNext);
                        if (RT_SUCCESS(vrc))
                        {
                            if (*pszNext == '%') /** @todo XXX: TODO: IPv6 zones */
                            {
                                size_t zlen = RTStrOffCharOrTerm(pszNext, '.');
                                LogRel(("HostDnsServiceResolvConf: line %u: FIXME: ignoring IPv6 zone %*.*s\n",
                                        iLine, zlen, zlen, pszNext));
                                pszNext += zlen;
                            }

                            if (*pszNext == '\0')
                            {
                                LogRel(("HostDnsServiceResolvConf: line %u: IPv6 nameserver %RTnaipv6\n", iLine, &IPv6Addr));
                                dnsInfo.serversV6.push_back(pszAddr);

                                if ((tok = getToken(NULL, &s)) != NULL)
                                    LogRel(("HostDnsServiceResolvConf: line %u: ignoring unexpected trailer on the IPv4 nameserver line (%s)\n", iLine, tok));
                            }
                            else
                                LogRel(("HostDnsServiceResolvConf: line %u: garbage at the end of IPv6 address %s\n", iLine, tok));
                        }
                        else
                            LogRel(("HostDnsServiceResolvConf: line %u: bad nameserver address %s\n", iLine, tok));
                    }
                }
            }
            else
                LogRel(("HostDnsServiceResolvConf: line %u: too many nameserver lines, ignoring %s\n", iLine, s));
        }
        /*
         * DOMAIN
         */
        else if (RTStrCmp(tok, "domain") == 0)
        {
            if (dnsInfo.domain.isEmpty())
            {
                tok = getToken(NULL, &s);
                if (tok == NULL)
                    LogRel(("HostDnsServiceResolvConf: line %u: domain line without value\n", iLine));
                else if (strlen(tok) > 253) /* Max FQDN Length */
                    LogRel(("HostDnsServiceResolvConf: line %u: domain name too long\n", iLine));
                else
                    dnsInfo.domain.assign(tok);
            }
            else
                LogRel(("HostDnsServiceResolvConf: line %u: ignoring multiple domain lines\n", iLine));
        }
        /*
         * SEARCH
         */
        else if (RTStrCmp(tok, "search") == 0)
        {
            while ((tok = getToken(NULL, &s)) != NULL)
            {
                if (dnsInfo.searchList.size() < RCPS_MAX_SEARCHLIST)
                {
                    dnsInfo.searchList.push_back(tok);
                    LogRel(("HostDnsServiceResolvConf: line %u: search domain %s", iLine, tok));
                }
                else
                    LogRel(("HostDnsServiceResolvConf: line %u: too many search domains, ignoring %s\n", iLine, tok));
            }
        }
        else
            LogRel(("HostDnsServiceResolvConf: line %u: ignoring: %s%s%s\n", iLine, tok, s ? " " : "", s ? s : ""));
    }

    return VINF_SUCCESS;
}

