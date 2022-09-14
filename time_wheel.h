#ifndef TIME_WHEEL_H
#define TIME_WHEEL_H
#include <iostream>
#include <time.h>
using namespace std;

extern int KEEP_LIVE;

struct work_thread_info {  //�붨ʱ���󶨵��߳���Ϣ
	pthread_t thread_id;  //�߳�id
	int cfd; //��ʱ�Ŀͻ����ļ�������
	work_thread_info(pthread_t id, int fd):thread_id(id), cfd(fd) {}
};

class timer {  //��ʱ����
public:
	timer(int r, int t, void (*f)(work_thread_info*), work_thread_info* d)
		:rotation(r), time_slot(t), prev(nullptr), next(nullptr), func(f), data(d), flags(1) {
	}
	~timer() {
		delete data;
	}
	int rotation; //��¼�ö�ʱ����ʱ����ת����Ȧ����Ч
	int time_slot; //��¼�ö�ʱ����ʱ���ֵĵڼ�������
	void (*func)(work_thread_info*); //��ʱ������ʱ�Ļص�����
	work_thread_info* data; //��ö�ʱ���󶨵��߳���Ϣ
	timer* prev; //ָ��ǰһ����ʱ��
	timer* next; //ָ���һ����ʱ��
	int flags;  //��־��ʱ���Ƿ�������, 0:������, 1:����
};

class timer_wheel {  //ʱ�����࣬ͳһ����ʱ����
	static const int Num = 60; //ʱ���ֵĲ���
	static const int SI = 1; //ʱ����ÿsiʱ��ת��һ��
	timer* slots[Num]; //��ʱ������
	int cur_slot; //ʱ���ֵ�ǰ������
public:
	timer_wheel() :cur_slot(0) {  //ʱ���ֵĹ��캯��
		for (int i = 0; i < Num; ++i) {
			slots[i] = nullptr;
		}
	}

	~timer_wheel() {  //ʱ���ֵ���������
		for (int i = 0; i < Num; ++i) {
			timer* temp = slots[i];
			while (temp) {
				slots[i] = temp->next;
				delete temp;
				temp = slots[i];
			}
		}
	}
	
	//�����µĶ�ʱ��
	timer* add_timer(int timeout, pthread_t thread_id, int cfd, void (*function)(work_thread_info*)) { 
		int ticks = 0;  //�δ�������ʾ�پ������ٵĲۺ󴥷��ö�ʱ��
		if (timeout < SI) {
			ticks = 1;
		}
		else {
			ticks = timeout / SI;
		}

		int rotation = ticks / Num; //�����پ�������Ȧ������Ȧ��
		int time_slot = (cur_slot + (ticks % Num)) % Num; //����ò����Ǹ�����

		timer* ptimer = new timer(rotation, time_slot, function,   //������ʱ��
						new work_thread_info(thread_id, cfd));

		return ptimer;
	}

	void del_timer(timer* ptimer) { //ɾ����ʱ��
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

	void insert(timer* ptimer) {  //��ʱ�����в��붨ʱ��
		int time_slot = ptimer->time_slot;
		if (!slots[time_slot]) { //���������޶�ʱ��
			slots[time_slot] = ptimer;
		}
		else {
			ptimer->next = slots[time_slot];
			slots[time_slot]->prev = ptimer;
			slots[time_slot] = ptimer;
		}
	}

	void update(timer* p) { //ˢ�¶�ʱ��
		del_timer(p); //���ɶ�ʱ����ʱ������ɾ��

		int ticks = KEEP_LIVE / SI;  //�δ�������ʾ�پ������ٵĲۺ󴥷��ö�ʱ��

		int rotation = ticks / Num; //�����پ�������Ȧ������Ȧ��
		int time_slot = (cur_slot + (ticks % Num)) % Num; //����ò����Ǹ�����

		p->rotation = rotation;
		p->time_slot = time_slot;

		insert(p);
	}

	void tick() { //SIʱ�䵽��(��alarm()��ʱ����),ʱ����ת��һ��
		timer* temp = slots[cur_slot];
		while (temp) {  //ѭ������ò��ڵĶ�ʱ��
			if (temp->rotation <= 0 && temp->flags) {  //�ѵ�����������
				temp->flags = 0; //���Ϊ�����
				temp->func(temp->data);  //���ö�ʱ���Ļص�����
				temp = temp->next;
			}
			else {  //δ���ڻ����Ѿ������
				--temp->rotation;
				temp = temp->next;
			}
		}
		cur_slot = (++cur_slot) % Num;  //���µ�ǰ����
	}
};

#endif // !time_wheel_h