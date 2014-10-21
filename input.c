/*
    KindlePDFViewer: input abstraction for Lua
    Copyright (C) 2011 Hans-Werner Hilse <hilse@web.de>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "popen-noshell/popen_noshell.h"
#include <err.h>
#include <stdio.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>

#include <linux/input.h>

#include "input.h"
#include <sys/types.h>
#include <sys/wait.h>

#define CODE_IN_SAVER		10000
#define CODE_OUT_SAVER		10001
#define CODE_USB_PLUG_IN	10010
#define CODE_USB_PLUG_OUT	10011
#define CODE_CHARGING		10020
#define CODE_NOT_CHARGING	10021

#define NUM_FDS 4
int inputfds[4] = { -1, -1, -1, -1 };
pid_t slider_pid = -1;
struct popen_noshell_pass_to_pclose pclose_arg;

void slider_handler(int sig)
{
	/* Kill lipc-wait-event properly on exit */
	if(pclose_arg.pid != 0) {
		// Be a little more gracious, lipc seems to handle SIGINT properly
		kill(pclose_arg.pid, SIGINT);
	}
}

int findFreeFdSlot() {
	int i;
	for(i=0; i<NUM_FDS; i++) {
		if(inputfds[i] == -1) {
			return i;
		}
	}
	return -1;
}

static int openInputDevice(lua_State *L) {
	const char* inputdevice = luaL_checkstring(L, 1);
	int fd;
	int childpid;
	fd = findFreeFdSlot();
	if(fd == -1) {
		return luaL_error(L, "no free slot for new input device <%s>", inputdevice);
	}
	if(!strcmp("fake_events", inputdevice)) {
		/* special case: the power slider */
		int pipefd[2];

		pipe(pipefd);
		if((childpid = fork()) == -1) {
			return luaL_error(L, "cannot fork() slider event listener");
		}
		if(childpid == 0) {
			// We send a SIGTERM to this child on exit, trap it to kill lipc properly.
			signal(SIGTERM, slider_handler);

			FILE *fp;
			char std_out[256];
			int status;
			struct input_event ev;
			__u16 key_code = 10000;

			close(pipefd[0]);

			ev.type = EV_KEY;
			ev.code = key_code;
			ev.value = 1;

			/* listen power slider events (listen for ever for multiple events) */
			char *argv[] = {"lipc-wait-event", "-m", "-s", "0", "com.lab126.powerd", "goingToScreenSaver,outOfScreenSaver,charging,notCharging", (char *) NULL};
			/* @TODO  07.06 2012 (houqp)
			*  plugin and out event can only be watched by:
				lipc-wait-event com.lab126.hal usbPlugOut,usbPlugIn
			*/

			fp = popen_noshell("lipc-wait-event", (const char * const *)argv, "r", &pclose_arg, 0);
			if (!fp) {
				err(EXIT_FAILURE, "popen_noshell()");
			}

			/* Flush to get rid of buffering issues? */
			fflush(fp);

			while(fgets(std_out, sizeof(std_out)-1, fp)) {
				if(std_out[0] == 'g') {
					ev.code = CODE_IN_SAVER;
				} else if(std_out[0] == 'o') {
					ev.code = CODE_OUT_SAVER;
				} else if((std_out[0] == 'u') && (std_out[7] == 'I')) {
					ev.code = CODE_USB_PLUG_IN;
				} else if((std_out[0] == 'u') && (std_out[7] == 'O')) {
					ev.code = CODE_USB_PLUG_OUT;
				} else if(std_out[0] == 'c') {
					ev.code = CODE_CHARGING;
				} else if(std_out[0] == 'n') {
					ev.code = CODE_NOT_CHARGING;
				} else {
					printf("Unrecognized event.\n");
				}
				/* fill event struct */
				gettimeofday(&ev.time, NULL);

				/* generate event */
				if(write(pipefd[1], &ev, sizeof(struct input_event)) == -1) {
					printf("Failed to generate event.\n");
				}
			}

			status = pclose_noshell(&pclose_arg);
			if (status == -1) {
				err(EXIT_FAILURE, "pclose_noshell()");
			} else {
				if (WIFEXITED(status)) {
					printf("lipc-wait-event exited normally with status: %d\n", WEXITSTATUS(status));
				} else if (WIFSIGNALED(status)) {
					printf("lipc-wait-event was killed by signal %d\n", WTERMSIG(status));
				} else if (WIFSTOPPED(status)) {
					printf("lipc-wait-event was stopped by signal %d\n", WSTOPSIG(status));
				} else if (WIFCONTINUED(status)) {
					printf("lipc-wait-event continued\n");
				}
			}

			// We're done, go away :).
			_exit(EXIT_SUCCESS);
		} else {
			close(pipefd[1]);
			inputfds[fd] = pipefd[0];
			slider_pid = childpid;
		}
	} else {
		inputfds[fd] = open(inputdevice, O_RDONLY | O_NONBLOCK, 0);
		if(inputfds[fd] != -1) {
			ioctl(inputfds[fd], EVIOCGRAB, 1);
			return 0;
		} else {
			return luaL_error(L, "error opening input device <%s>: %d", inputdevice, errno);
		}
	}
	return 0;
}

static int closeInputDevices(lua_State *L) {
	int i;
	for(i=0; i<NUM_FDS; i++) {
		if(inputfds[i] != -1) {
			ioctl(inputfds[i], EVIOCGRAB, 0);
			close(inputfds[i]);
		}
	}
	if(slider_pid != -1) {
		/* kill and wait for child process */
		kill(slider_pid, SIGTERM);
		waitpid(-1, NULL, 0);
	}
	return 0;
}

static int fakeTapInput(lua_State *L) {
	const char* inputdevice = luaL_checkstring(L, 1);
	int x = luaL_checkint(L, 2);
	int y = luaL_checkint(L, 3);
	int i;
	int inputfd = -1;
	struct input_event ev;
	inputfd = open(inputdevice, O_WRONLY | O_NDELAY);
	if (inputfd == NULL) {
		return luaL_error(L, "cannot open input device <%s>", inputdevice);
	}
	if(inputfd != -1) {
		gettimeofday(&ev.time, NULL);
		ev.type = 3;
		ev.code = 57;
		ev.value = 0;
		write(inputfd, &ev, sizeof(ev));
		gettimeofday(&ev.time, NULL);
		ev.type = 3;
		ev.code = 53;
		ev.value = x;
		write(inputfd, &ev, sizeof(ev));
		gettimeofday(&ev.time, NULL);
		ev.type = 3;
		ev.code = 54;
		ev.value = y;
		write(inputfd, &ev, sizeof(ev));
		gettimeofday(&ev.time, NULL);
		ev.type = 1;
		ev.code = 330;
		ev.value = 1;
		write(inputfd, &ev, sizeof(ev));
		gettimeofday(&ev.time, NULL);
		ev.type = 1;
		ev.code = 325;
		ev.value = 1;
		write(inputfd, &ev, sizeof(ev));
		gettimeofday(&ev.time, NULL);
		ev.type = 0;
		ev.code = 0;
		ev.value = 0;
		write(inputfd, &ev, sizeof(ev));
		gettimeofday(&ev.time, NULL);
		ev.type = 3;
		ev.code = 57;
		ev.value = -1;
		write(inputfd, &ev, sizeof(ev));
		gettimeofday(&ev.time, NULL);
		ev.type = 1;
		ev.code = 330;
		ev.value = 0;
		write(inputfd, &ev, sizeof(ev));
		gettimeofday(&ev.time, NULL);
		ev.type = 1;
		ev.code = 325;
		ev.value = 0;
		write(inputfd, &ev, sizeof(ev));
		gettimeofday(&ev.time, NULL);
		ev.type = 0;
		ev.code = 0;
		ev.value = 0;
		write(inputfd, &ev, sizeof(ev));
	}
	ioctl(inputfd, EVIOCGRAB, 0);
	close(inputfd);
	return 0;
}

static inline void set_event_table(lua_State *L, struct input_event input) {
	lua_newtable(L);
	lua_pushstring(L, "type");
	lua_pushinteger(L, (int) input.type);
	lua_settable(L, -3);
	lua_pushstring(L, "code");
	lua_pushinteger(L, (int) input.code);
	lua_settable(L, -3);
	lua_pushstring(L, "value");
	lua_pushinteger(L, (int) input.value);
	lua_settable(L, -3);

	lua_pushstring(L, "time");
	lua_newtable(L);
	lua_pushstring(L, "sec");
	lua_pushinteger(L, (int) input.time.tv_sec);
	lua_settable(L, -3);
	lua_pushstring(L, "usec");
	lua_pushinteger(L, (int) input.time.tv_usec);
	lua_settable(L, -3);
	lua_settable(L, -3);
}

static int waitForInput(lua_State *L) {
	struct input_event input;
	int n;
	int usecs = luaL_optint(L, 1, -1); // we check for <0 later
	fd_set fds;
	struct timeval timeout;
	int i, num, nfds;

	timeout.tv_sec = (usecs/1000000);
	timeout.tv_usec = (usecs%1000000);

	nfds = 0;

	FD_ZERO(&fds);
	for(i=0; i<NUM_FDS; i++) {
		if(inputfds[i] != -1)
			FD_SET(inputfds[i], &fds);
		if(inputfds[i] + 1 > nfds)
			nfds = inputfds[i] + 1;
	}

	/* when no value is given as argument, we pass
	 * NULL to select() for the timeout value, setting no
	 * timeout at all.
	 */
	num = select(nfds, &fds, NULL, NULL, (usecs < 0) ? NULL : &timeout);
	if (num == 0) {
		return luaL_error(L, "Waiting for input failed: timeout\n");
	} else if (num < 0) {
		return luaL_error(L, "Waiting for input failed: %d\n", errno);
	}

	for(i=0; i<NUM_FDS; i++) {
		if(inputfds[i] != -1 && FD_ISSET(inputfds[i], &fds)) {
			n = read(inputfds[i], &input, sizeof(struct input_event));
			if(n == sizeof(struct input_event)) {
				set_event_table(L, input);
				return 1;
			}
		}
	}
	return 0;
}

static const struct luaL_Reg input_func[] = {
	{"open", openInputDevice},
	{"closeAll", closeInputDevices},
	{"waitForEvent", waitForInput},
	{"fakeTapInput", fakeTapInput},
	{NULL, NULL}
};

int luaopen_input(lua_State *L) {
	luaL_register(L, "input", input_func);

	return 1;
}
