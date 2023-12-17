/*
 * turbostroi_workers.h
 *
 *  Created on: 28 авг. 2022 г.
 *      Author: mr0maks
 */

#pragma once

#include <lua.hpp>
#include <boost/lockfree/queue.hpp>
#include <boost/lockfree/spsc_queue.hpp>

extern double target_time;
extern double rate;

typedef struct {
	int message;
	char system_name[64];
	char name[64];
	double index;
	double value;
} thread_msg;

struct thread_userdata {
	double current_time;
	lua_State* L;
	int finished;

	boost::lockfree::spsc_queue<thread_msg> thread_to_sim, sim_to_thread;

	thread_userdata() : thread_to_sim(1024), sim_to_thread(1024) //256
	{
	}
};

typedef struct {
	int ent_id;
	int id;
	char name[64];
	double value;
} rn_thread_msg;

struct rn_thread_userdata {
	double current_time;
	lua_State* L;
	int finished;

	boost::lockfree::spsc_queue<rn_thread_msg> thread_to_sim, sim_to_thread;

	rn_thread_userdata() : thread_to_sim(256), sim_to_thread(256) //256
	{
	}
};

struct shared_message {
	char message[512];
};

void threadSimulation(thread_userdata* userdata);
void threadRailnetworkSimulation(rn_thread_userdata* userdata);
