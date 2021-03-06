#include <iostream>
#include <fstream>
#include <map>
#include <list>
#include <semaphore.h>
#include <sys/types.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ev.h>
//POSIX SIGNAL
#include <signal.h>
#include <sys/wait.h>

#define DEBUG
#define MAX_WORKER 4

ssize_t sock_fd_write(int sock, void *buf, ssize_t buflen, int fd);
ssize_t sock_fd_read(int sock, void *buf, ssize_t bufsize, int *fd);

int set_nonblock(int fd);

std::map<int, bool> workers;
std::list<int> ready_read_sockets;
std::map<int, int> workers_shutdown;

sem_t* locker;
char *host = 0, *port = 0, *dir = 0;

int safe_pop_front() {
    int fd;

    sem_wait(locker);

    if (ready_read_sockets.empty()){
        fd = -1;
    } else {
        fd = *ready_read_sockets.cbegin();
        ready_read_sockets.pop_front();
    }

    sem_post(locker);

    return fd;
}

void safe_push_back(int fd) {
    sem_wait(locker);
    ready_read_sockets.push_back(fd);
    sem_post(locker);
}

void extract_path_from_http_get_request(std::string& path, const char* buf, ssize_t len) {
    std::string request(buf, len);
    std::string s1(" ");
    std::string s2("?");

    std::size_t pos1 = 4;

    std::size_t pos2 = request.find(s2, 4);
    if (pos2 == std::string::npos){
        pos2 = request.find(s1, 4);
    }

    path = request.substr(4, pos2 - 4);
#ifdef DEBUG
        std::cout << "extract_path_from_http_get_request last stmbol = " <<  path[path.size()-1]  << std::endl;
#endif
    if(path[path.size()-1] == '/') {
        path = path + "index.html";
    }
#ifdef DEBUG
        std::cout << "test index.html extract_path_from_http_get_request " <<  path << std::endl;
#endif
}


void slave_send_to_worker(struct ev_loop *loop, struct ev_io *w, int revents) {
    int slave_socket = w->fd;

    ev_io_stop(loop, w);

#ifdef DEBUG
    std::cout << "slave_send_to_worker: got slave socket " << slave_socket << std::endl;
#endif

    // find a free worker and send slave socket to it
    for(auto it = workers.begin(); it != workers.end(); it++){
#ifdef DEBUG
            std::cout << "worker status: number " << (*it).first << "status " << (*it).second << std::endl;
#endif
        if ((*it).second)
        {
            // found free worker, it is busy from now
            (*it).second = false;

            char tmp[1];
            workers_shutdown[(*it).first] = slave_socket;
            sock_fd_write((*it).first, tmp, sizeof(tmp), slave_socket);
	     	
#ifdef DEBUG
            std::cout << "slave_send_to_worker: sent slave socket " << slave_socket << " to worker" << std::endl;
#endif
            return;
        }
    }

#ifdef DEBUG
    std::cout << "slave_send_to_worker: no free workers, so call safe_push_back(" << slave_socket << ")" << std::endl;
#endif

    // add to queue for later processing
    safe_push_back(slave_socket);
}

void process_slave_socket(int slave_socket) {
    // recv from slave socket
    char buf[64535], reply[64535];
    ssize_t recv_ret = recv(slave_socket, buf, sizeof(buf), MSG_NOSIGNAL);
    
    if (recv_ret == -1){
#ifdef DEBUG
        std::cout << "do_work: recv return -1" << std::endl;
#endif
        return;
    }

    if(recv_ret == 0) {
#ifdef DEBUG    
         std::cout << "do_work: recv return 0" << std::endl;
#endif         
         return;
    }

#ifdef DEBUG
    std::cout << "do_work: recv return " << recv_ret << std::endl;
    std::cout << "======== received message ========" << std::endl;
    std::cout << buf << std::endl;
    std::cout << "==================================" << std::endl;
#endif

    // process http request, extract file path
    std::string path;
    extract_path_from_http_get_request(path, buf, recv_ret);

    // if path exists open and read file
    std::string full_path = std::string(dir) + path;

#ifdef DEBUG
    std::cout << "============ full path ===========" << std::endl;
    std::cout << full_path << std::endl;
    std::cout << "==================================" << std::endl; 
#endif

    if (access(full_path.c_str(), F_OK) != -1){
        // file exists, get its size
        int fd = open(full_path.c_str(), O_RDONLY);
        int sz = lseek(fd, 0, SEEK_END);;

        sprintf(reply, "HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/html\r\n"
                       "Content-length: %d\r\n"
                       "Connection: close\r\n"
                       "\r\n", sz);

        ssize_t send_ret = send(slave_socket, reply, strlen(reply), MSG_NOSIGNAL);

#ifdef DEBUG
        std::cout << "do_work: send return " << send_ret << std::endl;
#endif

        off_t offset = 0;
        while (offset < sz){
            // think not the best solution
            offset = sendfile(slave_socket, fd, &offset, sz - offset);
        }

        close(fd);
    } else {
        strcpy(reply, "HTTP/1.1 404 Not Found\r\n"
                      "Content-Type: text/html\r\n"
                      "Content-length: 107\r\n"
                      "Connection: close\r\n"
                      "\r\n");

        ssize_t send_ret = send(slave_socket, reply, strlen(reply), MSG_NOSIGNAL);
#ifdef DEBUG
        std::cout << "do_work: send return " << send_ret << std::endl;
#endif
        strcpy(reply, "<html>\n<head>\n<title>Not Found</title>\n</head>\r\n");
        send_ret = send(slave_socket, reply, strlen(reply), MSG_NOSIGNAL);
#ifdef DEBUG
        std::cout << "do_work: send return " << send_ret << std::endl;
#endif
        strcpy(reply, "<body>\n<p>404 Request file not found.</p>\n</body>\n</html>\r\n");
        send_ret = send(slave_socket, reply, strlen(reply), MSG_NOSIGNAL);
#ifdef DEBUG
        std::cout << "do_work: send return " << send_ret << std::endl;
#endif
    }
}

void worker(struct ev_loop *loop, struct ev_io *w, int revents) {
#ifdef DEBUG
    std::cout << "worker: from socket " << w->fd << std::endl;
#endif

    // get appropriate slave socket and read from it
    int fd = w->fd;
    int slave_socket;
    char tmp[1];

#ifdef DEBUG
    for(auto it = workers.begin(); it != workers.end(); it++){
            std::cout << "in worker function worker status: number " << (*it).first << "status " << (*it).second << std::endl;
    }
    std::cout << "worker: fd " << fd << std::endl;
#endif

    ssize_t size = sock_fd_read(fd, tmp, sizeof(tmp), &slave_socket);
    if (slave_socket == -1){
        std::cout << "worker: slave_socket == -1" << std::endl;
        exit(4);
    }

#ifdef DEBUG
    std::cout << "worker: got slave socket " << slave_socket << " msg size " << size << std::endl;
#endif

    // do it
    process_slave_socket(slave_socket);

    // write back to paired socket to update worker status
    // 18.07.2016 debug
    sock_fd_write(w->fd, tmp, sizeof(tmp), slave_socket);
#ifdef DEBUG
    std::cout << "worker: sent slave socket " << slave_socket << std::endl;
#endif
    if(slave_socket > 0) {
    	shutdown(slave_socket, SHUT_RDWR);
        close(slave_socket);
    }
	
}


void set_worker_free(struct ev_loop *loop, struct ev_io *w, int revents){
    // get socket of the pair
    int fd = w->fd;

    char tmp[1];
    int slave_socket;
    bool shutdownflag = false;

    ssize_t size = sock_fd_read(fd, tmp, sizeof(tmp), &slave_socket);
#ifdef DEBUG
    std::cout << "set_worker_free: fd " << fd << " got slave socket " << slave_socket << " msg size " << size << std::endl;
#endif

    // here we can restore watcher for the slave socket

    // complete all the work from the queue
    int slave_socket_original_1 = slave_socket;
    int slave_socket_original_2 = workers_shutdown[fd];
    while ((slave_socket = safe_pop_front()) != -1) {
        process_slave_socket(slave_socket);
#ifdef DEBUG    
        std::cout << "shutdown [1] : socket " << slave_socket << " is free now" << std::endl;
#endif
        shutdownflag = true;
	shutdown(slave_socket, SHUT_RDWR);
    	close(slave_socket);
    } 

    if(shutdownflag == false) {
#ifdef DEBUG    
       std::cout << "shutdown [2] : socket 1 " << slave_socket_original_1 <<" socket 2 "<< slave_socket_original_2 << " is close now" << std::endl;
#endif
       if(slave_socket_original_1 > 0) {
		shutdown(slave_socket_original_1, SHUT_RDWR);
                close(slave_socket_original_1);
       }
       if(slave_socket_original_2 > 0) {	
       		shutdown(slave_socket_original_2, SHUT_RDWR);
       		close(slave_socket_original_2);
	}
    }
	
    workers[fd] = true;
#ifdef DEBUG    
    std::cout << "set_worker_free: worker associated with paired socket " << fd << " is free now" << std::endl;
#endif
}

pid_t create_worker(){
    int sp[2];
    if (socketpair(AF_LOCAL, SOCK_STREAM, 0, sp) == -1){
        printf("socketpair error, %s\n", strerror(errno));
        exit(1);
    }

   
    // get default loop
    struct ev_loop* loop = EV_DEFAULT;

    auto pid = fork();

    if (pid){
        // parent, use socket 0
        close(sp[1]);

        // save worker socket and set free status
        workers.insert(std::pair<int, bool>(sp[0], true));

        // to detect the worker finished work with a socket
        //18.07.2016
        struct ev_io* half_watcher = new ev_io;
        ev_init(half_watcher, set_worker_free);
        ev_io_set(half_watcher, sp[0], EV_READ);
        ev_io_start(loop, half_watcher);

#ifdef DEBUG
        std::cout << "Worker with pid " << std::to_string(pid)
                  << " associated with socket " << std::to_string(sp[0])
                  << " in parent" << std::endl;
#endif
    } else {
        // child, use socket 1
        close(sp[0]);

        // we use EVFLAG_FORKCHECK instead of
        // ev_default_fork();

        // create watcher for paired socket
        struct ev_io worker_watcher;
        ev_init(&worker_watcher, worker);
        ev_io_set(&worker_watcher, sp[1], EV_READ);
        ev_io_start(loop, &worker_watcher);

#ifdef DEBUG
        std::cout << "Worker associated with socket " << std::to_string(sp[1]) << " in child" << std::endl;
#endif

        // wait for events, run loop in a child
        ev_loop(loop, 0);
    }

    return pid;
}

void master_accept_connection(struct ev_loop *loop, struct ev_io *w, int revents) {
    // create slave socket
    int slave_socket = accept(w->fd, 0, 0);

    if (slave_socket == -1) {
        printf("accept error, %s\n", strerror(errno));
        exit(3);
    }

#ifdef DEBUG
        std::cout << "master_accept_connection [1]: slave socket is " << slave_socket << std::endl;
#endif

    set_nonblock(slave_socket);

    // create watcher for a slave socket
    struct ev_io* slave_watcher = new ev_io;
    ev_init(slave_watcher, slave_send_to_worker);
    ev_io_set(slave_watcher, slave_socket, EV_READ);
    ev_io_start(loop, slave_watcher);
#ifdef DEBUG
    	std::cout << "master_accept_connection [2]: slave socket is " << slave_socket << std::endl;
#endif
}


ssize_t sock_fd_write(int sock, void *buf, ssize_t buflen, int fd) {
    ssize_t     size;
    struct msghdr   msg;
    struct iovec    iov;
    union {
        struct cmsghdr  cmsghdr;
        char        control[CMSG_SPACE(sizeof (int))];
    } cmsgu;
    struct cmsghdr  *cmsg;

    iov.iov_base = buf;
    iov.iov_len = buflen;

    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (fd != -1) {
        msg.msg_control = cmsgu.control;
        msg.msg_controllen = sizeof(cmsgu.control);

        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_len = CMSG_LEN(sizeof (int));
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;

#ifdef DEBUG
        std::cout << "passing fd " << fd << std::endl;
#endif
        *((int *) CMSG_DATA(cmsg)) = fd;
    } else {
        msg.msg_control = NULL;
        msg.msg_controllen = 0;
#ifdef DEBUG
        std::cout << "not passing fd " << fd << std::endl;
#endif
    }

    size = sendmsg(sock, &msg, 0);

    if (size < 0)
        perror ("sendmsg");
    return size;
}

ssize_t sock_fd_read(int sock, void *buf, ssize_t bufsize, int *fd) {
    ssize_t     size;

    if (fd) {
        struct msghdr   msg;
        struct iovec    iov;
        union {
            struct cmsghdr  cmsghdr;
            char        control[CMSG_SPACE(sizeof (int))];
        } cmsgu;
        struct cmsghdr  *cmsg;

        iov.iov_base = buf;
        iov.iov_len = bufsize;

        msg.msg_name = NULL;
        msg.msg_namelen = 0;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsgu.control;
        msg.msg_controllen = sizeof(cmsgu.control);
        size = recvmsg (sock, &msg, 0);
        if (size < 0) {
            perror ("recvmsg");
            exit(1);
        }
        cmsg = CMSG_FIRSTHDR(&msg);
        if (cmsg && cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
            if (cmsg->cmsg_level != SOL_SOCKET) {
                fprintf (stderr, "invalid cmsg_level %d\n",
                     cmsg->cmsg_level);
                exit(1);
            }
            if (cmsg->cmsg_type != SCM_RIGHTS) {
                fprintf (stderr, "invalid cmsg_type %d\n",
                     cmsg->cmsg_type);
                exit(1);
            }

            *fd = *((int *) CMSG_DATA(cmsg));
#ifdef DEBUG
            std::cout << "received fd " << *fd << std::endl;
#endif
        } else
            *fd = -1;
    } else {
        size = read (sock, buf, bufsize);
        if (size < 0) {
            perror("read");
            exit(1);
        }
    }
    return size;
}

int set_nonblock(int fd)
{
    int flags;
#if defined(O_NONBLOCK)
    if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
        flags = 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
    flags = 1;
    return ioctl(fd, FIONBIO, &flags);
#endif
}


static void hdl_sighld_parent(int sig, siginfo_t *siginfo, void *context) {
        int exit_code;
        int saved_errno = errno;
        while (waitpid((pid_t)(-1), &exit_code, WNOHANG) > 0) {
        }
        errno = saved_errno;
}


int main(int argc, char* argv[]) {
    // we want to be a daemon
    // Allocate semaphore and initialize it as shared
    locker = new sem_t;
    sem_init(locker, 1, 1);
    struct sigaction act_sighld_parent;
    int opt;

    while ((opt = getopt(argc, argv, "h:p:d:")) != -1)
    {
        switch(opt)
        {
            case 'h':
                host = optarg;
                break;
            case 'p':
                port = optarg;
                break;
            case 'd':
                dir = optarg;
                break;
            default:
                printf("Usage: %s -h <host> -p <port> -d <folder>\n", argv[0]);
                exit(1);
        }
    }

    if (host == NULL || port == NULL || dir == NULL) {
        printf("Usage: %s -h <host> -p <port> -d <folder>\n", argv[0]);
        exit(1);
    }

/* The SA_SIGINFO flag tells sigaction() to use the sa_sigaction field, not sa_handler. */
    act_sighld_parent.sa_flags = SA_RESTART | SA_NOCLDSTOP | SA_SIGINFO;
    
   if (sigaction(SIGCHLD, &act_sighld_parent, NULL) < 0) {
   	perror ("sigaction SIGCHLD");
        return 1;
   }

    if (daemon(0, 0) == -1) {
            std::cout << "daemon error" << std::endl;
            exit(1);
    }

#ifdef DEBUG
    std::ofstream out("/tmp/final.log");
    std::streambuf *coutbuf = std::cout.rdbuf(); //save old buf
    std::cout.rdbuf(out.rdbuf()); //redirect std::cout to out.txt!
    std::cout << "main begin, parent pid is " << getpid() << std::endl;
#endif

    // Our event loop
    struct ev_loop *loop = ev_default_loop(EVFLAG_FORKCHECK);

    if (create_worker() == 0) {
         std::cout << "Worker 1 is about to return" << std::endl;
         return 0;
    }

    if (create_worker() == 0) {
        std::cout << "Worker 2 is about to return" << std::endl;
        return 0;
    }

    if (create_worker() == 0) {
        std::cout << "Worker 3 is about to return" << std::endl;
        return 0;
    }


    // Master socket, think non-blocking
    int master_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (master_socket == -1) {
        perror("socket erro");
        return 1;
    }

    set_nonblock(master_socket);

    char * pEnd;
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(strtol(port,&pEnd,10));

    if (inet_pton(AF_INET, host, &(addr.sin_addr.s_addr)) != 1) {
        perror("inet_aton error");
        return 2;
    }

    int optval = 1;
    if (setsockopt(master_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
        perror("Setsockopt(SO_REUSEADDR) error: ");
        return EXIT_FAILURE;
    }

    if (bind(master_socket, (struct sockaddr* )&addr, sizeof(addr)) == -1) {
        printf("bind return -1, %s\n", strerror(errno));
        return 3;
    }


    listen(master_socket, SOMAXCONN);

#ifdef DEBUG
    std::cout << "master socket is " << master_socket << std::endl;
#endif

    // Master watcher
    struct ev_io master_watcher;
    ev_init (&master_watcher, master_accept_connection);
    ev_io_set(&master_watcher, master_socket, EV_READ);
    ev_io_start(loop, &master_watcher);

    // Start loop
    ev_loop(loop, 0);

    close(master_socket);

    return 0;
}

