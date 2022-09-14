#include <iostream>
#include <netinet/in.h>
#include <bits/socket.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <bits/signum.h>
#include <cstdlib>
#include <cstddef>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unordered_map>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include "time_wheel.h"

using namespace std;

#define MAX_TASK_NUM 128  //平均每个工作线程最多能同时处理的连接数
#define MIN_TASK_NUM 8   //平均每个工作线程最少能同时处理的连接数
#define MAX_THREAD 100  //系统中允许存在的最大工作线程数
#define INIT_THREAD 1  //初始化线程个数
#define BUF_SIZE 512  //线程读缓冲区的大小,单位字节

int KEEP_LIVE = 180; //连接保活时间(s)
int now_thread = 0; //系统中的线程数 仅主线程修改，不必加锁
int now_task = 0;  //系统中的连接数

unordered_map<pthread_t, int> thread_task_map; //<子线程号、任务数>  哈希表中的元素仅主线程线程修改，不必加锁
unordered_map<pthread_t, int> thread_pipe_map; //g++ - std = c++11 <子线程号、管道文件描述符写端>

unordered_map<int, timer*> cfd_time_map; //<cfd、定时器指针>
int sig_timer_pipe[2];  //信号处理函数与主线程通信的管道，通知主线程定时时间到期

int fd_close_pipe[2];  //工作线程通知主线程有连接关闭或者刷新fd定时器的管道
pthread_mutex_t fd_close_pipe_lock;  //保护fd_close_pipe[1]
struct info { //工作线程向主线程发送消息的格式
    pthread_t thread_id;  //线程id
    int fd;  //线程管道读端
    int cfd; //cfd
    int flags;  //0:仅将连接关闭, 1:此线程退出, 2:刷新cfd的定时器
};

struct to_thread_info { //主线程向工作线程通知新连接或者有连接超时的格式
    int cfd; //标识客户端
    int flags; //0:表示新连接到来，1:cfd超时
};

void setnblock(int fd);  //设置fd为非阻塞

void setblock(int fd);  //设置fd为阻塞

void addsig(int sig);  //注册信号处理函数

void sig_handler(int sig); //信号处理函数

unordered_map<pthread_t, int>::iterator get_min_task();  //寻找任务数最小的元素

void add_pthread(pthread_t& thread_id, int* fd, int task_num);  //向相关数据结构中注册线程

void init_thread();  //创建初始化线程

void time_func(work_thread_info*); //定时器到期时的回调函数

void close_cfd(int fd, pthread_t thread_num, int& task_num, char* buf_read, int pipe_fd); //关闭cfd

void* worker(void* pipe_read);  //工作线程

int read_line(char* buf_read, int fd, int size);  //从相应的fd中读取一行

void discard_line(int fd);  //将处理过的请求的后续部分丢弃

int handle(char* buf_read, int fd);  //处理请求

void change_path(); //改变当前工作路径

void str_convert(char* left, char* right);  //中文转码

int hexit(char c); //字符转16进制

void send_head_http(int fd, int flags, int length);  //发送http的头部

void test_dir(char *str); //测试目录名是否规范，将不规范的目录名修改正确 ./dirname->./dirname/

int main(int argc, char* argv[])
{
    if (argc < 2) {
        cout << "请输入正确的端口号，例如：" << argv[0] << ' ' << 10000 << endl;
        exit(1);
    }

    //构造服务器端地址，ip地址为0，监听整机全部的网卡，端口号在运行时传入
    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    int port = atoi(argv[1]);
    server_address.sin_port = htons(port);
    server_address.sin_addr.s_addr = INADDR_ANY;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0); //创建套接字
    if (listen_fd == -1) {
        cout << "主线程socket()失败" << endl;
        perror("error"); exit(1);
    }
    cout << "主线程socket()成功" << endl;

    //绑定socket地址
    int bind_ret = bind(listen_fd, (sockaddr*)&server_address, sizeof(server_address));
    if (bind_ret == -1) {
        cout << "主线程bind()失败" << endl;
        if (errno == EACCES) cout << "请使用非知名端口号" << endl;
        if (errno == EADDRINUSE) cout << "被绑定的地址处在TIME_WAIT状态" << endl;
        perror("error"); exit(1);
    }
    cout << "主线程bind()成功" << endl;

    int listen_ret = listen(listen_fd, 5); //监听listen_fd
    if (listen_ret == -1) {
        cout << "主线程listen()失败" << endl;
        perror("error"); exit(1);
    }
    cout << "主线程listen()成功" << endl;

    int epoll_ret = epoll_create(1); //创建epoll监听表
    if (epoll_ret == -1) {
        cout << "主线程epoll_create()失败" << endl;
        perror("error"); exit(1);
    }
    cout << "主线程epoll_create()成功" << endl;

    struct epoll_event event;  //将listen_fd添加到epoll内核表中，一定使用LT模式
    event.events = EPOLLIN;
    event.data.fd = listen_fd;
    int epoll_ctl_ret = epoll_ctl(epoll_ret, EPOLL_CTL_ADD, listen_fd, &event);
    if (epoll_ctl_ret == -1) {
        cout << "主线程对listen_fd epoll_ctl()失败" << endl;
        perror("error"); exit(1);
    }
    cout << "主线程对listen_fd epoll_ctl()成功" << endl;

    int pipe_ret = pipe(fd_close_pipe);  //创建主线程与子线程通信连接关闭的管道
    if (pipe_ret == -1) {
        cout << "主线程对fd_close_pipe pipe()失败！" << endl;
        perror("error"); exit(1);
    }
    setnblock(fd_close_pipe[0]); setnblock(fd_close_pipe[1]); //设置管道为非阻塞

    event.events = EPOLLIN | EPOLLET; //将fd_close_pipe[0]添加到epoll内核表中,使用ET模式
    event.data.fd = fd_close_pipe[0];
    int epoll_ctl_close = epoll_ctl(epoll_ret, EPOLL_CTL_ADD, fd_close_pipe[0], &event);
    if (epoll_ctl_close == -1) {
        cout << "主线程对fd_close_pipe epoll_ctl()失败！" << endl;
        perror("error"); exit(1);
    }
    cout << "主线程对fd_close_pipe[0] epoll_ctl()成功" << endl;

    int sig_timer_pipe_ret = pipe(sig_timer_pipe);  //创建信号处理函数与主线程通信的管道
    if (sig_timer_pipe_ret == -1) {
        cout << "主线程对sig_timer_pipe pipe()失败！" << endl;
        perror("error"); exit(1);
    }
    setnblock(sig_timer_pipe[0]); setnblock(sig_timer_pipe[1]); //设置管道为非阻塞

    event.events = EPOLLIN; //将sig_timer_pipe[0]添加到epoll内核表中,使用LT模式
    event.data.fd = sig_timer_pipe[0];
    int epoll_ctl_timer = epoll_ctl(epoll_ret, EPOLL_CTL_ADD, sig_timer_pipe[0], &event);
    if (epoll_ctl_timer == -1) {
        cout << "主线程对sig_timer_pipe[0] epoll_ctl()失败！" << endl;
        perror("error"); exit(1);
    }
    cout << "主线程对sig_timer_pipe[0] epoll_ctl()成功" << endl;

    fd_close_pipe_lock = PTHREAD_MUTEX_INITIALIZER; //初始化保护fd_close_pipe[1]的互斥锁
    init_thread(); //创建初始化线程
    change_path(); //改变当前工作目录为资源目录
    class timer_wheel* pwheel = new timer_wheel();  //创建时间轮
    cout << "主线程时间轮创建成功" << endl;

    addsig(SIGALRM);  //注册sigalrm信号
    signal(SIGPIPE, SIG_IGN);  //忽略sigpipe信号，防止socket客户端读端关闭导致服务器被杀
    alarm(1);  //每一秒检查一次时间轮，查看是否有连接超时

    cout << "主线程epoll_wait()中" << endl;
    while (1) {  
        //循环监听listen_fd/fd_close_pipe/sig_timer_pipe, 接受连接,并将提取出的文件描述符派发给负载最小的线程
        struct epoll_event ret_event[3];
        int epoll_wait_ret = epoll_wait(epoll_ret, ret_event, 3, -1); //等待新的连接到来
        if (epoll_wait_ret == -1) {
            if (errno == EINTR) continue; //信号中断epoll_wait()
            else {
                cout << "主线程epoll_wait()失败" << endl;
                perror("error"); exit(1);
            }
        }

        for (int i = 0; i < epoll_wait_ret; ++i) {

            int fd = ret_event[i].data.fd;
            if (fd == listen_fd) { //有新连接到达,提取连接，将连接派发给负载最小的线程
                
                struct sockaddr_in client_address; socklen_t client_addr_length = sizeof(client_address);
                int accept_ret_fd = accept(listen_fd, (struct sockaddr*)&client_address, &client_addr_length);
                if (accept_ret_fd == -1) {
                    cout << "主线程accept()失败" << endl;
                    perror("error"); exit(1);
                }
                cout << "有新的连接到来,客户端的地址是：" << "ip: " << inet_ntoa(client_address.sin_addr)
                    << " port:" << ntohs(client_address.sin_port) << endl;

                if (now_task > (MAX_THREAD * MAX_TASK_NUM)) {  //系统现有连接过多
                    cout << "now_task > MAX_TASK!" << endl;
                    int write_ret = write(accept_ret_fd, "now_task > MAX_TASK!", sizeof("now_task > MAX_TASK!"));
                    if (write_ret == -1) {
                        cout << "主线程write()失败" << endl;
                        perror("error"); exit(1);
                    }
                    close(accept_ret_fd);
                    continue;
                }

                setnblock(accept_ret_fd); //设置新到的连接为非阻塞
                ++now_task; //系统现有连接数加一

                pthread_t thread_id = 0;  //记录线程号
                struct to_thread_info hand_info;
                hand_info.cfd = accept_ret_fd; hand_info.flags = 0;

                if (now_task <= (now_thread * MAX_TASK_NUM)) {  //有空闲线程
                    auto it = get_min_task(); thread_id = it->first; ++(it->second);
                    int write_ret = write(thread_pipe_map[thread_id], &hand_info, sizeof(hand_info));
                    if (write_ret == -1) {
                        cout << "主线程向管道write()失败" << endl;
                        perror("error"); exit(1);
                    }
                }

                else {  //无空闲线程
                    int fd[2]; int pipe_ret = pipe(fd);  //创建主线程与子线程通信的管道
                    if (pipe_ret == -1) {
                        cout << "主线程pipe()失败" << endl;
                        perror("error"); exit(1);
                    }
                    setnblock(fd[0]); setnblock(fd[1]); //设置管道为非阻塞

                    int thread_ret = pthread_create(&thread_id, nullptr, worker, (void*)(long int)fd[0]);
                    if (thread_ret != 0) {
                        cout << "新创建线程pthread_create()失败" << endl;
                        perror("error"); exit(1);
                    }

                    if (pthread_detach(thread_id)) {  //分离线程 g++ -lpthread,linux中没有线程库
                        cout << "新创建线程pthread_detach()失败" << endl;
                        perror("error"); exit(1);
                    }

                    add_pthread(thread_id, fd, 1); //向相关数据结构中注册该线程

                    int write_ret = write(fd[1], &hand_info, sizeof(hand_info)); //通知工作线程有新连接
                    if (write_ret == -1) {
                        cout << "主线程向管道write()失败" << endl;
                        perror("error"); exit(1);
                    }

                    cout << "工作线程不足,新线程创建完成,线程号是" << thread_id << endl;
                }

                //创建该连接的定时器并且加入到时间轮中
                timer *new_timer = 
                pwheel->add_timer(KEEP_LIVE, thread_id, accept_ret_fd, time_func); //创建定时器
                pwheel->insert(new_timer);  //加入到时间轮中

                cfd_time_map.emplace(accept_ret_fd, new_timer); //向cfd_timer_map中注册该定时器
 
                cout << "主线程向工作线程派发连接完成,现有线程 连接数 new_fd：" 
                     << now_thread << ' ' << now_task << ' ' << accept_ret_fd << endl;
            }
            
            else if (fd == sig_timer_pipe[0]) {  //定时到期
                int sig = 0;
                int count = read(fd, &sig, sizeof(int));
                if (count == -1) {
                    cout << "主线程对sig_timer_pipe[0] read()失败" << endl;
                    perror("error"); exit(1);              
                }

                pwheel->tick(); //检查时间轮上是否有定时器到期
            }

            else {  //工作线程有连接关闭或者刷新定时器
                while (1) {

                    struct info fd_close_info;
                    int bytes = read(fd, &fd_close_info, sizeof(fd_close_info));
                    if (bytes == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        else {
                            cout << "主线程对fd_close_pipe[0] read()失败" << endl;
                            perror("error"); exit(1);
                        }
                    }

                    pthread_t thread_id = fd_close_info.thread_id; //获取退出端信息
                    int pipe_read_fd = fd_close_info.fd; int flags = fd_close_info.flags;
                    int cfd = fd_close_info.cfd;

                    if (flags == 0) { //flags == 0 仅连接关闭
                        --now_task; --thread_task_map[thread_id];

                        timer* p = cfd_time_map[cfd];
                        pwheel->del_timer(p); delete p;  //从时间轮上删除定时器p
                        cfd_time_map.erase(cfd);

                        cout << "主线程已收到通知,有连接关闭,现有线程 连接数 退出fd: "
                             << now_thread << ' ' << now_task << ' ' << cfd << endl;
                    }

                    else if (flags == 1) { //flags == 1连接关闭的同时线程退出
                        --now_task; --now_thread;

                        //关闭主线程与工作线程间的通信管道
                        close(pipe_read_fd); close(thread_pipe_map[thread_id]);
                        thread_pipe_map.erase(thread_id); thread_task_map.erase(thread_id); 

                        timer* p = cfd_time_map[cfd];
                        pwheel->del_timer(p); delete p;  //从时间轮上删除定时器p
                        cfd_time_map.erase(cfd);

                        cout << "主线程已收到通知,有线程退出,现有线程 连接数 退出线程id 退出fd："
                             << now_thread << ' ' << now_task << ' ' << thread_id << ' ' << cfd << endl;
                    }

                    else {  //flags == 2重置cfd上的定时时间
                        cout << "主线程已收到通知,刷新定时器 fd=" << cfd << endl;
                        timer* p = cfd_time_map[cfd];  //找出cfd对应的定时器
                        pwheel->update(p); //刷新cfd对应的定时器
                    }
                }
            }
        }
    }

    close(listen_fd);
    close(epoll_ret);
    close(fd_close_pipe[0]);
    close(fd_close_pipe[1]);
    close(sig_timer_pipe[0]);
    close(sig_timer_pipe[1]);
    delete pwheel;

    return 0;
}

void setnblock(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
}

void setblock(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option & ~O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
}

void addsig(int sig) {  //注册信号处理函数
    struct sigaction sa;
    sa.sa_handler = sig_handler;
    if (sigaction(sig, &sa, nullptr) == -1) {
        cout << "主线程sigaction()失败" << endl;
        perror("error"); exit(-1);
    }
    cout << "主线程注册信号处理函数成功" << endl;
}

void sig_handler(int sig) { //信号处理函数
    int write_ret = write(sig_timer_pipe[1], &sig, sizeof(int));
    if (write_ret == -1) {
        cout << "信号处理函数向sig_timer_pipe[1] write()失败" << endl;
        perror("error"); exit(1);
    }
    alarm(1);
}

unordered_map<pthread_t, int>::iterator get_min_task() {  //寻找任务数最小的元素
    auto it_task = thread_task_map.begin();
    for (auto it = it_task; it != thread_task_map.end(); ++it) {
        if (it->second < it_task->second) it_task = it;
    }
    return it_task;
}

void add_pthread(pthread_t& thread_id, int* fd, int task_num) {  //向相关数据结构中注册线程

    thread_pipe_map.emplace(thread_id, fd[1]);     //写端

    thread_task_map.emplace(thread_id, task_num);  //将线程与任务数添加到map中

    ++now_thread;  //系统当前线程数加一
}

void init_thread() {  //创建初始化线程
    for (int i = 0; i < INIT_THREAD; ++i) {   //创建初始线程
        int fd[2]; int pipe_ret = pipe(fd);  //创建主线程与子线程通信的管道
        if (pipe_ret == -1) {
            cout << "主线程pipe()失败" << endl;
            perror("error"); exit(1);
        }
        setnblock(fd[0]); setnblock(fd[1]); //设置管道为非阻塞

        pthread_t thread_id = 0;  //创建线程
        int thread_ret = pthread_create(&thread_id, nullptr, worker, (void*)(long int)fd[0]);
        if (thread_ret != 0) {
            cout << "创建初始化线程失败" << endl;
            perror("error："); exit(1);
        }

        if (pthread_detach(thread_id)) {  //分离线程 g++ -lpthread,linux中没有线程库
            cout << "分离初始化线程失败" << endl;
            perror("error"); exit(1);
        }

        add_pthread(thread_id, fd, 0); //向相关数据结构中注册该线程

        cout << thread_id << "号初始化线程创建完成" << endl;
    }
}

void time_func(work_thread_info* cp) {  //定时器到期时的回调函数
    struct to_thread_info info;
    info.cfd = cp->cfd; info.flags = 1;  //向工作线程通知有连接超时

    int write_ret = write(thread_pipe_map[cp->thread_id], &info, sizeof(info));
    if (write_ret == -1) {
        cout << "主线程向管道write()失败" << endl;
        perror("error"); exit(1);
    }
}

void* worker(void* pipe_read) {

    pthread_t thread_num = pthread_self(); //获取线程号

    int epoll_ret = epoll_create(MAX_TASK_NUM + 1); //创建epoll监听表
    if (epoll_ret == -1) {
        cout << "工作线程epoll_create()失败" << endl;
        perror("error"); exit(1);
    }
    cout << thread_num << "号工作线程epoll_create()成功" << endl;

    int pipe_read_fd = (int)(long int)pipe_read;
    struct epoll_event event_pipe;  //将管道读端添加到epoll内核表中，使用LT模式
    event_pipe.events = EPOLLIN;
    event_pipe.data.fd = pipe_read_fd;
    int epoll_ctl_ret = epoll_ctl(epoll_ret, EPOLL_CTL_ADD, pipe_read_fd, &event_pipe);
    if (epoll_ctl_ret == -1) {
        cout << "工作线程对管道读端epoll_ctl()失败" << endl;
        perror("error"); exit(1);
    }
    cout << thread_num << "号工作线程对管道读端epoll_ctl()成功" << endl;

    int task_num = 0; //工作线程现在处理的任务数
    char* buf_read = new char[BUF_SIZE];  //线程读取请求行的缓冲区
    struct epoll_event events[MAX_TASK_NUM + 1];

    cout << thread_num << "号工作线程epoll_wait()中" << endl;
    while (1) {  //循环监听, 添加新的连接或者处理客户端请求或者关闭连接

        int epoll_wait_ret = epoll_wait(epoll_ret, events, MAX_TASK_NUM + 1, -1); //等待客户端请求
        if (epoll_wait_ret == -1) {
            if (errno == EINTR) continue;
            else {
                cout << "工作线程epoll_wait()失败" << endl;
                perror("error"); exit(1);
            }
        }

        for (int i = 0; i < epoll_wait_ret; ++i) {
 
            int fd = events[i].data.fd;
            if ((fd == pipe_read_fd) && (events[i].events & EPOLLIN)) {  //有新连接或者有连接超时

                struct to_thread_info new_info;
                int read_count = read(fd, &new_info, sizeof(new_info));
                if (read_count == -1) {
                    cout << "工作线程read()失败" << endl;
                    perror("error"); exit(1);
                }

                if (!new_info.flags) { //有新连接
                    ++task_num;

                    struct epoll_event event;  //将新任务添加到epoll内核表中，使用ET模式
                    event.events = EPOLLIN | EPOLLET;
                    event.data.fd = new_info.cfd;
                    int epoll_ctl_ret = epoll_ctl(epoll_ret, EPOLL_CTL_ADD, new_info.cfd, &event);
                    if (epoll_ctl_ret == -1) {
                        cout << "工作线程对新连接epoll_ctl()失败" << endl;
                        perror("error"); exit(1);
                    }

                    cout << thread_num << "号工作线程对新连接fd=" << new_info.cfd
                        << " epoll_ctl()成功, task_num:" << task_num << endl;
                }

                else {  //有连接超时
                    cout << thread_num << "号工作线程已收到有连接超时,即将关闭连接,fd=" << new_info.cfd << endl;
                    close_cfd(new_info.cfd, thread_num, task_num, buf_read, pipe_read_fd); //关闭cfd
                }
            }

            else {  //客户端请求

                while (1) { //循环处理客户端fd的请求

                    memset(buf_read, '\0', BUF_SIZE);
                    //将请求行读入buf_read中，其他行丢弃，返回实际读到的字节数
                    int read_count = read_line(buf_read, fd, BUF_SIZE);

                    if (read_count == -1) { //已经读到文件末尾
                        break;  //退出循环处理请求
                    }

                    else if (!read_count) { //对方关闭了连接

                        close_cfd(fd, thread_num, task_num, buf_read, pipe_read_fd); //关闭fd

                        break;
                    }

                    else { //处理读取出的请求

                        cout << thread_num << "号工作线程已读取到fd=" << fd << "的请求" << buf_read;

                        int handle_ret = handle(buf_read, fd);
                        if (handle_ret == -1) {
                            cout << "工作线程handle()失败" << endl;
                            perror("error"); exit(1);
                        }
                        discard_line(fd);  //丢弃掉处理过的请求的请求行之后的数据

                        struct info fd_close_info;   //向主线程通知刷新fd上的定时器
                        fd_close_info.thread_id = thread_num;
                        fd_close_info.fd = 0;
                        fd_close_info.cfd = fd;
                        fd_close_info.flags = 2;

                        if (pthread_mutex_lock(&fd_close_pipe_lock) == -1) { //对fd_close_pipe[1]加锁
                            cout << "工作线程pthread_mutex_lock()失败" << endl;
                            perror("error"); exit(1);
                        }
                        int write_ret = write(fd_close_pipe[1], &fd_close_info, sizeof(fd_close_info));
                        if (write_ret == -1) {
                            cout << "工作线程向fd_close_pipe[1] write()失败" << endl;
                            perror("error"); exit(1);
                        }
                        if (pthread_mutex_unlock(&fd_close_pipe_lock) == -1) { //对fd_close_pipe[1]解锁
                            cout << "工作线程pthread_mutex_unlock()失败" << endl;
                            perror("error"); exit(1);
                        }
                    }
                }
            }
        }
    }
    return nullptr;
}

void close_cfd(int fd, pthread_t thread_num, int& task_num, char* buf_read, int pipe_read_fd) {//工作线程关闭fd
    
    --task_num; close(fd);  //fd没有进行dup(fd)的话会自动从epoll中移除 

    struct info fd_close_info;
    fd_close_info.thread_id = thread_num;
    fd_close_info.fd = pipe_read_fd;
    fd_close_info.cfd = fd;
    fd_close_info.flags = 1;

    if (!task_num && now_task < (now_thread * MIN_TASK_NUM) && now_thread > INIT_THREAD) {
        //检测到此线程正在服务的任务数为0，并且系统中线程过多,此线程退出
        delete[] buf_read;

        if (pthread_mutex_lock(&fd_close_pipe_lock) == -1) { //对fd_close_pipe[1]加锁
            cout << "工作线程pthread_mutex_lock()失败" << endl;
            perror("error"); exit(1);
        }
        int write_ret = write(fd_close_pipe[1], &fd_close_info, sizeof(fd_close_info));//通知主进程该工作线程结束
        if (write_ret == -1) {
            cout << "工作线程向fd_close_pipe[1] write()失败" << endl;
            perror("error"); exit(1);
        }
        if (pthread_mutex_unlock(&fd_close_pipe_lock) == -1) { //对fd_close_pipe[1]解锁
            cout << "工作线程pthread_mutex_unlock()失败" << endl;
            perror("error"); exit(1);
        }

        cout << thread_num << "号工作线程退出,并且关闭了一个连接, fd=" << fd << endl;
        pthread_exit(nullptr);
    }

    else { //仅关闭连接
        fd_close_info.flags = 0;

        if (pthread_mutex_lock(&fd_close_pipe_lock) == -1) { //对fd_close_pipe[1]加锁
            cout << "工作线程pthread_mutex_lock()失败" << endl;
            perror("error"); exit(1);
        }
        int write_ret = write(fd_close_pipe[1], &fd_close_info, sizeof(fd_close_info));
        if (write_ret == -1) {
            cout << "工作线程向fd_close_pipe[1] write()失败" << endl;
            perror("error"); exit(1);
        }
        if (pthread_mutex_unlock(&fd_close_pipe_lock) == -1) { //对fd_close_pipe[1]解锁
            cout << "工作线程pthread_mutex_unlock()失败" << endl;
            perror("error"); exit(1);
        }

        cout << thread_num << "号工作线程关闭了一个连接,fd=" << fd << endl;
    }
}

int read_line(char* buf_read, int fd, int size) {  //读取一行到特定缓冲区中，返回实际读到的字符数，包括'\n'
    char temp_char; int count = 0;
    while (1) {
        int read_num = read(fd, &temp_char, 1);
        if (!read_num) return 0; //对方关闭连接
        else if (read_num == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -1; //读到文件末尾
            else {
                cout << "工作线程read_line()失败" << endl;
                perror("error"); exit(1);
            }
        }

        buf_read[count++] = temp_char;
        if (count > size) {
            cout << "工作线程read_line()失败, read_buf太小" << endl;
            perror("error"); exit(1);
        }
        if (temp_char == '\n') {
            return count;
        }
    }
}

void discard_line(int fd) {  //丢弃掉处理过的请求的请求行之后的数据
    char temp_buf[BUF_SIZE];
    while (1) {
        memset(temp_buf, '\0', BUF_SIZE);
        int read_count = read_line(temp_buf, fd, BUF_SIZE);
        if (read_count == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; //读到文件末尾
            else {
                cout << "工作线程discard_line()失败" << endl;
                perror("error"); exit(1);
            }
        }

        if (temp_buf[0] == '\r' && temp_buf[1] == '\n') break;
    }
}

int handle(char* buf_read, int fd) {
    char method[16] = ""; char path[256] = ""; char protocol[16] = "";
    //分别存储方法、路径(相对路径)、HTTP版本 GET /filename HTTP/1.0
    int sscanf_ret = sscanf(buf_read, "%[^ ] %[^ ] %[^ \r\n]", method, path, protocol);
    if (sscanf_ret < 3) {
        cout << "工作线程sscanf()失败" << endl;
        perror("error"); exit(1);
    }

    if (!strcasecmp(method, "GET")) {
        char* str = path + 1;

        str_convert(str, str); //中文转码

        if (*str == '\0') { //用户请求的/
            strcpy(str, "./");
        }

        cout << pthread_self() << "号工作线程对fd=" << fd 
            << "的请求中的文件名进行转码处理后为,文件名:[" << str << ']' << endl;

        struct stat my_stat;
        int stat_ret = stat(str, &my_stat);

        setblock(fd); //设置fd为阻塞

        if (stat_ret < 0) { //获取所请求文件信息 <0表示文件不存在
            cout << "工作线程判断fd=" << fd << "请求的文件不存在" << endl;

            int open_fd = open("error.html", O_RDONLY);  //打开错误html文件
            if (open_fd < 0) {
                cout << "工作线程open()失败" << endl;
                perror("error"); exit(1);
            }

            struct stat error_stat;
            if (stat("error.html", &error_stat) < 0) {  //获取错误文件的stat信息
                cout << "工作线程对error.html stat()失败" << endl;
                perror("error"); exit(1);
            }

            send_head_http(fd, 0, error_stat.st_size);  //发送http头部

            int send_file_ret = sendfile(fd, open_fd, nullptr, error_stat.st_size); //发送error.html文件
            if (send_file_ret == -1) {
                cout << "工作线程send_file()(文件不存在)失败" << endl;
                perror("error"); exit(1);
            }

            cout << "发送error.html文件成功" << endl;
            close(open_fd);  //关闭错误文件error.html
        }

        else { //文件存在
            if (S_ISREG(my_stat.st_mode)) { //请求文件是普通文件
                cout << "工作线程判断fd=" << fd << "请求的文件是普通文件" << endl;

                int open_fd = open(str, O_RDONLY);  //打开目标文件
                if (open_fd < 0) {
                    cout << "工作线程open()失败" << endl;
                    perror("error"); exit(1);
                }

                send_head_http(fd, 1, my_stat.st_size);  //发送http头部

                int send_file_ret = sendfile(fd, open_fd, nullptr, my_stat.st_size); //发送html文件
                if (send_file_ret == -1) {
                    cout << "工作线程send_file()(目标文件)失败" << endl;
                    perror("error"); exit(1);
                }

                cout << "工作线程向fd=" << fd << "发送目标文件成功" << endl;
                close(open_fd);  //关闭目标文件
            }

            if (S_ISDIR(my_stat.st_mode)) { //请求文件是目录
                cout << "工作线程判断fd=" << fd << "请求的文件是目录" << endl;

                test_dir(str);  //测试目录名是否规范，将不规范的目录名修改正确 ./dirname->./dirname/

                char buf_new[256]; memset(buf_new, '\0', 256);
                if (sprintf(buf_new, "%sdefault.html", str) <= 0) {
                    cout << "工作线程sprintf()失败" << endl;
                    perror("error"); exit(1);
                }

                int open_fd = open(buf_new, O_RDONLY);  //打开目录html文件
                if (open_fd < 0) {
                    cout << "工作线程open()失败" << endl;
                    perror("error"); exit(1);
                }

                struct stat dir_stat;
                if (stat(buf_new, &dir_stat) < 0) {  //获取错误文件的stat信息
                    cout << "工作线程对default.html stat()失败" << endl;
                    perror("error"); exit(1);
                }

                send_head_http(fd, 1, dir_stat.st_size);  //发送http头部

                int send_file_ret = sendfile(fd, open_fd, nullptr, dir_stat.st_size); //发送html文件
                if (send_file_ret == -1) {
                    cout << "工作线程send_file()(目录文件)失败" << endl;
                    perror("error"); exit(1);
                }

                cout << "工作线程向fd=" << fd << "发送目录文件成功" << endl;
                close(open_fd);  //关闭目录文件
            }
        }

        setnblock(fd); //恢复文件fd为非阻塞
    }  
}

void change_path() {  //改变程序当前目录
    char work_path[4096] = "";
    strcpy(work_path, getenv("PWD")); //获取当前工作目录
    strcat(work_path, "/resouce");  //拼接出资源路径
    chdir(work_path); //改变工作路径到资源目录下
}

void str_convert(char* to, char* from) {  //中文转码
    while (*from != '\0') {
        if (from[0] == '%' && isxdigit(from[1]) && isxdigit(from[2])) {
            *to = hexit(from[1]) * 16 + hexit(from[2]);  //char->0x
            from += 2;
        }
        else *to = *from;
        ++to;
        ++from;
    }
    *to = '\0';
}

int hexit(char c) {  //字符转16进制
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
}

void send_head_http(int fd, int flags, int length) {  //发送http头部
    int size = 0; char* buf = new char[512];
    memset(buf, '\0', 512);

    if (!flags) {  //flags == 0表示文件不存在
        size = sprintf(buf, "HTTP/1.1 404 Not Found\r\nContent-length: %d\r\nContent-Type: text/html; charset = gbk\r\n\r\n", length);
    }
    else { //flags == 1表示文件存在
        size = sprintf(buf, "HTTP/1.1 200 OK\r\nContent-length: %d\r\nContent-Type: text/html; charset = gbk\r\n\r\n", length);
    }

    if (size <= 0) {
        cout << "工作线程sprintf()失败" << endl;
        perror("error"); exit(1);
    }

    int send_ret = send(fd, buf, size, 0);//发送头部
    if (send_ret == -1) {
        cout << "工作线程send()(文件不存在)失败" << endl;
        perror("error"); exit(1);
    }

    cout << "工作线程向fd=" << fd << "发送http头部成功" << endl;
    delete[] buf;
}

void test_dir(char* str) { //测试目录名是否规范，将不规范的目录名修改正确 ./dirname->./dirname/
    while (*str != '\0') {
        ++str;
    }
    if (*(str - 1) != '/') {
        *str = '/';
        *(++str) = '\0';
    }
}