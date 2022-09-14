#ifndef TIME_WHEEL_H
#define TIME_WHEEL_H
#include <iostream>
#include <time.h>
using namespace std;

extern int KEEP_LIVE;

struct work_thread_info {  //与定时器绑定的线程信息
	pthread_t thread_id;  //线程id
	int cfd; //定时的客户端文件描述符
	work_thread_info(pthread_t id, int fd):thread_id(id), cfd(fd) {}
};

class timer {  //定时器类
public:
	timer(int r, int t, void (*f)(work_thread_info*), work_thread_info* d)
		:rotation(r), time_slot(t), prev(nullptr), next(nullptr), func(f), data(d), flags(1) {
	}
	~timer() {
		delete data;
	}
	int rotation; //记录该定时器在时间轮转多少圈后生效
	int time_slot; //记录该定时器在时间轮的第几个槽上
	void (*func)(work_thread_info*); //定时器到期时的回调函数
	work_thread_info* data; //与该定时器绑定的线程信息
	timer* prev; //指向前一个定时器
	timer* next; //指向后一个定时器
	int flags;  //标志定时器是否允许处理, 0:不处理, 1:处理
};

class timer_wheel {  //时间轮类，统一管理定时器类
	static const int Num = 60; //时间轮的槽数
	static const int SI = 1; //时间轮每si时间转动一格
	timer* slots[Num]; //定时器链表
	int cur_slot; //时间轮当前所属槽
public:
	timer_wheel() :cur_slot(0) {  //时间轮的构造函数
		for (int i = 0; i < Num; ++i) {
			slots[i] = nullptr;
		}
	}

	~timer_wheel() {  //时间轮的析构函数
		for (int i = 0; i < Num; ++i) {
			timer* temp = slots[i];
			while (temp) {
				slots[i] = temp->next;
				delete temp;
				temp = slots[i];
			}
		}
	}
	
	//创建新的定时器
	timer* add_timer(int timeout, pthread_t thread_id, int cfd, void (*function)(work_thread_info*)) { 
		int ticks = 0;  //滴答数，表示再经过多少的槽后触发该定时器
		if (timeout < SI) {
			ticks = 1;
		}
		else {
			ticks = timeout / SI;
		}

		int rotation = ticks / Num; //计算再经过多少圈出发的圈数
		int time_slot = (cur_slot + (ticks % Num)) % Num; //计算该插在那个槽上

		timer* ptimer = new timer(rotation, time_slot, function,   //创建定时器
						new work_thread_info(thread_id, cfd));

		return ptimer;
	}

	void del_timer(timer* ptimer) { //删除定时器
		int ts = ptimer->time_slot;

		if (ptimer == slots[ts]) {
			slots[ts] = slots[ts]->next;
			if (slots[ts]) {
				slots[ts]->prev = nullptr;
			}
		}

		else {
			ptimer->prev->next = ptimer->next;
			if (ptimer->next) {
				ptimer->next->prev = ptimer->prev;
			}
		}
	}

	void insert(timer* ptimer) {  //向时间轮中插入定时器
		int time_slot = ptimer->time_slot;
		if (!slots[time_slot]) { //所属槽上无定时器
			slots[time_slot] = ptimer;
		}
		else {
			ptimer->next = slots[time_slot];
			slots[time_slot]->prev = ptimer;
			slots[time_slot] = ptimer;
		}
	}

	void update(timer* p) { //刷新定时器
		del_timer(p); //将旧定时器从时间轮中删除

		int ticks = KEEP_LIVE / SI;  //滴答数，表示再经过多少的槽后触发该定时器

		int rotation = ticks / Num; //计算再经过多少圈出发的圈数
		int time_slot = (cur_slot + (ticks % Num)) % Num; //计算该插在那个槽上

		p->rotation = rotation;
		p->time_slot = time_slot;

		insert(p);
	}

	void tick() { //SI时间到期(即alarm()定时到期),时间轮转动一格
		timer* temp = slots[cur_slot];
		while (temp) {  //循环处理该槽内的定时器
			if (temp->rotation <= 0 && temp->flags) {  //已到期且允许处理
				temp->flags = 0; //标记为处理过
				temp->func(temp->data);  //调用定时器的回调函数
				temp = temp->next;
			}
			else {  //未到期或者已经处理过
				--temp->rotation;
				temp = temp->next;
			}
		}
		cur_slot = (++cur_slot) % Num;  //更新当前槽数
	}
};

#endif // !time_wheel_h