/*
*   Calvin Neo
*   Copyright (C) 2017  Calvin Neo <calvinneo@calvinneo.com>
*   https://github.com/CalvinNeo/ATP
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 2 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License along
*   with this program; if not, write to the Free Software Foundation, Inc.,
*   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#pragma once

#include <sys/types.h> // ssize_t
#include <sys/socket.h>
#include <netinet/in.h> // sockaddr
#include <arpa/inet.h> // inet_ functions
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include "error.h"
#include <stdint.h>

#include <sys/sysctl.h>
#include <poll.h>
#include "atp_common.h"

#define SA struct sockaddr

typedef void sigfunc_t(int);

sigfunc_t * setup_signal(int signo, sigfunc_t * func);

int make_socket(int family, int type, int protocol, int port, const char * ipaddr_str);
struct sockaddr_in make_socketaddr_in(int family, const char * ipaddr_str, int port);

ATP_PROC_RESULT normal_sendto(atp_callback_arguments * args);

inline void activate_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) err_sys("fcntl");
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) err_sys("fcntl");
}