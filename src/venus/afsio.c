/*
 * Copyright (c) 2007, Hartmut Reuter,
 * RZG, Max-Planck-Institut f. Plasmaphysik.
 * All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in
 *      the documentation and/or other materials provided with the
 *      distribution.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * Revised in 2010 by Chaz Chandler to enhance clientless operations.
 * Now utilizes libafscp by Chaskiel Grundman.
 * Work funded in part by Sine Nomine Associates (http://www.sinenomine.net/)
 */

#include <afsconfig.h>
#include <afs/param.h>
#include <afs/stds.h>

#include <stdio.h>
#include <setjmp.h>
#include <sys/types.h>
#include <string.h>
#include <ctype.h>

#ifdef AFS_NT40_ENV
#include <windows.h>
#include <winsock2.h>
#define _CRT_RAND_S
#include <stdlib.h>
#include <process.h>
#include <fcntl.h>
#include <io.h>
#include <afs/smb_iocons.h>
#include <afs/afsd.h>
#include <afs/cm_ioctl.h>
#include <afs/pioctl_nt.h>
#include <WINNT/syscfg.h>
#else
#include <sys/param.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>
#include <fcntl.h>
#include <pwd.h>
#include <afs/venus.h>
#include <sys/time.h>
#include <netdb.h>
#include <afs/afsint.h>
#define FSINT_COMMON_XG 1
#endif
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
#include <afs/vice.h>
#include <afs/cmd.h>
#include <afs/auth.h>
#include <afs/vlserver.h>
#include <afs/ihandle.h>
#include <afs/com_err.h>
#include <afs/afscp.h>
#ifdef HAVE_DIRENT_H
#include <dirent.h>
#endif
#ifdef HAVE_DIRECT_H
#include <direct.h>
#endif
#include <afs/errors.h>
#include <afs/sys_prototypes.h>
#include <des_prototypes.h>
#include <rx/rx_prototypes.h>
#include "../rxkad/md5.h"
#ifdef O_LARGEFILE
#define afs_stat        stat64
#define afs_fstat       fstat64
#define afs_open        open64
#else /* !O_LARGEFILE */
#define afs_stat        stat
#define afs_fstat       fstat
#define afs_open        open
#endif /* !O_LARGEFILE */
#ifdef AFS_PTHREAD_ENV
#include <assert.h>
pthread_key_t uclient_key;
#endif

static int readFile(struct cmd_syndesc *, void *);
static int writeFile(struct cmd_syndesc *, void *);
static void printDatarate(void);
static void summarizeDatarate(struct timeval *, const char *);
static int CmdProlog(struct cmd_syndesc *, char **, char **,
                     char **, char **);
static int ScanFid(char *, struct AFSFid *);
static afs_int32 GetVenusFidByFid(char *, char *, int, struct afscp_venusfid **);
static afs_int32 GetVenusFidByPath(char *, char *, struct afscp_venusfid **);
static int BreakUpPath(char *, char *, char *);

static char pnp[AFSPATHMAX];	/* filename of this program when called */
static int verbose = 0;		/* Set if -verbose option given */
static int cellGiven = 0;	/* Set if -cell option given */
static int force = 0;		/* Set if -force option given */
static int useFid = 0;		/* Set if fidwrite/fidread/fidappend invoked */
static int append = 0;		/* Set if append/fidappend invoked */
static struct timeval starttime, opentime, readtime, writetime;
static afs_uint64 xfered = 0;
static struct timeval now;
#ifdef AFS_NT40_ENV
static int Timezone;            /* Roken gettimeofday ignores the timezone */
#else
static struct timezone Timezone;
#endif

#define BUFFLEN 65536
#define WRITEBUFLEN (BUFFLEN * 1024)
#define MEGABYTE_F 1048576.0f

static MD5_CTX md5;
static int md5sum = 0;		/* Set if -md5 option given */

struct wbuf {
    struct wbuf *next;
    afs_uint32 offset;		/* offset inside the buffer */
    afs_uint32 buflen;		/* total length == BUFFLEN */
    afs_uint32 used;		/* bytes used inside buffer */
    char buf[BUFFLEN];
};

/*!
 *  returns difference in seconds between two times
 *
 *  \param[in]	from	start time
 *  \param[in]	to	end time
 *
 *  \post returns "to" minus "from" in seconds
 *
 */
static_inline float
time_elapsed(struct timeval *from, struct timeval *to)
{
    return (float)(to->tv_sec + (to->tv_usec * 0.000001) - from->tv_sec -
		   (from->tv_usec * 0.000001));
} /* time_elapsed */

/*!
 * prints current average data transfer rate at no less than 30-second intervals
 */
static void
printDatarate(void)
{
    static float oldseconds = 0.0;
    static afs_uint64 oldxfered = 0;
    float seconds;

    gettimeofday(&now, &Timezone);
    seconds = time_elapsed(&opentime, &now);
    if ((seconds - oldseconds) > 30) {
	fprintf(stderr, "%llu MB transferred, present data rate = %.3f MB/sec.\n", xfered >> 20,	/* total bytes transferred, in MB */
		(xfered - oldxfered) / (seconds - oldseconds) / MEGABYTE_F);
	oldxfered = xfered;
	oldseconds = seconds;
    }
} /* printDatarate */

/*!
 * prints overall average data transfer rate and elapsed time
 *
 * \param[in]	tvp		current time (to compare with file open time)
 * \param[in]	xfer_type	string identify transfer type ("read" or "write")
 */
static void
summarizeDatarate(struct timeval *tvp, const char *xfer_type)
{
    float seconds = time_elapsed(&opentime, tvp);

    fprintf(stderr, "Transfer of %llu bytes took %.3f sec.\n",
	    xfered, seconds);
    fprintf(stderr, "Total data rate = %.03f MB/sec. for %s\n",
	    xfered / seconds / MEGABYTE_F, xfer_type);
} /* summarizeDatarate */

/*!
 * prints final MD5 sum of all file data transferred
 *
 * \param[in]	fname	file name or FID
 */
static void
summarizeMD5(char *fname)
{
    afs_uint32 md5int[4];
    char *p;

    MD5_Final((char *) &md5int[0], &md5);
    p = fname + strlen(fname);
    while (p > fname) {
	if (*(--p) == '/') {
	    ++p;
	    break;
	}
    }
    fprintf(stderr, "%08x%08x%08x%08x  %s\n", htonl(md5int[0]),
	    htonl(md5int[1]), htonl(md5int[2]), htonl(md5int[3]), p);
} /* summarizeMD5 */

/*!
 * parses all command-line arguments
 *
 * \param[in]  as	arguments list
 * \param[out] cellp	cell name
 * \param[out] realmp	realm name
 * \param[out] fnp	filename (either fid or path)
 * \param[out] slp	"synthesized" (made up) data given
 *
 * \post returns 0 on success or -1 on error
 *
 */
static int
CmdProlog(struct cmd_syndesc *as, char **cellp, char **realmp,
          char **fnp, char **slp)
{
    int i;
    struct cmd_parmdesc *pdp;

    if (as == NULL) {
	afs_com_err(pnp, EINVAL, "(syndesc is null)");
	return -1;
    }

    /* determine which command was requested */
    if (strncmp(as->name, "fid", 3) == 0) /* fidread/fidwrite/fidappend */
	useFid = 1;
    if ( (strcmp(as->name, "append") == 0) ||
         (strcmp(as->name, "fidappend") == 0) )
	append = 1;		/* global */

    /* attempts to ensure loop is bounded: */
    for (pdp = as->parms, i = 0; pdp && (i < as->nParms); i++, pdp++) {
	if (pdp->items != NULL) {
	    if (strcmp(pdp->name, "-verbose") == 0)
	        verbose = 1;
            else if (strcmp(pdp->name, "-md5") == 0)
		md5sum = 1;	/* global */
            else if (strcmp(pdp->name, "-cell") == 0) {
		cellGiven = 1;	/* global */
		*cellp = pdp->items->data;
            } else if ( (strcmp(pdp->name, "-file") == 0) ||
                        (strcmp(pdp->name, "-fid") == 0) ||
                        (strcmp(pdp->name, "-vnode") == 0) )
		*fnp = pdp->items->data;
            else if (strcmp(pdp->name, "-force") == 0)
		force = 1;	/* global */
            else if (strcmp(pdp->name, "-synthesize") == 0)
		*slp = pdp->items->data;
            else if (strcmp(pdp->name, "-realm") == 0)
		*realmp = pdp->items->data;
	}
    }
    return 0;
}				/* CmdProlog */

int
main(int argc, char **argv)
{
    afs_int32 code = 0;
    struct cmd_syndesc *ts;
    char baseName[AFSNAMEMAX];

    /* try to get only the base name of this executable for use in logs */
    if (BreakUpPath(argv[0], NULL, baseName) > 0)
	strlcpy(pnp, baseName, AFSNAMEMAX);
    else
	strlcpy(pnp, argv[0], AFSPATHMAX);

#ifdef AFS_PTHREAD_ENV
    assert(pthread_key_create(&uclient_key, NULL) == 0);
#endif

    ts = cmd_CreateSyntax("read", readFile, CMD_REQUIRED,
			  "read a file from AFS");
    cmd_AddParm(ts, "-file", CMD_SINGLE, CMD_REQUIRED, "AFS-filename");
    cmd_AddParm(ts, "-cell", CMD_SINGLE, CMD_OPTIONAL, "cellname");
    cmd_AddParm(ts, "-verbose", CMD_FLAG, CMD_OPTIONAL, (char *)0);
    cmd_AddParm(ts, "-md5", CMD_FLAG, CMD_OPTIONAL, "calculate md5 checksum");
    cmd_AddParm(ts, "-realm", CMD_SINGLE, CMD_OPTIONAL, "REALMNAME");

    ts = cmd_CreateSyntax("fidread", readFile, CMD_REQUIRED,
			  "read on a non AFS-client a file from AFS");
    cmd_IsAdministratorCommand(ts);
    cmd_AddParm(ts, "-fid", CMD_SINGLE, CMD_REQUIRED,
		"volume.vnode.uniquifier");
    cmd_AddParm(ts, "-cell", CMD_SINGLE, CMD_OPTIONAL, "cellname");
    cmd_AddParm(ts, "-verbose", CMD_FLAG, CMD_OPTIONAL, (char *)0);
    cmd_AddParm(ts, "-md5", CMD_FLAG, CMD_OPTIONAL, "calculate md5 checksum");
    cmd_AddParm(ts, "-realm", CMD_SINGLE, CMD_OPTIONAL, "REALMNAME");

    ts = cmd_CreateSyntax("write", writeFile, CMD_REQUIRED,
			  "write a file into AFS");
    cmd_AddParm(ts, "-file", CMD_SINGLE, CMD_REQUIRED, "AFS-filename");
    cmd_AddParm(ts, "-cell", CMD_SINGLE, CMD_OPTIONAL, "cellname");
    cmd_AddParm(ts, "-verbose", CMD_FLAG, CMD_OPTIONAL, (char *)0);
    cmd_AddParm(ts, "-md5", CMD_FLAG, CMD_OPTIONAL, "calculate md5 checksum");
    cmd_AddParm(ts, "-force", CMD_FLAG, CMD_OPTIONAL,
		"overwrite existing file");
    cmd_AddParm(ts, "-synthesize", CMD_SINGLE, CMD_OPTIONAL,
		"create data pattern of specified length instead reading from stdin");
    cmd_AddParm(ts, "-realm", CMD_SINGLE, CMD_OPTIONAL, "REALMNAME");

    ts = cmd_CreateSyntax("fidwrite", writeFile, CMD_REQUIRED,
			  "write a file into AFS");
    cmd_IsAdministratorCommand(ts);
    cmd_AddParm(ts, "-vnode", CMD_SINGLE, CMD_REQUIRED,
		"volume.vnode.uniquifier");
    cmd_AddParm(ts, "-cell", CMD_SINGLE, CMD_OPTIONAL, "cellname");
    cmd_AddParm(ts, "-verbose", CMD_FLAG, CMD_OPTIONAL, (char *)0);
    cmd_AddParm(ts, "-md5", CMD_FLAG, CMD_OPTIONAL, "calculate md5 checksum");
    cmd_AddParm(ts, "-force", CMD_FLAG, CMD_OPTIONAL,
		"overwrite existing file");
    cmd_AddParm(ts, "-realm", CMD_SINGLE, CMD_OPTIONAL, "REALMNAME");

    ts = cmd_CreateSyntax("append", writeFile, CMD_REQUIRED,
			  "append to a file in AFS");
    cmd_AddParm(ts, "-file", CMD_SINGLE, CMD_REQUIRED, "AFS-filename");
    cmd_AddParm(ts, "-cell", CMD_SINGLE, CMD_OPTIONAL, "cellname");
    cmd_AddParm(ts, "-verbose", CMD_FLAG, CMD_OPTIONAL, (char *)0);
    cmd_AddParm(ts, "-realm", CMD_SINGLE, CMD_OPTIONAL, "REALMNAME");

    ts = cmd_CreateSyntax("fidappend", writeFile, CMD_REQUIRED,
			  "append to a file in AFS");
    cmd_IsAdministratorCommand(ts);
    cmd_AddParm(ts, "-vnode", CMD_SINGLE, CMD_REQUIRED,
		"volume.vnode.uniquifier");
    cmd_AddParm(ts, "-cell", CMD_SINGLE, CMD_OPTIONAL, "cellname");
    cmd_AddParm(ts, "-verbose", CMD_FLAG, CMD_OPTIONAL, (char *)0);
    cmd_AddParm(ts, "-realm", CMD_SINGLE, CMD_OPTIONAL, "REALMNAME");

    if (afscp_Init(NULL) != 0)
	exit(1);

    code = cmd_Dispatch(argc, argv);

    afscp_Finalize();
    exit(0);
} /* main */

/*!
 * standardized way of parsing a File ID (FID) from command line input
 *
 * \param[in]	fidString	dot-delimited FID triple
 * \param[out]	fid		pointer to the AFSFid to fill in
 *
 * \post The FID pointed to by "fid" is filled in which the parsed Volume,
 *       Vnode, and Uniquifier data.  The string should be in the format
 *       of three numbers separated by dot (.) delimiters, representing
 *       (in order) the volume id, vnode number, and uniquifier.
 *       Example: "576821346.1.1"
 */
static int
ScanFid(char *fidString, struct AFSFid *fid)
{
    int i = 0, code = 0;
    long unsigned int f1, f2, f3;

    if (fidString) {
	i = sscanf(fidString, "%lu.%lu.%lu", &f1, &f2, &f3);
	fid->Volume = (afs_uint32) f1;
	fid->Vnode = (afs_uint32) f2;
	fid->Unique = (afs_uint32) f3;
    }
    if (i != 3) {
	fid->Volume = 0;
	fid->Vnode = 0;
	fid->Unique = 0;
	code = EINVAL;
	afs_com_err(pnp, code, "(invalid FID triple: %s)", fidString);
    }

    return code;
} /* ScanFid */

/*!
 * look up cell info and verify FID info from user input
 *
 * \param[in]	fidString	string containing FID info
 * \param[in]	cellName	cell name string
 * \param[in]	onlyRW		bool: 1 = RW vol only, 0 = any vol type
 * \param[out]	avfpp		pointer to venusfid info
 *
 * \post *avfpp will contain the VenusFid info found for the FID
 *       given by the used in the string fidString and zero is
 *       returned.  If not found, an appropriate afs error code
 *       is returned and *avfpp will be NULL.
 *
 * \note Any non-NULL avfpp returned should later be freed with
 *       afscp_FreeFid() when no longer needed.
 */
static afs_int32
GetVenusFidByFid(char *fidString, char *cellName, int onlyRW,
                 struct afscp_venusfid **avfpp)
{
    afs_int32 code = 0;
    struct stat sbuf;
    struct afscp_volume *avolp;

    if (*avfpp == NULL) {
	*avfpp = malloc(sizeof(struct afscp_venusfid));
	if ( *avfpp == NULL ) {
	    code = ENOMEM;
	    return code;
	}
    }
    memset(*avfpp, 0, sizeof(struct afscp_venusfid));

    if (cellName == NULL) {
	(*avfpp)->cell = afscp_DefaultCell();
    } else {
	(*avfpp)->cell = afscp_CellByName(cellName, NULL);
    }
    if ((*avfpp)->cell == NULL) {
	if (afscp_errno == 0)
	    code = EINVAL;
	else
	    code = afscp_errno;
	return code;
    }

    code = ScanFid(fidString, &((*avfpp)->fid));
    if (code != 0) {
	code = EINVAL;
	return code;
    }

    avolp = afscp_VolumeById((*avfpp)->cell, (*avfpp)->fid.Volume);
    if (avolp == NULL) {
	if (afscp_errno == 0)
	    code = ENOENT;
	else
	    code = afscp_errno;
	afs_com_err(pnp, code, "(finding volume %lu)",
		    afs_printable_uint32_lu((*avfpp)->fid.Volume));
	return code;
    }

    if ( onlyRW && (avolp->voltype != RWVOL) ) {
	avolp = afscp_VolumeByName((*avfpp)->cell, avolp->name, RWVOL);
	if (avolp == NULL) {
	    if (afscp_errno == 0)
		code = ENOENT;
	    else
		code = afscp_errno;
	    afs_com_err(pnp, code, "(finding volume %lu)",
		        afs_printable_uint32_lu((*avfpp)->fid.Volume));
	    return code;
	}
	(*avfpp)->fid.Volume = avolp->id; /* is this safe? */
    }

    code = afscp_Stat((*avfpp), &sbuf);
    if (code != 0) {
	afs_com_err(pnp, code, "(stat failed with code %d)", code);
	return code;
    }
    return 0;
} /* GetVenusFidByFid */

/*!
 * Split a full path up into dirName and baseName components
 *
 * \param[in]	fullPath	can be absolute, relative, or local
 * \param[out]	dirName		pointer to allocated char buffer or NULL
 * \param[out]	baseName	pointer to allocated char buffer or NULL
 *
 * \post To the fulleset extent possible, the rightmost full path
 *       component will be copied into baseName and all other
 *       components into dirName (minus the trailing path separator).
 *       If either dirName or baseName are NULL, only the non-NULL
 *       pointer will be filled in (but both can't be null or it would
 *       be pointless) -- so the caller can retrieve, say, only baseName
 *       if desired.  The return code is the number of strings copied:
 *       0 if neither dirName nor baseName could be filled in
 *       1 if either dirName or baseName were filled in
 *       2 if both dirName and baseName were filled in
 */
static int
BreakUpPath(char *fullPath, char *dirName, char *baseName)
{
    char *lastSlash;
    size_t dirNameLen = 0;
    int code = 0, useDirName = 1, useBaseName = 1;

    if (fullPath == NULL) {
	return code;
    }

    if (dirName == NULL)
	useDirName = 0;
    if (baseName == NULL)
	useBaseName = 0;
    if (!useBaseName && !useDirName) {
	/* would be pointless to continue -- must be error in call */
	return code;
    }
#ifdef AFS_NT40_ENV
    lastSlash = strrchr(fullPath, '\\');
#else
    lastSlash = strrchr(fullPath, '/');
#endif
    if (lastSlash != NULL) {
	/* then lastSlash points to the last path separator in fullPath */
	if (useDirName) {
	    dirNameLen = strlen(fullPath) - strlen(lastSlash);
	    strlcpy(dirName, fullPath, dirNameLen + 1);
	    code++;
	}
	if (useBaseName) {
	    lastSlash++;
	    strlcpy(baseName, lastSlash, strlen(lastSlash) + 1);
	    code++;
	}
    } else {
	/* there are no path separators in fullPath -- it's just a baseName */
	if (useBaseName) {
	    strlcpy(baseName, fullPath, strlen(fullPath) + 1);
	    code++;
	}
    }
    return code;
} /* BreakUpPath */

/*!
 * Get the VenusFid info available for the file at AFS path 'fullPath'.
 * Works without pioctls/afsd by using libafscp.  Analogous to
 * get_file_cell() in the previous iteration of afsio.
 *
 * \param[in]	fullPath	the file name
 * \param[in]	cellName	the cell name to look up
 * \param[out]	avfpp		pointer to Venus FID info to be filled in
 *
 * \post If the path resolves successfully (using afscp_ResolvePath),
 *       then vfpp will contain the Venus FID info (cell info plus
 *       AFSFid) of the last path segment in fullPath.
 */
static afs_int32
GetVenusFidByPath(char *fullPath, char *cellName,
                  struct afscp_venusfid **avfpp)
{
    afs_int32 code = 0;

    if (fullPath == NULL) {
	return -1;
    }

    if (cellName != NULL) {
	code = (afs_int32) afscp_SetDefaultCell(cellName);
	if (code != 0) {
	    return code;
	}
    }

    *avfpp = afscp_ResolvePath(fullPath);
    if (*avfpp == NULL) {
	if (afscp_errno == 0)
	    code = ENOENT;
	else
	    code = afscp_errno;
    }

    return code;
} /* GetVenusFidByPath */

static int
readFile(struct cmd_syndesc *as, void *unused)
{
    char *fname = NULL;
    char *cell = NULL;
    char *realm = NULL;
    afs_int32 code = 0;
    struct AFSFetchStatus OutStatus;
    struct afscp_venusfid *avfp = NULL;
    afs_int64 Pos;
    afs_int32 len;
    afs_int64 length = 0, Len;
    int bytes;
    int worstCode = 0;
    char *buf = 0;
    char ipv4_addr[16];
    int bufflen = BUFFLEN;

#ifdef AFS_NT40_ENV
    /* stdout on Windows defaults to _O_TEXT mode */
    _setmode(1, _O_BINARY);
#endif

    gettimeofday(&starttime, &Timezone);

    CmdProlog(as, &cell, &realm, &fname, NULL);
    afscp_AnonymousAuth(1);

    if (md5sum)
	MD5_Init(&md5);

    if (realm != NULL)
	code = afscp_SetDefaultRealm(realm);

    if (cell != NULL)
	code = afscp_SetDefaultCell(cell);

    if (useFid)
	code = GetVenusFidByFid(fname, cell, 0, &avfp);
    else
	code = GetVenusFidByPath(fname, cell, &avfp);
    if (code != 0) {
	afs_com_err(pnp, code, "(file not found: %s)", fname);
	return code;
    }

    if (avfp->fid.Vnode & 1) {
	code = ENOENT;
	afs_com_err(pnp, code, "(%s is a directory, not a file)", fname);
	afscp_FreeFid(avfp);
	return code;
    }

    code = afscp_GetStatus(avfp, &OutStatus);
    if (code != 0) {
	afs_inet_ntoa_r(avfp->cell->fsservers[0]->addrs[0], ipv4_addr);
	afs_com_err(pnp, code, "(failed to get status of file %s from"
		    "server %s, code = %d)", fname, ipv4_addr, code);
	afscp_FreeFid(avfp);
	return code;
    }

    gettimeofday(&opentime, &Timezone);
    if (verbose)
	fprintf(stderr, "Startup to find the file took %.3f sec.\n",
		time_elapsed(&starttime, &opentime));
    Len = OutStatus.Length_hi;
    Len <<= 32;
    Len += OutStatus.Length;
    ZeroInt64(Pos);
    buf = (char *) malloc(bufflen * sizeof(char));
    if (buf == NULL) {
	code = ENOMEM;
	afs_com_err(pnp, code, "(cannot allocate buffer)");
	afscp_FreeFid(avfp);
	return code;
    }
    memset(buf, 0, bufflen * sizeof(char));
    length = Len;
    while (!code && NonZeroInt64(length)) {
	if (length > bufflen)
	    len = bufflen;
	else
	    len = (afs_int32) length;
	bytes = afscp_PRead(avfp, buf, len, Pos);
	if (bytes != len)
	    code = -3; /* what error name should we use here? */
	if (md5sum)
	    MD5_Update(&md5, buf, len);
	if (code == 0) {
	    len = write(1, buf, len); /* to stdout */
	    if (len == 0)
		code = errno;
	}
	length -= len;
	xfered += len;
	if (verbose)
	    printDatarate();
	Pos += len;
	worstCode = code;
    }
    afscp_FreeFid(avfp);

    gettimeofday(&readtime, &Timezone);
    if (md5sum)
	summarizeMD5(fname);
    if (verbose)
	summarizeDatarate(&readtime, "read");
    if (buf != NULL)
	free(buf);

    return worstCode;
} /* readFile */

static int
writeFile(struct cmd_syndesc *as, void *unused)
{
    char *fname = NULL;
    char *cell = NULL;
    char *sSynthLen = NULL;
    char *realm = NULL;
    afs_int32 code = 0;
    afs_int32 byteswritten;
    struct AFSFetchStatus OutStatus;
    struct AFSStoreStatus InStatus;
    struct afscp_venusfid *dirvfp = NULL, *newvfp = NULL;
    afs_int64 Pos;
    afs_int64 length, Len, synthlength = 0, offset = 0;
    afs_int64 bytes;
    int worstCode = 0;
    int synthesize = 0;
    int overWrite = 0;
    struct wbuf *bufchain = 0;
    struct wbuf *previous, *tbuf;
    char dirName[AFSPATHMAX];
    char baseName[AFSNAMEMAX];
    char ipv4_addr[16];

#ifdef AFS_NT40_ENV
    /* stdin on Windows defaults to _O_TEXT mode */
    _setmode(0, _O_BINARY);
#endif

    CmdProlog(as, &cell, &realm, &fname, &sSynthLen);
    afscp_AnonymousAuth(1);

    if (realm != NULL)
	code = afscp_SetDefaultRealm(realm);

    if (cell != NULL)
	code = afscp_SetDefaultCell(cell);

    if (sSynthLen) {
	code = util_GetInt64(sSynthLen, &synthlength);
	if (code != 0) {
	    afs_com_err(pnp, code, "(invalid value for synthesize length %s)",
			sSynthLen);
	    return code;
	}
	synthesize = 1;
    }

    if (useFid) {
	code = GetVenusFidByFid(fname, cell, 1, &newvfp);
	if (code != 0) {
	    afs_com_err(pnp, code, "(GetVenusFidByFid returned code %d)", code);
	    return code;
	}
    } else {
	code = GetVenusFidByPath(fname, cell, &newvfp);
	if (code == 0) { /* file was found */
	    if (force)
		overWrite = 1;
	    else if (!append) {
		/*
		 * file cannot already exist if specified by path and not
		 * appending to it unless user forces overwrite
		 */
		code = EEXIST;
		afscp_FreeFid(newvfp);
		afs_com_err(pnp, code, "(use -force to overwrite)");
		return code;
	    }
	} else { /* file not found */
	    if (append) {
		code = ENOENT;
		afs_com_err(pnp, code, "(cannot append to non-existent file)");
		return code;
	    }
	}
	if (!append && !overWrite) { /* must create a new file in this case */
	    if ( BreakUpPath(fname, dirName, baseName) != 2 ) {
		code = EINVAL;
		afs_com_err(pnp, code, "(must provide full AFS path)");
		afscp_FreeFid(newvfp);
		return code;
	    }

	    code = GetVenusFidByPath(dirName, cell, &dirvfp);
	    afscp_FreeFid(newvfp); /* release now-unneeded fid */
	    newvfp = NULL;
	    if (code != 0) {
		afs_com_err(pnp, code, "(is dir %s in AFS?)", dirName);
		return code;
	    }
	}
    }

    if ( (newvfp != NULL) && (newvfp->fid.Vnode & 1) ) {
	code = EISDIR;
	afs_com_err(pnp, code, "(%s is a directory, not a file)", fname);
	afscp_FreeFid(newvfp);
	afscp_FreeFid(dirvfp);
	return code;
    }
    gettimeofday(&starttime, &Timezone);

    InStatus.UnixModeBits = 0644;
    if (newvfp == NULL) {
	code = afscp_CreateFile(dirvfp, baseName, &InStatus, &newvfp);
	if (code != 0) {
	    afs_com_err(pnp, code,
		        "(could not create file %s in directory %lu.%lu.%lu)",
		        baseName, afs_printable_uint32_lu(dirvfp->fid.Volume),
		        afs_printable_uint32_lu(dirvfp->fid.Vnode),
		        afs_printable_uint32_lu(dirvfp->fid.Unique));
	    return code;
	}
    }
    code = afscp_GetStatus(newvfp, &OutStatus);
    if (code != 0) {
	afs_inet_ntoa_r(newvfp->cell->fsservers[0]->addrs[0], ipv4_addr);
	afs_com_err(pnp, code, "(failed to get status of file %s from"
		    "server %s, code = %d)", fname, ipv4_addr, code);
	afscp_FreeFid(newvfp);
	afscp_FreeFid(dirvfp);
	return code;
    }

    if ( !append && !force &&
	 (OutStatus.Length != 0 || OutStatus.Length_hi !=0 ) ) {
	/*
	 * file exists, is of non-zero length, and we're not appending
	 * to it: user must force overwrite
	 * (covers fidwrite edge case)
	 */
	code = EEXIST;
	afscp_FreeFid(newvfp);
	afscp_FreeFid(dirvfp);
	afs_com_err(pnp, code, "(use -force to overwrite)");
	return code;
    }

    InStatus.Mask = AFS_SETMODE + AFS_FSYNC;
    if (append) {
	Pos = OutStatus.Length_hi;
	Pos = (Pos << 32) | OutStatus.Length;
    } else
	Pos = 0;
    previous = (struct wbuf *)&bufchain;
    if (md5sum)
	MD5_Init(&md5);

    /*
     * currently, these two while loops (1) read the whole source file in
     * before (2) writing any of it out, meaning that afsio can't deal with
     * files larger than the maximum amount of memory designated for
     * reading a file in (WRITEBUFLEN).
     * Consider going to a single loop, like in readFile(), though will
     * have implications on timing statistics (such as the "Startup to
     * find the file" time, below).
     */
    Len = 0;
    while (Len < WRITEBUFLEN) {
	tbuf = (struct wbuf *)malloc(sizeof(struct wbuf));
	if (tbuf == NULL) {
	    if (!bufchain) {
		code = ENOMEM;
		afscp_FreeFid(newvfp);
		afscp_FreeFid(dirvfp);
		afs_com_err(pnp, code, "(cannot allocate buffer)");
		return code;
	    }
	    break;
	}
	memset(tbuf, 0, sizeof(struct wbuf));
	tbuf->buflen = BUFFLEN;
	if (synthesize) {
	    afs_int64 ll, l = tbuf->buflen;
	    if (l > synthlength)
		l = synthlength;
	    for (ll = 0; ll < l; ll += 4096) {
		sprintf(&tbuf->buf[ll], "Offset (0x%x, 0x%x)\n",
			(unsigned int)((offset + ll) >> 32),
			(unsigned int)((offset + ll) & 0xffffffff));
	    }
	    offset += l;
	    synthlength -= l;
	    tbuf->used = (afs_int32) l;
	} else
	    tbuf->used = read(0, &tbuf->buf, tbuf->buflen); /* from stdin */
	if (tbuf->used == 0) {
	    free(tbuf);
	    break;
	}
	if (md5sum)
	    MD5_Update(&md5, &tbuf->buf, tbuf->used);
	previous->next = tbuf;
	previous = tbuf;
	Len += tbuf->used;
    }
    gettimeofday(&opentime, &Timezone);
    if (verbose)
	fprintf(stderr, "Startup to find the file took %.3f sec.\n",
		time_elapsed(&starttime, &opentime));
    bytes = Len;
    while (!code && bytes) {
	Len = bytes;
	length = Len;
	tbuf = bufchain;
	if (Len) {
	    for (tbuf = bufchain; tbuf; tbuf = tbuf->next) {
		if (tbuf->used == 0)
		    break;
		byteswritten = afscp_PWrite(newvfp, tbuf->buf,
					    tbuf->used, Pos + xfered);
		if (byteswritten != tbuf->used) {
	            fprintf(stderr,"Only %d instead of %" AFS_INT64_FMT " bytes transferred by rx_Write()\n", byteswritten, length);
	            fprintf(stderr, "At %" AFS_UINT64_FMT " bytes from the end\n", length);
		    code = -4;
		    break;
		}
		xfered += tbuf->used;
		if (verbose)
		    printDatarate();
		length -= tbuf->used;
	    }
	}
	Pos += Len;
	bytes = 0;
	if (!code) {
	    for (tbuf = bufchain; tbuf; tbuf = tbuf->next) {
		tbuf->offset = 0;
		if (synthesize) {
		    afs_int64 ll, l = tbuf->buflen;
		    if (l > synthlength)
			l = synthlength;
		    for (ll = 0; ll < l; ll += 4096) {
			sprintf(&tbuf->buf[ll], "Offset (0x%x, 0x%x)\n",
				(unsigned int)((offset + ll) >> 32),
				(unsigned int)((offset + ll) & 0xffffffff));
		    }
		    offset += l;
		    synthlength -= l;
		    tbuf->used = (afs_int32) l;
		} else
		    tbuf->used = read(0, &tbuf->buf, tbuf->buflen); /* from stdin */
		if (!tbuf->used)
		    break;
		if (md5sum)
		    MD5_Update(&md5, &tbuf->buf, tbuf->used);
		Len += tbuf->used;
		bytes += tbuf->used;
	    }
	}
    }
    afscp_FreeFid(newvfp);
    afscp_FreeFid(dirvfp);

    gettimeofday(&writetime, &Timezone);
    if (code) {
	afs_com_err(pnp, code, "(%s failed with code %d)", as->name,
		    code);
    } else if (verbose) {
	summarizeDatarate(&writetime, "write");
    }
    while (bufchain) {
	tbuf = bufchain;
	bufchain = tbuf->next;
	free(tbuf);
    }

    if (md5sum)
	summarizeMD5(fname);

    return worstCode;
} /* writeFile */
