/* $Id: tstHostDnsResolvConf.cpp 112403 2026-01-11 19:29:08Z knut.osmundsen@oracle.com $ */
/** @file
 * HostDnsServiceResolvConf parsing tests.
 *
 * This adds a native testcase binary that exercises the resolv.conf parser used on
 * Unixy hosts. It validates return codes, comment handling, domain parsing and basic
 * nameserver acceptance & limits. It uses the IPRT test framework for output.
 */

/*
 * Copyright (C) 2025-2026 Oracle and/or its affiliates.
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


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_ENABLED
#define LOG_GROUP LOG_GROUP_MAIN
#include <VBox/log.h>

#include <iprt/file.h>
#include <iprt/err.h>
#include <iprt/path.h>
#include <iprt/string.h>
#include <iprt/test.h>

#if defined(RT_OS_LINUX) || defined(RT_OS_FREEBSD) || defined(RT_OS_SOLARIS)
# include "../src-server/HostDnsService.h"


/* Test stub to satisfy link for HostDnsService.cpp when building tstHostDnsResolvConf.
 * In full build, VirtualBox::i_onHostNameResolutionConfigurationChange() notifies
 * guest of changes via API. Not required for unit tests as it only tests i_rpcParse()
 * and the notificiation is emitted higher up the call stack.
 */
class VirtualBox
{
public:
    void i_onHostNameResolutionConfigurationChange();
};

void VirtualBox::i_onHostNameResolutionConfigurationChange()
{
    /* no-op for testcase */
}


/*********************************************************************************************************************************
*   Test shim                                                                                                                    *
*********************************************************************************************************************************/
class HostDnsServiceResolvConfTest : public HostDnsServiceResolvConf
{
public:
    HostDnsServiceResolvConfTest() : HostDnsServiceResolvConf(false) {}
    static int parse(const char *pszFilename, HostDnsInformation &rInfo)
    {
        return i_rcpParse(pszFilename, rInfo);
    }
};


/*********************************************************************************************************************************
*   Helpers                                                                                                                      *
*********************************************************************************************************************************/
static int createTempFileWith(const char *pszContent, com::Utf8Str &rPath)
{
    /* Create a unique temporary file; rPath is updated with the chosen path,
       but only valid after the jolt() call below. */
    RTFILE hFile = NIL_RTFILE;
    rPath.reserve(RTPATH_MAX);
    int rc = RTFileOpenTemp(&hFile, rPath.mutableRaw(), rPath.capacity() - 1,
                            RTFILE_O_CREATE | RTFILE_O_READWRITE | RTFILE_O_DENY_NONE);
    if (RT_FAILURE(rc))
        return VERR_ACCESS_DENIED;
    rPath.jolt();

    /* Write the content to it. */
    const size_t cb  = strlen(pszContent);
    size_t       cbW = 0;

    rc = RTFileWrite(hFile, pszContent, cb, &cbW);
    AssertRC(rc);

    int rc2 = RTFileClose(hFile);
    AssertRC(rc2);

    if (RT_SUCCESS(rc) && RT_SUCCESS(rc2) && cbW == cb)
        return VINF_SUCCESS;
    RTFileDelete(rPath.c_str());
    return VERR_WRITE_ERROR;
}


/*********************************************************************************************************************************
*   Tests                                                                                                                        *
*********************************************************************************************************************************/
static void testNullFilename(RTTEST hTest)
{
    /** @todo r=bird: Not useful, as impossible as c_str() never returns NULL.   */
    RTTestSub(hTest, "NULL filename");
    HostDnsInformation Info;
    int rc = HostDnsServiceResolvConfTest::parse(NULL, Info);
    RTTESTI_CHECK_MSG(rc == VERR_INVALID_PARAMETER, ("rc=%Rrc\n", rc));
}

static void testNonexistentFile(RTTEST hTest)
{
    RTTestSub(hTest, "Nonexistent file");
    HostDnsInformation Info;
    int rc = HostDnsServiceResolvConfTest::parse("/nonexistent/path/definitely-not-here", Info);
    RTTESTI_CHECK_MSG(RT_FAILURE(rc), ("rc=%Rrc (expected failure)\n", rc));
}

static void testNameserverIPv4(RTTEST hTest)
{
    RTTestSub(hTest, "nameserver IPv4");
    com::Utf8Str Path;
    int rc = createTempFileWith("nameserver 1.2.3.4\n", Path);
    RTTESTI_CHECK_RC_OK(rc);
    if (RT_SUCCESS(rc))
    {
        HostDnsInformation Info;
        rc = HostDnsServiceResolvConfTest::parse(Path.c_str(), Info);
        RTTESTI_CHECK_RC_OK(rc);
        RTTESTI_CHECK(Info.servers.size() == 1);
        RTTESTI_CHECK(RTStrCmp(Info.servers.front().c_str(), "1.2.3.4") == 0);
        RTFileDelete(Path.c_str());
    }
}

static void testNameserverGarbageTrailing(RTTEST hTest)
{
    RTTestSub(hTest, "nameserver garbage trailing");
    com::Utf8Str Path;
    int rc = createTempFileWith("nameserver 1.2.3.4x\n", Path);
    RTTESTI_CHECK_RC_OK(rc);
    if (RT_SUCCESS(rc))
    {
        HostDnsInformation Info;
        rc = HostDnsServiceResolvConfTest::parse(Path.c_str(), Info);
        RTTESTI_CHECK_RC_OK(rc);
        /* Invalid due to trailing garbage - should be ignored. */
        RTTESTI_CHECK(Info.servers.size() == 0);
        RTFileDelete(Path.c_str());
    }
}

static void testNameserverIPv6AndComments(RTTEST hTest)
{
    RTTestSub(hTest, "IPv6 and comment stripping");
    com::Utf8Str Path;
    static const char *s_psz =
        "# full line comment\n"
        "   ; another comment\n"
        "nameserver 2001:db8::1   # trailing comment\n";
    int rc = createTempFileWith(s_psz, Path);
    RTTESTI_CHECK_RC_OK(rc);
    if (RT_SUCCESS(rc))
    {
        HostDnsInformation Info;
        rc = HostDnsServiceResolvConfTest::parse(Path.c_str(), Info);
        RTTESTI_CHECK_RC_OK(rc);
        RTTESTI_CHECK(Info.serversV6.size() == 1 && Info.servers.size() == 0);
        RTTESTI_CHECK(RTStrCmp(Info.serversV6.front().c_str(), "2001:db8::1") == 0);
        RTFileDelete(Path.c_str());
    }
}

static void testNameserverLimit(RTTEST hTest)
{
    RTTestSub(hTest, "nameserver count limit");
    com::Utf8Str Path;
    static const char * const s_psz =
        "nameserver 1.1.1.1\n"
        "nameserver 2.2.2.2\n"
        "nameserver 3.3.3.3\n"
        "nameserver 4.4.4.4\n"; /* Should be ignored as per RCPS_MAX_NAMESERVERS=3 */
    int rc = createTempFileWith(s_psz, Path);
    RTTESTI_CHECK_RC_OK(rc);
    if (RT_SUCCESS(rc))
    {
        HostDnsInformation Info;
        rc = HostDnsServiceResolvConfTest::parse(Path.c_str(), Info);
        RTTESTI_CHECK_RC_OK(rc);
        RTTESTI_CHECK_MSG(Info.servers.size() == 3, ("servers.size()=%zu\n", Info.servers.size()));
        RTFileDelete(Path.c_str());
    }
}

static void testDomainBasic(RTTEST hTest)
{
    RTTestSub(hTest, "domain line");
    com::Utf8Str Path;
    int rc = createTempFileWith("domain example.com\n", Path);
    RTTESTI_CHECK_RC_OK(rc);
    if (RT_SUCCESS(rc))
    {
        HostDnsInformation Info;
        rc = HostDnsServiceResolvConfTest::parse(Path.c_str(), Info);
        RTTESTI_CHECK_RC_OK(rc);
        RTTESTI_CHECK_MSG(Info.domain.equals("example.com"), ("domain=\"%s\"\n", Info.domain.c_str()));
        RTFileDelete(Path.c_str());
    }
}

/* Note: search list tests are intentionally omitted here because the current
 * implementation uses an uninitialized index variable for the limit check,
 * which makes behaviour undefined across platforms/configs. */

#endif /* defined(RT_OS_LINUX) || defined(RT_OS_FREEBSD) || defined(RT_OS_SOLARIS) */


/*********************************************************************************************************************************
*   main                                                                                                                         *
*********************************************************************************************************************************/
int main()
{
    RTTEST      hTest;
    RTEXITCODE  rcExit = RTTestInitAndCreate("tstHostDnsResolvConf", &hTest);
    if (rcExit == RTEXITCODE_SUCCESS)
    {
        RTTestBanner(hTest);

#if defined(RT_OS_LINUX) || defined(RT_OS_FREEBSD) || defined(RT_OS_SOLARIS)
# if 1
        static const char *s_apszGroups[] = VBOX_LOGGROUP_NAMES;
        PRTLOGGER          pRelLogger = NULL;
        int rc = RTLogCreate(&pRelLogger, 0 /*fFlags*/, "all.e", "tstHostDnsResolvConf_",
                             RT_ELEMENTS(s_apszGroups), s_apszGroups, RTLOGDEST_STDERR, "");
        if (RT_SUCCESS(rc))
            RTLogRelSetDefaultInstance(pRelLogger);
# endif

        testNullFilename(hTest);
        testNonexistentFile(hTest);
        testNameserverIPv4(hTest);
        testNameserverGarbageTrailing(hTest);
        testNameserverIPv6AndComments(hTest);
        testNameserverLimit(hTest);
        testDomainBasic(hTest);

        rcExit = RTTestSummaryAndDestroy(hTest);

        RTLogDestroy(RTLogRelSetDefaultInstance(NULL));
#else
        rcExit = RTTestSkipAndDestroy(hTest, "Not supported on this host OS");
#endif
    }
    return rcExit;
}
