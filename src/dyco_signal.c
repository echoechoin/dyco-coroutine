#include "dyco/dyco_coroutine.h"

int dyco_signal_waitchild(const pid_t child, int *status, int timeout)
{
	int ret = waitpid(child, status, WNOHANG | WUNTRACED);
	if (timeout == 0 || ret > 0) {
		return ret;
	}

	dyco_schedule *sched = _get_sched();
	if (sched == NULL) {
		return -1;
	}
	dyco_coroutine *co = sched->curr_thread;
	if (co == NULL) {
		return -1;
	}
	DYCO_MUSTNOT(TESTBIT(co->status, COROUTINE_FLAGS_ASYMMETRIC));

	sigset_t sigmask;
	DYCO_MUST(sigemptyset(&sigmask) == 0);
	DYCO_MUST(sigaddset(&sigmask, SIGCHLD) == 0);
	dyco_schedcall_sigprocmask(SIG_BLOCK, &sigmask, &co->old_sigmask);
	int sigfd = signalfd(-1, &sigmask, SFD_NONBLOCK);
	DYCO_MUSTNOT(sigfd == -1);

	struct epoll_event ev;
	ev.data.fd = sigfd;
	ev.events = EPOLLIN | EPOLLHUP | EPOLLERR | EPOLLET;

	DYCO_MUST(epoll_ctl(sched->epollfd, EPOLL_CTL_ADD, sigfd, &ev) == 0);
	_schedule_sched_wait(co, sigfd);
	_schedule_sched_sleep(co, timeout);

	_yield(co);

	_schedule_cancel_sleep(co);
	_schedule_cancel_wait(co, sigfd);
	DYCO_MUST(epoll_ctl(sched->epollfd, EPOLL_CTL_DEL, sigfd, NULL) == 0);
	
	close(sigfd);
	return waitpid(child, status, WNOHANG | WUNTRACED);
}


int dyco_signal_init(sigset_t *mask)
{
	dyco_schedule *sched = _get_sched();
	if (sched == NULL) {
		return -1;
	}
	dyco_coroutine *co = sched->curr_thread;
	if (co == NULL) {
		return -1;
	}
	
	if (TESTBIT(co->status, COROUTINE_FLAGS_WAITSIGNAL)) {
		return -1;
	}
	dyco_schedcall_sigprocmask(SIG_BLOCK, mask, &co->old_sigmask);
	int sigfd = signalfd(-1, mask, SFD_NONBLOCK);
	DYCO_MUSTNOT(sigfd == -1);

	co->sigfd = sigfd;
	SETBIT(co->status, COROUTINE_FLAGS_WAITSIGNAL);

	return 0;
}


void dyco_signal_destroy()
{
	dyco_schedule *sched = _get_sched();
	if (sched == NULL) {
		return;
	}
	dyco_coroutine *co = sched->curr_thread;
	if (co == NULL) {
		return;
	}
	if (!TESTBIT(co->status, COROUTINE_FLAGS_WAITSIGNAL)) {
		return;
	}
	
	CLRBIT(co->status, COROUTINE_FLAGS_WAITSIGNAL);
	close(co->sigfd);

	dyco_schedcall_sigprocmask(SIG_SETMASK, &co->old_sigmask, NULL);

	return;
}


int dyco_signal_wait(struct signalfd_siginfo *sinfo, int timeout)
{
	dyco_schedule *sched = _get_sched();
	if (sched == NULL) {
		return -1;
	}
	dyco_coroutine *co = sched->curr_thread;
	if (co == NULL) {
		return -1;
	}
	assert(!TESTBIT(co->status, COROUTINE_FLAGS_ASYMMETRIC));
	if (!TESTBIT(co->status, COROUTINE_FLAGS_WAITSIGNAL)) {
		return -1;
	}
	
	int ret;
	ret = read(co->sigfd, sinfo, sizeof(struct signalfd_siginfo));
	if (timeout == 0 || ret > 0) {
		return ret;
	}

	struct epoll_event ev;
	ev.data.fd = co->sigfd;
	ev.events = EPOLLIN | EPOLLHUP | EPOLLERR | EPOLLET;
	DYCO_MUST(epoll_ctl(sched->epollfd, EPOLL_CTL_ADD, co->sigfd, &ev) == 0);
	_schedule_sched_wait(co, ev.data.fd);
	
	_schedule_sched_sleep(co, timeout);
	
	_yield(co);

	_schedule_cancel_sleep(co);

	_schedule_cancel_wait(co, co->sigfd);
	DYCO_MUST(epoll_ctl(sched->epollfd, EPOLL_CTL_DEL, co->sigfd, NULL) == 0);

	return read(co->sigfd, sinfo, sizeof(struct signalfd_siginfo));
}
