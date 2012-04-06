#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include "ct/ct.h"
#include "dat.h"

static void mustsend(int fd, char* cmd);

static int srvpid;
static int timeout = 500000000; // 500ms


static int
mustdiallocal(int port)
{
    int r, fd;
    struct sockaddr_in addr = {};

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    r = inet_aton("127.0.0.1", &addr.sin_addr);
    if (!r) {
        errno = EINVAL;
        twarn("inet_aton");
        exit(1);
    }

    fd = socket(PF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        twarn("socket");
        exit(1);
    }

    r = connect(fd, (struct sockaddr*)&addr, sizeof addr);
    if (r == -1) {
        twarn("connect");
        exit(1);
    }

    return fd;
}


#define SERVER() (progname=__func__, mustforksrv())

static int
mustforksrv()
{
    int r, len, port;
    struct sockaddr_in addr;

    srv.sock.fd = make_server_socket("127.0.0.1", "0");
    if (srv.sock.fd == -1) {
        puts("mustforksrv failed");
        exit(1);
    }

    len = sizeof(addr);
    r = getsockname(srv.sock.fd, (struct sockaddr*)&addr, (socklen_t*)&len);
    if (r == -1 || len > sizeof(addr)) {
        puts("mustforksrv failed");
        exit(1);
    }

    port = ntohs(addr.sin_port);
    srvpid = fork();
    if (srvpid != 0) {
        return port;
    }

    /* now in child */

    prot_init();

    if (srv.wal.use) {
        struct job list = {};
        // We want to make sure that only one beanstalkd tries
        // to use the wal directory at a time. So acquire a lock
        // now and never release it.
        if (!waldirlock(&srv.wal)) {
            twarnx("failed to lock wal dir %s", srv.wal.dir);
            exit(10);
        }

        list.prev = list.next = &list;
        walinit(&srv.wal, &list);
        prot_replay(&srv, &list);
    }

    srvserve(&srv); /* does not return */
    exit(1); /* satisfy the compiler */
}


static char *
readline(int fd)
{
    int r, c = 0, p = 0, i = 0;
    static char buf[1024];
    fd_set rfd;
    struct timeval tv;

    printf("<%d ", fd);
    fflush(stdout);
    for (;;) {
        FD_ZERO(&rfd);
        FD_SET(fd, &rfd);
        tv.tv_sec = timeout / 1000000000;
        tv.tv_usec = (timeout/1000) % 1000000;
        r = select(fd+1, &rfd, NULL, NULL, &tv);
        switch (r) {
        case 1:
            break;
        case 0:
            fputs("timeout", stderr);
            exit(8);
        case -1:
            perror("select");
            exit(1);
        default:
            fputs("unknown error", stderr);
            exit(3);
        }

        r = read(fd, &c, 1);
        if (r == -1) {
            perror("write");
            exit(1);
        }
        if (i >= sizeof(buf)-1) {
            fputs("response too big", stderr);
            exit(4);
        }
        putc(c, stdout);
        fflush(stdout);
        buf[i++] = c;
        if (p == '\r' && c == '\n') {
            break;
        }
        p = c;
    }
    buf[i] = '\0';
    return buf;
}


static void
ckresp(int fd, char *exp)
{
    char *line;

    line = readline(fd);
    assertf(strcmp(exp, line) == 0, "\"%s\" != \"%s\"", exp, line);
}


static void
ckrespsub(int fd, char *sub)
{
    char *line;

    line = readline(fd);
    assertf(strstr(line, sub), "\"%s\" not in \"%s\"", sub, line);
}


static void
killsrv(void)
{
    if (srvpid > 1) {
        kill(srvpid, 9);
    }
}


static int
filesize(char *path)
{
    int r;
    struct stat s;

    r = stat(path, &s);
    if (r == -1) {
        twarn("stat");
        exit(1);
    }
    return s.st_size;
}


static int
exist(char *path)
{
    int r;
    struct stat s;

    r = stat(path, &s);
    return r != -1;
}


static void
pest(char *path)
{
    int port;
    char cmd[100];

    port = SERVER();

    sprintf(cmd, "exec pest localhost:%d %s", port, path);
    assert(system(cmd) == 0);
    killsrv();
}


void
cttestpause()
{
    int port, fd;
    int64 s;

    port = SERVER();
    fd = mustdiallocal(port);
    mustsend(fd, "put 0 0 0 1\r\n");
    mustsend(fd, "x\r\n");
    ckresp(fd, "INSERTED 1\r\n");
    s = nanoseconds();
    mustsend(fd, "pause-tube default 1\r\n");
    ckresp(fd, "PAUSED\r\n");
    timeout = 1100000000; // 1.1s
    mustsend(fd, "reserve\r\n");
    ckresp(fd, "RESERVED 1 1\r\n");
    ckresp(fd, "x\r\n");
    assert(nanoseconds() - s >= 1000000000); // 1s

    killsrv();
}


void
cttestunderscore()
{
    pest("test/underscore.pest");
}


void
cttesttoobig()
{
    job_data_size_limit = 10;
    pest("test/toobig.pest");
}


void
cttestdeleteready()
{
    pest("test/delete-ready.pest");
}


void
cttestmultitube()
{
    pest("test/multi-tube.pest");
}


void
cttestnonegativedelay()
{
    pest("test/no-negative-delay.pest");
}


void
cttestnoomittimeleft()
{
    pest("test/omit-time-left.pest");
}


void
cttestsmalldelay()
{
    pest("test/small-delay.pest");
}


void
ctteststatstube()
{
    pest("test/stats-tube.pest");
}


void
cttestttrlarge()
{
    int port, fd;

    port = SERVER();
    fd = mustdiallocal(port);
    mustsend(fd, "put 0 0 120 1\r\n");
    mustsend(fd, "a\r\n");
    ckresp(fd, "INSERTED 1\r\n");
    mustsend(fd, "put 0 0 4294 1\r\n");
    mustsend(fd, "a\r\n");
    ckresp(fd, "INSERTED 2\r\n");
    mustsend(fd, "put 0 0 4295 1\r\n");
    mustsend(fd, "a\r\n");
    ckresp(fd, "INSERTED 3\r\n");
    mustsend(fd, "put 0 0 4296 1\r\n");
    mustsend(fd, "a\r\n");
    ckresp(fd, "INSERTED 4\r\n");
    mustsend(fd, "put 0 0 4297 1\r\n");
    mustsend(fd, "a\r\n");
    ckresp(fd, "INSERTED 5\r\n");
    mustsend(fd, "put 0 0 5000 1\r\n");
    mustsend(fd, "a\r\n");
    ckresp(fd, "INSERTED 6\r\n");
    mustsend(fd, "put 0 0 21600 1\r\n");
    mustsend(fd, "a\r\n");
    ckresp(fd, "INSERTED 7\r\n");
    mustsend(fd, "stats-job 1\r\n");
    ckrespsub(fd, "OK ");
    ckrespsub(fd, "\nttr: 120\n");
    mustsend(fd, "stats-job 2\r\n");
    ckrespsub(fd, "OK ");
    ckrespsub(fd, "\nttr: 4294\n");
    mustsend(fd, "stats-job 3\r\n");
    ckrespsub(fd, "OK ");
    ckrespsub(fd, "\nttr: 4295\n");
    mustsend(fd, "stats-job 4\r\n");
    ckrespsub(fd, "OK ");
    ckrespsub(fd, "\nttr: 4296\n");
    mustsend(fd, "stats-job 5\r\n");
    ckrespsub(fd, "OK ");
    ckrespsub(fd, "\nttr: 4297\n");
    mustsend(fd, "stats-job 6\r\n");
    ckrespsub(fd, "OK ");
    ckrespsub(fd, "\nttr: 5000\n");
    mustsend(fd, "stats-job 7\r\n");
    ckrespsub(fd, "OK ");
    ckrespsub(fd, "\nttr: 21600\n");

    killsrv();
}


void
cttestttrsmall()
{
    pest("test/ttr-small.pest");
}


void
cttestzerodelay()
{
    pest("test/zero-delay.pest");
}


void
cttestreservewithtimeout2conn()
{
    int port, fd0, fd1;

    job_data_size_limit = 10;

    port = SERVER();
    fd0 = mustdiallocal(port);
    fd1 = mustdiallocal(port);
    mustsend(fd0, "watch foo\r\n");
    ckresp(fd0, "WATCHING 2\r\n");
    mustsend(fd0, "reserve-with-timeout 1\r\n");
    mustsend(fd1, "watch foo\r\n");
    ckresp(fd1, "WATCHING 2\r\n");
    timeout = 1100000000; // 1.1s
    ckresp(fd0, "TIMED_OUT\r\n");

    killsrv();
}


void
cttestbinlogemptyexit()
{
    int port, fd;
    char dir[] = "/tmp/beanstalkd.test.XXXXXX";

    mkdtemp(dir);
    srv.wal.dir = dir;
    srv.wal.use = 1;
    job_data_size_limit = 10;

    port = SERVER();

    killsrv();

    port = SERVER();
    fd = mustdiallocal(port);
    mustsend(fd, "put 0 0 0 0\r\n");
    mustsend(fd, "\r\n");
    ckresp(fd, "INSERTED 1\r\n");

    killsrv();
    execlp("rm", "rm", "-rf", dir, NULL);
}


void
cttestbinlogbury()
{
    int port, fd;
    char dir[] = "/tmp/beanstalkd.test.XXXXXX";

    mkdtemp(dir);
    srv.wal.dir = dir;
    srv.wal.use = 1;
    job_data_size_limit = 10;

    port = SERVER();
    fd = mustdiallocal(port);
    mustsend(fd, "put 0 0 100 0\r\n");
    mustsend(fd, "\r\n");
    ckresp(fd, "INSERTED 1\r\n");
    mustsend(fd, "reserve\r\n");
    ckresp(fd, "RESERVED 1 0\r\n");
    ckresp(fd, "\r\n");
    mustsend(fd, "bury 1 0\r\n");
    ckresp(fd, "BURIED\r\n");

    killsrv();
    execlp("rm", "rm", "-rf", dir, NULL);
}


void
cttestbinlogbasic()
{
    int port, fd;
    char dir[] = "/tmp/beanstalkd.test.XXXXXX";

    mkdtemp(dir);
    srv.wal.dir = dir;
    srv.wal.use = 1;
    job_data_size_limit = 10;

    port = SERVER();
    fd = mustdiallocal(port);
    mustsend(fd, "put 0 0 100 0\r\n");
    mustsend(fd, "\r\n");
    ckresp(fd, "INSERTED 1\r\n");

    killsrv();
    waitpid(srvpid, NULL, 0);

    port = SERVER();
    fd = mustdiallocal(port);
    mustsend(fd, "delete 1\r\n");
    ckresp(fd, "DELETED\r\n");

    killsrv();
    execlp("rm", "rm", "-rf", dir, NULL);
}


void
cttestbinlogsizelimit()
{
    int port, fd, i = 0;
    char dir[] = "/tmp/beanstalkd.test.XXXXXX", *b2;
    int size = 1024, gotsize;

    mkdtemp(dir);
    srv.wal.dir = dir;
    srv.wal.use = 1;
    srv.wal.filesz = size;
    srv.wal.syncrate = 0;
    srv.wal.wantsync = 1;

    port = SERVER();
    fd = mustdiallocal(port);
    b2 = fmtalloc("%s/binlog.2", dir);
    while (!exist(b2)) {
        mustsend(fd, "put 0 0 100 50\r\n");
        mustsend(fd, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\r\n");
        ckresp(fd, fmtalloc("INSERTED %d\r\n", ++i));
    }

    gotsize = filesize(fmtalloc("%s/binlog.1", dir));
    assertf(gotsize == size, "binlog.1 %d != %d", gotsize, size);
    gotsize = filesize(b2);
    assertf(gotsize == size, "binlog.2 %d != %d", gotsize, size);

    killsrv();
    execlp("rm", "rm", "-rf", dir, NULL);
}


void
cttestbinlogallocation()
{
    int port, fd, i = 0;
    char dir[] = "/tmp/beanstalkd.test.XXXXXX";
    int size = 601;

    mkdtemp(dir);
    srv.wal.dir = dir;
    srv.wal.use = 1;
    srv.wal.filesz = size;
    srv.wal.syncrate = 0;
    srv.wal.wantsync = 1;

    port = SERVER();
    fd = mustdiallocal(port);
    for (i = 1; i <= 96; i++) {
        mustsend(fd, "put 0 0 120 22\r\n");
        mustsend(fd, "job payload xxxxxxxxxx\r\n");
        ckresp(fd, fmtalloc("INSERTED %d\r\n", i));
    }
    for (i = 1; i <= 96; i++) {
        mustsend(fd, fmtalloc("delete %d\r\n", i));
        ckresp(fd, "DELETED\r\n");
    }

    killsrv();
    execlp("rm", "rm", "-rf", dir, NULL);
}


static void
writefull(int fd, char *s, int n)
{
    int c;
    for (; n; n -= c) {
        c = write(fd, s, n);
        if (c == -1) {
            perror("write");
            exit(1);
        }
        s += c;
    }
}


static void
mustsend(int fd, char *s)
{
    writefull(fd, s, strlen(s));
    printf(">%d %s", fd, s);
    fflush(stdout);
}
