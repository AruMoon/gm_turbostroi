/*
 * ffi_api.cpp
 *
 *  Created on: 28 авг. 2022 г.
 *      Author: mr0maks
 */

#include "turbostroi_workers.h"

extern rn_thread_userdata* rn_userdata;


//extern "C" __declspec(dllexport)
extern "C" __attribute__ ((visibility ("default"), externally_visible)) bool ThreadSendMessage(void* p, int message, const char* system_name, const char* name, double index, double value) {
	bool successful = false;

	thread_userdata* userdata = (thread_userdata*)p;

	if (userdata) {
		thread_msg tmsg;
		tmsg.message = message;
		strncpy(tmsg.system_name, system_name, 63);
		tmsg.system_name[63] = 0;
		strncpy(tmsg.name, name, 63);
		tmsg.name[63] = 0;
		tmsg.index = index;
		tmsg.value = value;
		if (userdata->thread_to_sim.push(tmsg)) {
			successful = true;
		}
	}
	return successful;
}

//extern "C" __declspec(dllexport)
extern "C" __attribute__ ((visibility ("default"), externally_visible)) thread_msg ThreadRecvMessage(void* p) {
	thread_userdata* userdata = (thread_userdata*)p;
	thread_msg tmsg;
	//tmsg.message = NULL;
	if (userdata) {
		userdata->sim_to_thread.pop(tmsg);
	}
	return tmsg;
}

extern "C" __attribute__ ((visibility ("default"), externally_visible)) int ThreadReadAvailable(void* p) {
//extern "C" __declspec(dllexport) int ThreadReadAvailable(void* p) {
	thread_userdata* userdata = (thread_userdata*)p;
	return userdata->sim_to_thread.read_available();
}

//------------------------------------------------------------------------------
// RailNetwork sim thread API
//------------------------------------------------------------------------------

//extern "C" __declspec(dllexport)
extern "C" __attribute__ ((visibility ("default"), externally_visible)) bool RnThreadSendMessage(int ent_id, int id, const char* name, double value) {
	bool successful = false;

	if (rn_userdata) {
		rn_thread_msg tmsg;
		tmsg.ent_id = ent_id;
		tmsg.id = id;
		strncpy(tmsg.name, name, 63);
		tmsg.name[63] = 0;
		tmsg.value = value;
		if (rn_userdata->thread_to_sim.push(tmsg)) {
			successful = true;
		}
	}

	return successful;
}
