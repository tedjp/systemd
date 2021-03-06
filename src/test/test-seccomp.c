/* SPDX-License-Identifier: LGPL-2.1+ */
/***
  This file is part of systemd.

  Copyright 2016 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <poll.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <unistd.h>

#include "alloc-util.h"
#include "fd-util.h"
#include "macro.h"
#include "missing.h"
#include "nsflags.h"
#include "process-util.h"
#include "raw-clone.h"
#include "seccomp-util.h"
#include "set.h"
#include "string-util.h"
#include "util.h"
#include "virt.h"

#if SCMP_SYS(socket) < 0 || defined(__i386__) || defined(__s390x__) || defined(__s390__)
/* On these archs, socket() is implemented via the socketcall() syscall multiplexer,
 * and we can't restrict it hence via seccomp. */
#  define SECCOMP_RESTRICT_ADDRESS_FAMILIES_BROKEN 1
#else
#  define SECCOMP_RESTRICT_ADDRESS_FAMILIES_BROKEN 0
#endif

static void test_seccomp_arch_to_string(void) {
        uint32_t a, b;
        const char *name;

        a = seccomp_arch_native();
        assert_se(a > 0);
        name = seccomp_arch_to_string(a);
        assert_se(name);
        assert_se(seccomp_arch_from_string(name, &b) >= 0);
        assert_se(a == b);
}

static void test_architecture_table(void) {
        const char *n, *n2;

        NULSTR_FOREACH(n,
                       "native\0"
                       "x86\0"
                       "x86-64\0"
                       "x32\0"
                       "arm\0"
                       "arm64\0"
                       "mips\0"
                       "mips64\0"
                       "mips64-n32\0"
                       "mips-le\0"
                       "mips64-le\0"
                       "mips64-le-n32\0"
                       "ppc\0"
                       "ppc64\0"
                       "ppc64-le\0"
                       "s390\0"
                       "s390x\0") {
                uint32_t c;

                assert_se(seccomp_arch_from_string(n, &c) >= 0);
                n2 = seccomp_arch_to_string(c);
                log_info("seccomp-arch: %s → 0x%"PRIx32" → %s", n, c, n2);
                assert_se(streq_ptr(n, n2));
        }
}

static void test_syscall_filter_set_find(void) {
        assert_se(!syscall_filter_set_find(NULL));
        assert_se(!syscall_filter_set_find(""));
        assert_se(!syscall_filter_set_find("quux"));
        assert_se(!syscall_filter_set_find("@quux"));

        assert_se(syscall_filter_set_find("@clock") == syscall_filter_sets + SYSCALL_FILTER_SET_CLOCK);
        assert_se(syscall_filter_set_find("@default") == syscall_filter_sets + SYSCALL_FILTER_SET_DEFAULT);
        assert_se(syscall_filter_set_find("@raw-io") == syscall_filter_sets + SYSCALL_FILTER_SET_RAW_IO);
}

static void test_filter_sets(void) {
        unsigned i;
        int r;

        if (!is_seccomp_available())
                return;
        if (geteuid() != 0)
                return;

        for (i = 0; i < _SYSCALL_FILTER_SET_MAX; i++) {
                pid_t pid;

                log_info("Testing %s", syscall_filter_sets[i].name);

                pid = fork();
                assert_se(pid >= 0);

                if (pid == 0) { /* Child? */
                        int fd;

                        if (i == SYSCALL_FILTER_SET_DEFAULT) /* if we look at the default set, whitelist instead of blacklist */
                                r = seccomp_load_syscall_filter_set(SCMP_ACT_ERRNO(EUCLEAN), syscall_filter_sets + i, SCMP_ACT_ALLOW);
                        else
                                r = seccomp_load_syscall_filter_set(SCMP_ACT_ALLOW, syscall_filter_sets + i, SCMP_ACT_ERRNO(EUCLEAN));
                        if (r < 0)
                                _exit(EXIT_FAILURE);

                        /* Test the sycall filter with one random system call */
                        fd = eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC);
                        if (IN_SET(i, SYSCALL_FILTER_SET_IO_EVENT, SYSCALL_FILTER_SET_DEFAULT))
                                assert_se(fd < 0 && errno == EUCLEAN);
                        else {
                                assert_se(fd >= 0);
                                safe_close(fd);
                        }

                        _exit(EXIT_SUCCESS);
                }

                assert_se(wait_for_terminate_and_warn(syscall_filter_sets[i].name, pid, true) == EXIT_SUCCESS);
        }
}

static void test_restrict_namespace(void) {
        _cleanup_free_ char *s = NULL;
        unsigned long ul;
        pid_t pid;

        assert_se(namespace_flag_to_string(0) == NULL);
        assert_se(streq(namespace_flag_to_string(CLONE_NEWNS), "mnt"));
        assert_se(namespace_flag_to_string(CLONE_NEWNS|CLONE_NEWIPC) == NULL);
        assert_se(streq(namespace_flag_to_string(CLONE_NEWCGROUP), "cgroup"));

        assert_se(namespace_flag_from_string("mnt") == CLONE_NEWNS);
        assert_se(namespace_flag_from_string(NULL) == 0);
        assert_se(namespace_flag_from_string("") == 0);
        assert_se(namespace_flag_from_string("uts") == CLONE_NEWUTS);
        assert_se(namespace_flag_from_string(namespace_flag_to_string(CLONE_NEWUTS)) == CLONE_NEWUTS);
        assert_se(streq(namespace_flag_to_string(namespace_flag_from_string("ipc")), "ipc"));

        assert_se(namespace_flag_from_string_many(NULL, &ul) == 0 && ul == 0);
        assert_se(namespace_flag_from_string_many("", &ul) == 0 && ul == 0);
        assert_se(namespace_flag_from_string_many("mnt uts ipc", &ul) == 0 && ul == (CLONE_NEWNS|CLONE_NEWUTS|CLONE_NEWIPC));

        assert_se(namespace_flag_to_string_many(NAMESPACE_FLAGS_ALL, &s) == 0);
        assert_se(streq(s, "cgroup ipc net mnt pid user uts"));
        assert_se(namespace_flag_from_string_many(s, &ul) == 0 && ul == NAMESPACE_FLAGS_ALL);

        if (!is_seccomp_available())
                return;
        if (geteuid() != 0)
                return;

        pid = fork();
        assert_se(pid >= 0);

        if (pid == 0) {

                assert_se(seccomp_restrict_namespaces(CLONE_NEWNS|CLONE_NEWNET) >= 0);

                assert_se(unshare(CLONE_NEWNS) == 0);
                assert_se(unshare(CLONE_NEWNET) == 0);
                assert_se(unshare(CLONE_NEWUTS) == -1);
                assert_se(errno == EPERM);
                assert_se(unshare(CLONE_NEWIPC) == -1);
                assert_se(errno == EPERM);
                assert_se(unshare(CLONE_NEWNET|CLONE_NEWUTS) == -1);
                assert_se(errno == EPERM);

                /* We use fd 0 (stdin) here, which of course will fail with EINVAL on setns(). Except of course our
                 * seccomp filter worked, and hits first and makes it return EPERM */
                assert_se(setns(0, CLONE_NEWNS) == -1);
                assert_se(errno == EINVAL);
                assert_se(setns(0, CLONE_NEWNET) == -1);
                assert_se(errno == EINVAL);
                assert_se(setns(0, CLONE_NEWUTS) == -1);
                assert_se(errno == EPERM);
                assert_se(setns(0, CLONE_NEWIPC) == -1);
                assert_se(errno == EPERM);
                assert_se(setns(0, CLONE_NEWNET|CLONE_NEWUTS) == -1);
                assert_se(errno == EPERM);
                assert_se(setns(0, 0) == -1);
                assert_se(errno == EPERM);

                pid = raw_clone(CLONE_NEWNS);
                assert_se(pid >= 0);
                if (pid == 0)
                        _exit(EXIT_SUCCESS);
                pid = raw_clone(CLONE_NEWNET);
                assert_se(pid >= 0);
                if (pid == 0)
                        _exit(EXIT_SUCCESS);
                pid = raw_clone(CLONE_NEWUTS);
                assert_se(pid < 0);
                assert_se(errno == EPERM);
                pid = raw_clone(CLONE_NEWIPC);
                assert_se(pid < 0);
                assert_se(errno == EPERM);
                pid = raw_clone(CLONE_NEWNET|CLONE_NEWUTS);
                assert_se(pid < 0);
                assert_se(errno == EPERM);

                _exit(EXIT_SUCCESS);
        }

        assert_se(wait_for_terminate_and_warn("nsseccomp", pid, true) == EXIT_SUCCESS);
}

static void test_protect_sysctl(void) {
        pid_t pid;

        if (!is_seccomp_available())
                return;
        if (geteuid() != 0)
                return;

        if (detect_container() > 0) /* in containers _sysctl() is likely missing anyway */
                return;

        pid = fork();
        assert_se(pid >= 0);

        if (pid == 0) {
#if __NR__sysctl > 0
                assert_se(syscall(__NR__sysctl, NULL) < 0);
                assert_se(errno == EFAULT);
#endif

                assert_se(seccomp_protect_sysctl() >= 0);

#if __NR__sysctl > 0
                assert_se(syscall(__NR__sysctl, 0, 0, 0) < 0);
                assert_se(errno == EPERM);
#endif

                _exit(EXIT_SUCCESS);
        }

        assert_se(wait_for_terminate_and_warn("sysctlseccomp", pid, true) == EXIT_SUCCESS);
}

static void test_restrict_address_families(void) {
        pid_t pid;

        if (!is_seccomp_available())
                return;
        if (geteuid() != 0)
                return;

        pid = fork();
        assert_se(pid >= 0);

        if (pid == 0) {
                int fd;
                Set *s;

                fd = socket(AF_INET, SOCK_DGRAM, 0);
                assert_se(fd >= 0);
                safe_close(fd);

                fd = socket(AF_UNIX, SOCK_DGRAM, 0);
                assert_se(fd >= 0);
                safe_close(fd);

                fd = socket(AF_NETLINK, SOCK_DGRAM, 0);
                assert_se(fd >= 0);
                safe_close(fd);

                assert_se(s = set_new(NULL));
                assert_se(set_put(s, INT_TO_PTR(AF_UNIX)) >= 0);

                assert_se(seccomp_restrict_address_families(s, false) >= 0);

                fd = socket(AF_INET, SOCK_DGRAM, 0);
                assert_se(fd >= 0);
                safe_close(fd);

                fd = socket(AF_UNIX, SOCK_DGRAM, 0);
#if SECCOMP_RESTRICT_ADDRESS_FAMILIES_BROKEN
                assert_se(fd >= 0);
                safe_close(fd);
#else
                assert_se(fd < 0);
                assert_se(errno == EAFNOSUPPORT);
#endif

                fd = socket(AF_NETLINK, SOCK_DGRAM, 0);
                assert_se(fd >= 0);
                safe_close(fd);

                set_clear(s);

                assert_se(set_put(s, INT_TO_PTR(AF_INET)) >= 0);

                assert_se(seccomp_restrict_address_families(s, true) >= 0);

                fd = socket(AF_INET, SOCK_DGRAM, 0);
                assert_se(fd >= 0);
                safe_close(fd);

                fd = socket(AF_UNIX, SOCK_DGRAM, 0);
#if SECCOMP_RESTRICT_ADDRESS_FAMILIES_BROKEN
                assert_se(fd >= 0);
                safe_close(fd);
#else
                assert_se(fd < 0);
                assert_se(errno == EAFNOSUPPORT);
#endif

                fd = socket(AF_NETLINK, SOCK_DGRAM, 0);
#if SECCOMP_RESTRICT_ADDRESS_FAMILIES_BROKEN
                assert_se(fd >= 0);
                safe_close(fd);
#else
                assert_se(fd < 0);
                assert_se(errno == EAFNOSUPPORT);
#endif

                _exit(EXIT_SUCCESS);
        }

        assert_se(wait_for_terminate_and_warn("socketseccomp", pid, true) == EXIT_SUCCESS);
}

static void test_restrict_realtime(void) {
        pid_t pid;

        if (!is_seccomp_available())
                return;
        if (geteuid() != 0)
                return;

        if (detect_container() > 0) /* in containers RT privs are likely missing anyway */
                return;

        pid = fork();
        assert_se(pid >= 0);

        if (pid == 0) {
                assert_se(sched_setscheduler(0, SCHED_FIFO, &(struct sched_param) { .sched_priority = 1 }) >= 0);
                assert_se(sched_setscheduler(0, SCHED_RR, &(struct sched_param) { .sched_priority = 1 }) >= 0);
                assert_se(sched_setscheduler(0, SCHED_IDLE, &(struct sched_param) { .sched_priority = 0 }) >= 0);
                assert_se(sched_setscheduler(0, SCHED_BATCH, &(struct sched_param) { .sched_priority = 0 }) >= 0);
                assert_se(sched_setscheduler(0, SCHED_OTHER, &(struct sched_param) {}) >= 0);

                assert_se(seccomp_restrict_realtime() >= 0);

                assert_se(sched_setscheduler(0, SCHED_IDLE, &(struct sched_param) { .sched_priority = 0 }) >= 0);
                assert_se(sched_setscheduler(0, SCHED_BATCH, &(struct sched_param) { .sched_priority = 0 }) >= 0);
                assert_se(sched_setscheduler(0, SCHED_OTHER, &(struct sched_param) {}) >= 0);

                assert_se(sched_setscheduler(0, SCHED_FIFO, &(struct sched_param) { .sched_priority = 1 }) < 0);
                assert_se(errno == EPERM);
                assert_se(sched_setscheduler(0, SCHED_RR, &(struct sched_param) { .sched_priority = 1 }) < 0);
                assert_se(errno == EPERM);

                _exit(EXIT_SUCCESS);
        }

        assert_se(wait_for_terminate_and_warn("realtimeseccomp", pid, true) == EXIT_SUCCESS);
}

static void test_memory_deny_write_execute_mmap(void) {
        pid_t pid;

        if (!is_seccomp_available())
                return;
        if (geteuid() != 0)
                return;

        pid = fork();
        assert_se(pid >= 0);

        if (pid == 0) {
                void *p;

                p = mmap(NULL, page_size(), PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS, -1,0);
                assert_se(p != MAP_FAILED);
                assert_se(munmap(p, page_size()) >= 0);

                p = mmap(NULL, page_size(), PROT_WRITE|PROT_READ, MAP_PRIVATE|MAP_ANONYMOUS, -1,0);
                assert_se(p != MAP_FAILED);
                assert_se(munmap(p, page_size()) >= 0);

                assert_se(seccomp_memory_deny_write_execute() >= 0);

                p = mmap(NULL, page_size(), PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS, -1,0);
#if defined(__x86_64__) || defined(__i386__) || defined(__powerpc64__) || defined(__arm__) || defined(__aarch64__)
                assert_se(p == MAP_FAILED);
                assert_se(errno == EPERM);
#else /* unknown architectures */
                assert_se(p != MAP_FAILED);
                assert_se(munmap(p, page_size()) >= 0);
#endif

                p = mmap(NULL, page_size(), PROT_WRITE|PROT_READ, MAP_PRIVATE|MAP_ANONYMOUS, -1,0);
                assert_se(p != MAP_FAILED);
                assert_se(munmap(p, page_size()) >= 0);

                _exit(EXIT_SUCCESS);
        }

        assert_se(wait_for_terminate_and_warn("memoryseccomp-mmap", pid, true) == EXIT_SUCCESS);
}

static void test_memory_deny_write_execute_shmat(void) {
        int shmid;
        pid_t pid;

        if (!is_seccomp_available())
                return;
        if (geteuid() != 0)
                return;

        shmid = shmget(IPC_PRIVATE, page_size(), 0);
        assert_se(shmid >= 0);

        pid = fork();
        assert_se(pid >= 0);

        if (pid == 0) {
                void *p;

                p = shmat(shmid, NULL, 0);
                assert_se(p != MAP_FAILED);
                assert_se(shmdt(p) == 0);

                p = shmat(shmid, NULL, SHM_EXEC);
                assert_se(p != MAP_FAILED);
                assert_se(shmdt(p) == 0);

                assert_se(seccomp_memory_deny_write_execute() >= 0);

                p = shmat(shmid, NULL, SHM_EXEC);
#if defined(__x86_64__) || defined(__arm__) || defined(__aarch64__)
                assert_se(p == MAP_FAILED);
                assert_se(errno == EPERM);
#else /* __i386__, __powerpc64__, and "unknown" architectures */
                assert_se(p != MAP_FAILED);
                assert_se(shmdt(p) == 0);
#endif

                p = shmat(shmid, NULL, 0);
                assert_se(p != MAP_FAILED);
                assert_se(shmdt(p) == 0);

                _exit(EXIT_SUCCESS);
        }

        assert_se(wait_for_terminate_and_warn("memoryseccomp-shmat", pid, true) == EXIT_SUCCESS);
}

static void test_restrict_archs(void) {
        pid_t pid;

        if (!is_seccomp_available())
                return;
        if (geteuid() != 0)
                return;

        pid = fork();
        assert_se(pid >= 0);

        if (pid == 0) {
                _cleanup_set_free_ Set *s = NULL;

                assert_se(access("/", F_OK) >= 0);

                assert_se(s = set_new(NULL));

#ifdef __x86_64__
                assert_se(set_put(s, UINT32_TO_PTR(SCMP_ARCH_X86+1)) >= 0);
#endif
                assert_se(seccomp_restrict_archs(s) >= 0);

                assert_se(access("/", F_OK) >= 0);
                assert_se(seccomp_restrict_archs(NULL) >= 0);

                assert_se(access("/", F_OK) >= 0);

                _exit(EXIT_SUCCESS);
        }

        assert_se(wait_for_terminate_and_warn("archseccomp", pid, true) == EXIT_SUCCESS);
}

static void test_load_syscall_filter_set_raw(void) {
        pid_t pid;

        if (!is_seccomp_available())
                return;
        if (geteuid() != 0)
                return;

        pid = fork();
        assert_se(pid >= 0);

        if (pid == 0) {
                _cleanup_hashmap_free_ Hashmap *s = NULL;

                assert_se(access("/", F_OK) >= 0);
                assert_se(poll(NULL, 0, 0) == 0);

                assert_se(seccomp_load_syscall_filter_set_raw(SCMP_ACT_ALLOW, NULL, SCMP_ACT_KILL) >= 0);
                assert_se(access("/", F_OK) >= 0);
                assert_se(poll(NULL, 0, 0) == 0);

                assert_se(s = hashmap_new(NULL));
#if SCMP_SYS(access) >= 0
                assert_se(hashmap_put(s, UINT32_TO_PTR(__NR_access + 1), INT_TO_PTR(-1)) >= 0);
#else
                assert_se(hashmap_put(s, UINT32_TO_PTR(__NR_faccessat + 1), INT_TO_PTR(-1)) >= 0);
#endif

                assert_se(seccomp_load_syscall_filter_set_raw(SCMP_ACT_ALLOW, s, SCMP_ACT_ERRNO(EUCLEAN)) >= 0);

                assert_se(access("/", F_OK) < 0);
                assert_se(errno == EUCLEAN);

                assert_se(poll(NULL, 0, 0) == 0);

                s = hashmap_free(s);

                assert_se(s = hashmap_new(NULL));
#if SCMP_SYS(access) >= 0
                assert_se(hashmap_put(s, UINT32_TO_PTR(__NR_access + 1), INT_TO_PTR(EILSEQ)) >= 0);
#else
                assert_se(hashmap_put(s, UINT32_TO_PTR(__NR_faccessat + 1), INT_TO_PTR(EILSEQ)) >= 0);
#endif

                assert_se(seccomp_load_syscall_filter_set_raw(SCMP_ACT_ALLOW, s, SCMP_ACT_ERRNO(EUCLEAN)) >= 0);

                assert_se(access("/", F_OK) < 0);
                assert_se(errno == EILSEQ);

                assert_se(poll(NULL, 0, 0) == 0);

                s = hashmap_free(s);

                assert_se(s = hashmap_new(NULL));
#if SCMP_SYS(poll) >= 0
                assert_se(hashmap_put(s, UINT32_TO_PTR(__NR_poll + 1), INT_TO_PTR(-1)) >= 0);
#else
                assert_se(hashmap_put(s, UINT32_TO_PTR(__NR_ppoll + 1), INT_TO_PTR(-1)) >= 0);
#endif

                assert_se(seccomp_load_syscall_filter_set_raw(SCMP_ACT_ALLOW, s, SCMP_ACT_ERRNO(EUNATCH)) >= 0);

                assert_se(access("/", F_OK) < 0);
                assert_se(errno == EILSEQ);

                assert_se(poll(NULL, 0, 0) < 0);
                assert_se(errno == EUNATCH);

                s = hashmap_free(s);

                assert_se(s = hashmap_new(NULL));
#if SCMP_SYS(poll) >= 0
                assert_se(hashmap_put(s, UINT32_TO_PTR(__NR_poll + 1), INT_TO_PTR(EILSEQ)) >= 0);
#else
                assert_se(hashmap_put(s, UINT32_TO_PTR(__NR_ppoll + 1), INT_TO_PTR(EILSEQ)) >= 0);
#endif

                assert_se(seccomp_load_syscall_filter_set_raw(SCMP_ACT_ALLOW, s, SCMP_ACT_ERRNO(EUNATCH)) >= 0);

                assert_se(access("/", F_OK) < 0);
                assert_se(errno == EILSEQ);

                assert_se(poll(NULL, 0, 0) < 0);
                assert_se(errno == EILSEQ);

                _exit(EXIT_SUCCESS);
        }

        assert_se(wait_for_terminate_and_warn("syscallrawseccomp", pid, true) == EXIT_SUCCESS);
}

static void test_lock_personality(void) {
        unsigned long current;
        pid_t pid;

        if (!is_seccomp_available())
                return;
        if (geteuid() != 0)
                return;

        assert_se(opinionated_personality(&current) >= 0);

        log_info("current personality=%lu", current);

        pid = fork();
        assert_se(pid >= 0);

        if (pid == 0) {
                assert_se(seccomp_lock_personality(current) >= 0);

                assert_se((unsigned long) safe_personality(current) == current);

                /* Note, we also test that safe_personality() works correctly, by checkig whether errno is properly
                 * set, in addition to the return value */
                errno = 0;
                assert_se(safe_personality(PER_LINUX | ADDR_NO_RANDOMIZE) == -EPERM);
                assert_se(errno == EPERM);

                assert_se(safe_personality(PER_LINUX | MMAP_PAGE_ZERO) == -EPERM);
                assert_se(safe_personality(PER_LINUX | ADDR_COMPAT_LAYOUT) == -EPERM);
                assert_se(safe_personality(PER_LINUX | READ_IMPLIES_EXEC) == -EPERM);
                assert_se(safe_personality(PER_LINUX_32BIT) == -EPERM);
                assert_se(safe_personality(PER_SVR4) == -EPERM);
                assert_se(safe_personality(PER_BSD) == -EPERM);
                assert_se(safe_personality(current == PER_LINUX ? PER_LINUX32 : PER_LINUX) == -EPERM);
                assert_se(safe_personality(PER_LINUX32_3GB) == -EPERM);
                assert_se(safe_personality(PER_UW7) == -EPERM);
                assert_se(safe_personality(0x42) == -EPERM);

                assert_se(safe_personality(PERSONALITY_INVALID) == -EPERM); /* maybe remove this later */

                assert_se((unsigned long) personality(current) == current);
                _exit(EXIT_SUCCESS);
        }

        assert_se(wait_for_terminate_and_warn("lockpersonalityseccomp", pid, true) == EXIT_SUCCESS);
}

static void test_filter_sets_ordered(void) {
        size_t i;

        /* Ensure "@default" always remains at the beginning of the list */
        assert_se(SYSCALL_FILTER_SET_DEFAULT == 0);
        assert_se(streq(syscall_filter_sets[0].name, "@default"));

        for (i = 0; i < _SYSCALL_FILTER_SET_MAX; i++) {
                const char *k, *p = NULL;

                /* Make sure each group has a description */
                assert_se(!isempty(syscall_filter_sets[0].help));

                /* Make sure the groups are ordered alphabetically, except for the first entry */
                assert_se(i < 2 || strcmp(syscall_filter_sets[i-1].name, syscall_filter_sets[i].name) < 0);

                NULSTR_FOREACH(k, syscall_filter_sets[i].value) {

                        /* Ensure each syscall list is in itself ordered, but groups before names */
                        assert_se(!p ||
                                  (*p == '@' && *k != '@') ||
                                  (((*p == '@' && *k == '@') ||
                                    (*p != '@' && *k != '@')) &&
                                   strcmp(p, k) < 0));

                        p = k;
                }
        }
}

int main(int argc, char *argv[]) {

        log_set_max_level(LOG_DEBUG);

        test_seccomp_arch_to_string();
        test_architecture_table();
        test_syscall_filter_set_find();
        test_filter_sets();
        test_restrict_namespace();
        test_protect_sysctl();
        test_restrict_address_families();
        test_restrict_realtime();
        test_memory_deny_write_execute_mmap();
        test_memory_deny_write_execute_shmat();
        test_restrict_archs();
        test_load_syscall_filter_set_raw();
        test_lock_personality();
        test_filter_sets_ordered();

        return 0;
}
