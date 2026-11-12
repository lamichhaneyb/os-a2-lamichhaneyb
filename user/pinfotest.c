// Tests for getpinfo() and the per-process scheduling counters.
// Run with: make CPUS=1 qemu, then `pinfotest`.

#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/pinfo.h"
#include "user/user.h"

#define RUNNING 4 // enum procstate in kernel/proc.h
#define SPIN 20   // ticks to spin

static struct pinfo buf[NPROC + 1];
static int fails = 0;

static void
check(int ok, char *what)
{
  printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok)
    fails++;
}

// Fill *out with this process's entry; return 0 on success.
static int
self(struct pinfo *out)
{
  int n = getpinfo(buf, NPROC);
  int pid = getpid();
  for (int i = 0; i < n; i++) {
    if (buf[i].pid == pid) {
      *out = buf[i];
      return 0;
    }
  }
  return -1;
}

static void
spin(int nticks)
{
  int t0 = uptime();
  while (uptime() - t0 < nticks)
    ;
}

int
main(void)
{
  struct pinfo a, b;

  // --- argument handling ---
  check(getpinfo(buf, -1) == -1, "max < 0 returns -1");
  check(getpinfo(buf, 0) == 0, "max == 0 returns 0");
  char *bad = sbrk(0) + (1 << 20); // beyond the end of our memory
  check(getpinfo((struct pinfo *)bad, NPROC) == -1, "bad buffer returns -1");

  buf[1].pid = -12345;
  check(getpinfo(buf, 1) == 1, "max == 1 returns 1");
  check(buf[1].pid == -12345, "max == 1 writes only one entry");

  int n = getpinfo(buf, NPROC);
  check(n >= 3, "at least init, sh and pinfotest listed");
  check(n >= 1 && buf[0].pid == 1 && strcmp(buf[0].name, "init") == 0,
        "first entry is init (pid 1)");
  int zero = 0;
  for (int i = 0; i < n; i++)
    if (buf[i].state == 0)
      zero = 1;
  check(!zero, "no UNUSED entries reported");

  // --- our own entry ---
  check(self(&a) == 0, "found self");
  check(a.state == RUNNING, "self is RUNNING");
  check(strcmp(a.name, "pinfotest") == 0, "self name is pinfotest");
  check(a.level >= 0 && a.level <= 3, "level in 0..3");
  check(a.ctime >= 0 && a.ctime <= uptime(), "ctime <= uptime");
  check(a.nsched >= 1, "nsched >= 1");

  // --- charged ~every tick while spinning alone ---
  self(&a);
  spin(SPIN);
  self(&b);
  int dr = b.runticks - a.runticks;
  printf("     spun %d ticks: runticks +%d, nsched +%d\n", SPIN, dr,
         b.nsched - a.nsched);
  check(dr >= SPIN - 3 && dr <= SPIN + 3, "runticks grows ~1 per tick");
  check(b.nsched > a.nsched, "nsched grows");
  check(b.ctime == a.ctime, "ctime unchanged");

  // --- child's counters start at 0 ---
  int parent_ctime = b.ctime;
  int pid = fork();
  if (pid < 0) {
    printf("FAIL: fork\n");
    exit(1);
  }
  if (pid == 0) {
    struct pinfo c;
    int bad = 0;
    if (self(&c) < 0) {
      printf("FAIL: child not found\n");
      exit(1);
    }
    printf("     child: runticks %d, nsched %d, ctime %d, level %d\n",
           c.runticks, c.nsched, c.ctime, c.level);
    if (c.runticks > 1) {
      printf("FAIL: child runticks not reset\n");
      bad = 1;
    }
    if (c.nsched < 1 || c.nsched > 2) {
      printf("FAIL: child nsched not reset\n");
      bad = 1;
    }
    if (c.ctime < parent_ctime) {
      printf("FAIL: child ctime before parent's\n");
      bad = 1;
    }
    if (c.level != 0) {
      printf("FAIL: child does not start at level 0\n");
      bad = 1;
    }
    exit(bad);
  }
  int status = -1;
  wait(&status);
  check(status == 0, "child counters start fresh");

  if (fails == 0)
    printf("pinfotest: all tests passed\n");
  else
    printf("pinfotest: %d test(s) FAILED\n", fails);
  exit(fails != 0);
}
